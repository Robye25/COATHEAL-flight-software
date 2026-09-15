#include "coatheal/command_server.hpp"

#include <algorithm>
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

// A client gets this long to deliver its one command line, and this long
// again to take the reply. The server is single-threaded: without a bound,
// a peer that connects and goes quiet -- a link drop mid-command, a laptop
// that went to sleep, an `nc` left open -- would hold the listener for the
// rest of the flight and every later command (HEATERS_OFF included) would
// queue behind it, unanswered.
constexpr int kClientTimeoutMs = 5000;
// Longest command line accepted before the client is dropped.
constexpr std::size_t kMaxLineBytes = 4096;

#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
// A peer that reset the connection must not raise SIGPIPE in the flight
// process; EPIPE from send() is handled like any other write failure.
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Largest reply chunk: a STATUS reply fits one chunk, and a chunk still fits
// the 1 600 B share next to a live telemetry frame.
constexpr std::size_t kReplyChunkBytes = 600;
// Budget wait for a whole reply: under the ground station's 3 s default.
constexpr auto kReplyBudgetWait = std::chrono::milliseconds(2500);
constexpr auto kEmissionTail = std::chrono::milliseconds(50);

bool IsLoopback(const std::string& peer_ip) {
  return peer_ip.rfind("127.", 0) == 0 || peer_ip == "::1";
}

bool SendAll(int fd, const char* data, std::size_t size) {
  while (size > 0) {
#ifdef _WIN32
    const int sent = send(static_cast<SOCKET>(fd), data, static_cast<int>(size), 0);
#else
    const int sent = static_cast<int>(send(fd, data, size, kSendFlags));
#endif
    if (sent <= 0) {
      return false;
    }
    data += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

void ResetOnClose(int fd) {
#ifndef _WIN32
  const linger reset_on_close{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close));
#else
  (void)fd;
#endif
}

void SetClientTimeouts(int fd) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(kClientTimeoutMs);
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval tv{};
  tv.tv_sec = kClientTimeoutMs / 1000;
  tv.tv_usec = (kClientTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
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
#ifndef _WIN32
  // close() alone does not wake a thread blocked in accept() on Linux, and
  // Stop() would wait for it forever; shutdown() does.
  if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
#endif
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
  // One command per connection, exactly as docs/protocol.md promises: read
  // one line, reply once, return (the caller closes the socket). Every
  // ground-station client already opens a fresh connection per command;
  // bounding each connection this way is what keeps one stuck client from
  // wedging the command path for the whole flight.
  SetClientTimeouts(client_fd);

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
      return;  // peer closed, error, or the receive timeout expired
    }

    buffer.append(chunk, chunk + n);
    const std::size_t pos = buffer.find('\n');
    if (pos == std::string::npos) {
      if (buffer.size() > kMaxLineBytes) {
        return;  // no newline in 4 KiB: not a command line
      }
      continue;
    }

    std::string line = buffer.substr(0, pos);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    std::string response = "NACK,UNKNOWN,internal error";
    if (handler_) {
      response = handler_(line, peer_ip);
    }
    response.push_back('\n');

    if (budget_ == nullptr || (IsLoopback(peer_ip) && !pace_loopback_)) {
      SendAll(client_fd, response.data(), response.size());
    } else {
      SendPacedReply(client_fd, response);
    }
    return;  // replied once; the caller closes the connection
  }
}

bool CommandServer::SendPacedReply(int client_fd, const std::string& response) {
  const auto deadline = std::chrono::steady_clock::now() + kReplyBudgetWait;
  std::size_t offset = 0;
  bool first = true;
  while (offset < response.size()) {
    const std::size_t chunk = std::min(kReplyChunkBytes, response.size() - offset);
    // The ground station's charge for the exchange already holds the
    // headers of a one-segment reply; every later chunk is a segment of its
    // own. Each chunk holds the ground station's ACK of it, our reset if that
    // ACK is late, and the reset our kernel answers the late ACK with.
    const std::uint32_t headers =
        (first ? 0U : wire::kTcpHeaders + wire::kFrameOverhead) + wire::kPureAck;
    const std::uint32_t cost = static_cast<std::uint32_t>(chunk) + headers + 2 * wire::kReset;
    LinkBudget::Ticket ticket;
    if (!budget_->WaitHold(cost, LinkPriority::kCommandReply, deadline, &ticket)) {
      ResetOnClose(client_fd);
      return false;
    }
    // Gives up with a reset; answers already on their way draw resets from
    // our kernel for a while longer.
    const auto abort_reply = [&]() {
      wire::TcpState state;
      const bool known = wire::ReadTcpState(client_fd, &state);
      const auto release_at =
          std::chrono::steady_clock::now() + wire::kAbortTail +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              known ? state.srtt : std::chrono::microseconds(0));
      ResetOnClose(client_fd);
      budget_->ReleaseAt(ticket, release_at);
    };
    const std::chrono::milliseconds ack_deadline = wire::AckDeadline(client_fd);
    if (!SendAll(client_fd, response.data() + offset, chunk)) {
      abort_reply();
      return false;
    }
    // The chunk itself is on the wire; its ACK and the resets are not.
    budget_->ReleasePart(ticket, cost - 2 * wire::kReset - wire::kPureAck,
                         std::chrono::steady_clock::now());
    // Reset before Linux could retransmit the chunk (wire::kAckDeadline).
    // The ground station reads the reply and closes at once, so its ACK is
    // back within a round trip.
    if (!wire::WaitAllAcknowledged(client_fd, ack_deadline)) {
      abort_reply();
      return false;
    }
    budget_->Refund(ticket, 2 * wire::kReset);  // acknowledged: no resets
    budget_->ReleaseAt(ticket, std::chrono::steady_clock::now() + kEmissionTail);
    offset += chunk;
    first = false;
  }
  return true;
}

}  // namespace coatheal
