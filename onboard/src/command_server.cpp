#include "coatheal/command_server.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace coatheal {
namespace {

void CloseFd(int* fd) {
  if (*fd < 0) {
    return;
  }
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(*fd));
#else
  close(*fd);
#endif
  *fd = -1;
}

}  // namespace

CommandServer::CommandServer(int port) : port_(port) {
#ifdef _WIN32
  WSADATA wsa_data;
  WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

CommandServer::~CommandServer() {
  Stop();
#ifdef _WIN32
  WSACleanup();
#endif
}

bool CommandServer::Start(Handler handler, std::string* error) {
  if (running_) {
    if (error != nullptr) {
      *error = "command server already running";
    }
    return false;
  }
  handler_ = std::move(handler);
  running_ = true;
  thread_ = std::thread(&CommandServer::RunLoop, this);
  return true;
}

void CommandServer::CloseListenSocket() {
  CloseFd(&listen_fd_);
}

void CommandServer::Stop() {
  running_ = false;
  CloseListenSocket();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void CommandServer::RunLoop() {
  while (running_) {
    listen_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    const int opt = 1;
#ifdef _WIN32
    setsockopt(static_cast<SOCKET>(listen_fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr)) != 0) {
      CloseListenSocket();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    if (listen(listen_fd_, 4) != 0) {
      CloseListenSocket();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    while (running_) {
      sockaddr_in client_addr{};
      socklen_t client_len = sizeof(client_addr);
      int client_fd = static_cast<int>(accept(
          listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len));
      if (client_fd < 0) {
        if (!running_) {
          break;
        }
        continue;
      }

      char ip_buf[INET_ADDRSTRLEN] = {0};
      std::string peer_ip;
      if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf,
                    sizeof(ip_buf)) != nullptr) {
        peer_ip = ip_buf;
      }

      HandleClient(client_fd, peer_ip);
      CloseFd(&client_fd);
    }
    CloseListenSocket();
  }
}

void CommandServer::HandleClient(int client_fd, const std::string& peer_ip) {
  std::string buffer;
  buffer.reserve(1024);

  char chunk[512];
  while (running_) {
#ifdef _WIN32
    const int n = recv(static_cast<SOCKET>(client_fd), chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const int n = static_cast<int>(recv(client_fd, chunk, sizeof(chunk), 0));
#endif
    if (n <= 0) {
      break;
    }

    buffer.append(chunk, chunk + n);

    std::size_t pos = 0;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      std::string response = "NACK,UNKNOWN,internal error";
      if (handler_) {
        response = handler_(line, peer_ip);
      }
      response.push_back('\n');

      const char* ptr = response.c_str();
      std::size_t remain = response.size();
      while (remain > 0) {
#ifdef _WIN32
        const int sent = send(static_cast<SOCKET>(client_fd), ptr, static_cast<int>(remain), 0);
#else
        const int sent = static_cast<int>(send(client_fd, ptr, remain, 0));
#endif
        if (sent <= 0) {
          return;
        }
        ptr += sent;
        remain -= static_cast<std::size_t>(sent);
      }
    }
  }
}

}  // namespace coatheal
