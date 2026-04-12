#include "coatheal/telemetry_client.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace coatheal {
namespace {

void CloseSocket(int* fd) {
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

bool SetSocketRecvTimeoutMs(int fd, int timeout_ms) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(timeout_ms);
  return setsockopt(static_cast<SOCKET>(fd),
                    SOL_SOCKET,
                    SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout),
                    sizeof(timeout)) == 0;
#else
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

std::vector<std::string> SplitCsv(const std::string& input) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream iss(input);
  while (std::getline(iss, token, ',')) {
    tokens.push_back(token);
  }
  return tokens;
}

}  // namespace

TelemetryClient::TelemetryClient(std::string host,
                                 int telemetry_port,
                                 int command_port,
                                 int reconnect_ms,
                                 bool discovery_enabled,
                                 int discovery_port,
                                 std::string static_ground_ip,
                                 std::string static_pi_ip)
    : configured_host_(std::move(host)),
      active_host_(configured_host_),
      telemetry_port_(telemetry_port),
      command_port_(command_port),
      reconnect_ms_(reconnect_ms),
      discovery_enabled_(discovery_enabled),
      discovery_port_(discovery_port),
      static_ground_ip_(std::move(static_ground_ip)),
      static_pi_ip_(std::move(static_pi_ip)),
      session_id_(BuildSessionId()) {
#ifdef _WIN32
  WSADATA wsa_data;
  WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

TelemetryClient::~TelemetryClient() {
  std::lock_guard<std::mutex> lock(mu_);
  CloseLocked();
#ifdef _WIN32
  WSACleanup();
#endif
}

bool TelemetryClient::ConnectLocked() {
  CloseLocked();

  active_host_ = configured_host_;
  int target_telemetry_port = telemetry_port_;
  int discovered_command_port = command_port_;

  std::string discovered_host;
  if (discovery_enabled_ &&
      DiscoverGroundHostLocked(&discovered_host, &target_telemetry_port, &discovered_command_port)) {
    active_host_ = discovered_host;
    command_port_ = discovered_command_port;
  } else if (!static_ground_ip_.empty()) {
    active_host_ = static_ground_ip_;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  const std::string port_str = std::to_string(target_telemetry_port);
  if (getaddrinfo(active_host_.c_str(), port_str.c_str(), &hints, &result) != 0 || result == nullptr) {
    connected_ = false;
    return false;
  }

  int fd = -1;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = static_cast<int>(socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
    if (fd < 0) {
      continue;
    }
    if (connect(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
      break;
    }
    CloseSocket(&fd);
  }

  freeaddrinfo(result);

  if (fd < 0) {
    connected_ = false;
    return false;
  }

  socket_fd_ = fd;
  connected_ = true;
  recv_buffer_.clear();
  return true;
}

void TelemetryClient::CloseLocked() {
  connected_ = false;
  recv_buffer_.clear();
  CloseSocket(&socket_fd_);
}

bool TelemetryClient::DiscoverGroundHostLocked(std::string* discovered_host,
                                               int* discovered_telemetry_port,
                                               int* discovered_command_port) {
  if (discovered_host == nullptr || discovered_telemetry_port == nullptr ||
      discovered_command_port == nullptr) {
    return false;
  }

  int udp_fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
  if (udp_fd < 0) {
    return false;
  }

  const int reuse = 1;
#ifdef _WIN32
  setsockopt(static_cast<SOCKET>(udp_fd),
             SOL_SOCKET,
             SO_REUSEADDR,
             reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
#else
  setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(static_cast<uint16_t>(discovery_port_));

  if (bind(udp_fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    CloseSocket(&udp_fd);
    return false;
  }

  if (!SetSocketRecvTimeoutMs(udp_fd, reconnect_ms_)) {
    CloseSocket(&udp_fd);
    return false;
  }

  char buffer[512];
  for (int attempt = 0; attempt < 3; ++attempt) {
    sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);
#ifdef _WIN32
    const int n = recvfrom(static_cast<SOCKET>(udp_fd),
                           buffer,
                           static_cast<int>(sizeof(buffer) - 1),
                           0,
                           reinterpret_cast<sockaddr*>(&sender),
                           &sender_len);
#else
    const int n = static_cast<int>(recvfrom(
        udp_fd, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &sender_len));
#endif
    if (n <= 0) {
      continue;
    }

    buffer[n] = '\0';
    std::string line(buffer);
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    const std::vector<std::string> tokens = SplitCsv(line);
    if (tokens.size() < 4 || tokens[0] != "GS_HELLO") {
      continue;
    }

    const std::string& nonce = tokens[1];

    int telemetry_port = telemetry_port_;
    int command_port = command_port_;
    try {
      telemetry_port = std::stoi(tokens[2]);
      command_port = std::stoi(tokens[3]);
    } catch (...) {
      continue;
    }

    char ip_buf[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &sender.sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
      continue;
    }

    std::ostringstream reply;
    reply << "ONBOARD_HELLO," << nonce << ',' << session_id_ << ',' << Hostname() << ','
          << command_port_ << ',' << telemetry_port_;
    const std::string reply_payload = reply.str();

#ifdef _WIN32
    sendto(static_cast<SOCKET>(udp_fd),
           reply_payload.c_str(),
           static_cast<int>(reply_payload.size()),
           0,
           reinterpret_cast<const sockaddr*>(&sender),
           sender_len);
#else
    sendto(udp_fd,
           reply_payload.c_str(),
           reply_payload.size(),
           0,
           reinterpret_cast<const sockaddr*>(&sender),
           sender_len);
#endif

    *discovered_host = ip_buf;
    *discovered_telemetry_port = telemetry_port;
    *discovered_command_port = command_port;
    CloseSocket(&udp_fd);
    return true;
  }

  CloseSocket(&udp_fd);
  return false;
}

bool TelemetryClient::SendAllLocked(const std::string& payload) {
  const char* ptr = payload.c_str();
  std::size_t remaining = payload.size();

  while (remaining > 0) {
#ifdef _WIN32
    const int sent = send(static_cast<SOCKET>(socket_fd_), ptr, static_cast<int>(remaining), 0);
#else
    const int sent = static_cast<int>(send(socket_fd_, ptr, remaining, 0));
#endif
    if (sent <= 0) {
      return false;
    }
    ptr += sent;
    remaining -= static_cast<std::size_t>(sent);
  }

  return true;
}

bool TelemetryClient::ReadLineLocked(std::string* line) {
  if (line == nullptr) {
    return false;
  }

  if (!SetSocketRecvTimeoutMs(socket_fd_, reconnect_ms_)) {
    return false;
  }

  std::size_t newline = recv_buffer_.find('\n');
  while (newline == std::string::npos) {
    char chunk[512];
#ifdef _WIN32
    const int n = recv(static_cast<SOCKET>(socket_fd_), chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const int n = static_cast<int>(recv(socket_fd_, chunk, sizeof(chunk), 0));
#endif
    if (n <= 0) {
      return false;
    }

    recv_buffer_.append(chunk, chunk + n);
    newline = recv_buffer_.find('\n');
  }

  *line = recv_buffer_.substr(0, newline);
  recv_buffer_.erase(0, newline + 1);
  if (!line->empty() && line->back() == '\r') {
    line->pop_back();
  }

  return true;
}

bool TelemetryClient::ParseAckLine(const std::string& line, TelemetryAck* ack) {
  if (ack == nullptr) {
    return false;
  }

  const std::vector<std::string> tokens = SplitCsv(line);
  if (tokens.size() != 3 || tokens[0] != "ACK") {
    return false;
  }

  try {
    ack->session_id = tokens[1];
    ack->seq = static_cast<std::uint64_t>(std::stoull(tokens[2]));
  } catch (...) {
    return false;
  }

  return !ack->session_id.empty();
}

bool TelemetryClient::SendFrameAwaitAck(const std::string& frame, TelemetryAck* ack) {
  std::lock_guard<std::mutex> lock(mu_);

  if (!transmit_enabled_) {
    // Radio silence: drop the socket quickly and refuse to transmit. Callers
    // keep the frame in the persistent queue for replay after RADIO_RESUME.
    if (socket_fd_ >= 0) {
      CloseLocked();
    }
    return false;
  }

  if (!connected_) {
    if (!ConnectLocked()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms_));
      return false;
    }
  }

  const std::string payload = frame + "\n";
  if (!SendAllLocked(payload)) {
    CloseLocked();
    return false;
  }

  std::string ack_line;
  if (!ReadLineLocked(&ack_line)) {
    CloseLocked();
    return false;
  }

  TelemetryAck parsed;
  if (!ParseAckLine(ack_line, &parsed)) {
    CloseLocked();
    return false;
  }

  if (ack != nullptr) {
    *ack = parsed;
  }

  return true;
}

bool TelemetryClient::is_connected() const {
  std::lock_guard<std::mutex> lock(mu_);
  return connected_;
}

void TelemetryClient::SetTransmitEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  transmit_enabled_ = enabled;
  if (!enabled) {
    CloseLocked();
  }
}

bool TelemetryClient::transmit_enabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return transmit_enabled_;
}

std::string TelemetryClient::session_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return session_id_;
}

std::string TelemetryClient::current_host() const {
  std::lock_guard<std::mutex> lock(mu_);
  return active_host_;
}

std::string TelemetryClient::BuildSessionId() {
  const auto unix_s = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();

  std::ostringstream oss;
  oss << Hostname() << '-' << unix_s << '-' << (mono_ns % 1000000);
  return oss.str();
}

std::string TelemetryClient::Hostname() {
  char buffer[128] = {0};
#ifdef _WIN32
  DWORD size = static_cast<DWORD>(sizeof(buffer));
  if (GetComputerNameA(buffer, &size) != 0) {
    return std::string(buffer, size);
  }
#else
  if (gethostname(buffer, sizeof(buffer) - 1) == 0) {
    return std::string(buffer);
  }
#endif
  return "coatheal-onboard";
}

}  // namespace coatheal




