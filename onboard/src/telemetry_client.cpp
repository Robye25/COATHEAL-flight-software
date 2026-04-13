#include "coatheal/telemetry_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
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

std::string TrimCrLf(std::string line) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  return line;
}

}  // namespace

TelemetryClient::TelemetryClient(std::string host,
                                 int telemetry_port,
                                 int command_port,
                                 int reconnect_ms,
                                 bool discovery_enabled,
                                 int discovery_port,
                                 std::string static_ground_ip,
                                 std::string static_pi_ip,
                                 int discovery_period_ms,
                                 int rediscover_period_s,
                                 int failover_grace_s,
                                 int priority)
    : configured_host_(std::move(host)),
      active_host_(configured_host_),
      telemetry_port_(telemetry_port),
      command_port_(command_port),
      reconnect_ms_(reconnect_ms),
      discovery_enabled_(discovery_enabled),
      discovery_port_(discovery_port),
      static_ground_ip_(std::move(static_ground_ip)),
      static_pi_ip_(std::move(static_pi_ip)),
      discovery_period_ms_(discovery_period_ms > 0 ? discovery_period_ms : 2000),
      rediscover_period_s_(rediscover_period_s > 0 ? rediscover_period_s : 30),
      failover_grace_s_(failover_grace_s > 0 ? failover_grace_s : 5),
      priority_(priority),
      session_id_(BuildSessionId()) {
#ifdef _WIN32
  WSADATA wsa_data;
  WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
}

TelemetryClient::~TelemetryClient() {
  Stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    CloseLocked();
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

void TelemetryClient::Start() {
  if (!discovery_enabled_) {
    return;
  }
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  listener_thread_ = std::thread(&TelemetryClient::DiscoveryListenerLoop, this);
  beacon_thread_ = std::thread(&TelemetryClient::BeaconSenderLoop, this);
}

void TelemetryClient::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(beacon_mu_);
  }
  beacon_cv_.notify_all();
  if (beacon_thread_.joinable()) {
    beacon_thread_.join();
  }
  if (listener_thread_.joinable()) {
    listener_thread_.join();
  }
}

int TelemetryClient::OpenDiscoverySocket() {
  int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    return -1;
  }

  const int one = 1;
#ifdef _WIN32
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&one), sizeof(one));
  setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char*>(&one), sizeof(one));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
#endif

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(static_cast<uint16_t>(discovery_port_));

  if (bind(fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    CloseSocket(&fd);
    return -1;
  }

  return fd;
}

void TelemetryClient::SendOnboardBeacon(int fd) {
  std::ostringstream oss;
  oss << "ONBOARD_BEACON," << session_id_ << ',' << Hostname() << ','
      << command_port_ << ',' << telemetry_port_;
  const std::string payload = oss.str();

  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(static_cast<uint16_t>(discovery_port_));
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

#ifdef _WIN32
  sendto(static_cast<SOCKET>(fd), payload.c_str(),
         static_cast<int>(payload.size()), 0,
         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#else
  sendto(fd, payload.c_str(), payload.size(), 0,
         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
}

void TelemetryClient::SendOnboardHelloReply(int fd, const sockaddr_in& to,
                                            int to_len, const std::string& nonce) {
  std::ostringstream oss;
  oss << "ONBOARD_HELLO," << nonce << ',' << session_id_ << ',' << Hostname()
      << ',' << command_port_ << ',' << telemetry_port_;
  const std::string payload = oss.str();

#ifdef _WIN32
  sendto(static_cast<SOCKET>(fd), payload.c_str(),
         static_cast<int>(payload.size()), 0,
         reinterpret_cast<const sockaddr*>(&to), to_len);
#else
  sendto(fd, payload.c_str(), payload.size(), 0,
         reinterpret_cast<const sockaddr*>(&to), static_cast<socklen_t>(to_len));
#endif
}

bool TelemetryClient::ProcessIncomingDiscoveryLine(const std::string& raw_line,
                                                   const std::string& sender_ip) {
  const std::string line = TrimCrLf(raw_line);
  const std::vector<std::string> tokens = SplitCsv(line);
  if (tokens.empty()) {
    return false;
  }

  if (tokens[0] == "GS_BEACON" && tokens.size() >= 5) {
    try {
      const int tel = std::stoi(tokens[2]);
      const int cmd = std::stoi(tokens[3]);
      const int prio = std::stoi(tokens[4]);

      std::lock_guard<std::mutex> lock(mu_);
      latest_gs_.host = sender_ip;
      latest_gs_.telemetry_port = tel;
      latest_gs_.command_port = cmd;
      latest_gs_.priority = prio;
      latest_gs_.last_seen = std::chrono::steady_clock::now();
      latest_gs_.valid = true;

      // Failover decision (only relevant while connected to a different host).
      if (connected_ && sender_ip != active_host_ && prio > current_priority_) {
        const auto now = std::chrono::steady_clock::now();
        if (current_failover_candidate_host_ == sender_ip &&
            current_failover_candidate_priority_ == prio) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - current_failover_first_seen_)
                                   .count();
          if (elapsed <= failover_grace_s_) {
            // Confirmed: same higher-priority host beaconed again inside the
            // grace window. Request failover; TCP path will tear down.
            failover_requested_.store(true);
            std::cerr << "[telemetry] failover confirmed: " << active_host_
                      << " -> " << sender_ip << " (priority " << current_priority_
                      << " -> " << prio << ")\n";
          }
        } else {
          current_failover_candidate_host_ = sender_ip;
          current_failover_candidate_priority_ = prio;
          current_failover_first_seen_ = now;
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  // Legacy pull-style hello: we must respond so older GS builds still work.
  // Return true so the caller knows to send the reply; however the reply is
  // sent by the listener loop (which has the socket + sender addr).
  if (tokens[0] == "GS_HELLO" && tokens.size() >= 4) {
    return true;
  }

  return false;
}

void TelemetryClient::DiscoveryListenerLoop() {
  while (running_.load()) {
    const int fd = OpenDiscoverySocket();
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms_));
      continue;
    }
    SetSocketRecvTimeoutMs(fd, 500);

    char buffer[1024];
    while (running_.load()) {
      sockaddr_in sender{};
      socklen_t sender_len = sizeof(sender);
#ifdef _WIN32
      const int n = recvfrom(static_cast<SOCKET>(fd), buffer,
                             static_cast<int>(sizeof(buffer) - 1), 0,
                             reinterpret_cast<sockaddr*>(&sender), &sender_len);
#else
      const int n = static_cast<int>(
          recvfrom(fd, buffer, sizeof(buffer) - 1, 0,
                   reinterpret_cast<sockaddr*>(&sender), &sender_len));
#endif
      if (!running_.load()) {
        break;
      }
      if (n <= 0) {
        continue;  // timeout — loop back and re-check running_
      }
      buffer[n] = '\0';

      char ip_buf[INET_ADDRSTRLEN] = {0};
      if (inet_ntop(AF_INET, &sender.sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
        continue;
      }
      const std::string sender_ip = ip_buf;
      const std::string line = TrimCrLf(std::string(buffer));

      const std::vector<std::string> tokens = SplitCsv(line);
      if (tokens.empty()) {
        continue;
      }

      if (tokens[0] == "GS_HELLO" && tokens.size() >= 4) {
        // Legacy path: reply with ONBOARD_HELLO. Also fold into latest_gs_
        // at priority 0 so the connect path has something to dial if this
        // is the only thing we ever hear.
        const std::string& nonce = tokens[1];
        SendOnboardHelloReply(fd, sender, static_cast<int>(sender_len), nonce);
        try {
          const int tel = std::stoi(tokens[2]);
          const int cmd = std::stoi(tokens[3]);
          std::lock_guard<std::mutex> lock(mu_);
          if (!latest_gs_.valid || latest_gs_.priority == 0) {
            latest_gs_.host = sender_ip;
            latest_gs_.telemetry_port = tel;
            latest_gs_.command_port = cmd;
            latest_gs_.priority = 0;
            latest_gs_.last_seen = std::chrono::steady_clock::now();
            latest_gs_.valid = true;
          }
        } catch (...) {
          // ignore bad ports
        }
        continue;
      }

      ProcessIncomingDiscoveryLine(line, sender_ip);
    }

    int local = fd;
    CloseSocket(&local);
  }
}

void TelemetryClient::BeaconSenderLoop() {
  const int fd = OpenDiscoverySocket();
  if (fd < 0) {
    return;
  }
  while (running_.load()) {
    bool connected_now;
    {
      std::lock_guard<std::mutex> lock(mu_);
      connected_now = connected_;
    }
    if (!connected_now) {
      SendOnboardBeacon(fd);
    }
    std::unique_lock<std::mutex> lock(beacon_mu_);
    beacon_cv_.wait_for(lock, std::chrono::milliseconds(discovery_period_ms_),
                        [this]() { return !running_.load(); });
  }
  int local = fd;
  CloseSocket(&local);
}

bool TelemetryClient::PickTargetHostLocked(std::string* host, int* tel_port,
                                           int* cmd_port) {
  const auto now = std::chrono::steady_clock::now();
  if (latest_gs_.valid) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                         now - latest_gs_.last_seen)
                         .count();
    if (age <= rediscover_period_s_) {
      *host = latest_gs_.host;
      *tel_port = latest_gs_.telemetry_port > 0 ? latest_gs_.telemetry_port
                                                : telemetry_port_;
      *cmd_port = latest_gs_.command_port > 0 ? latest_gs_.command_port
                                              : command_port_;
      current_priority_ = latest_gs_.priority;
      return true;
    }
  }
  if (!static_ground_ip_.empty()) {
    *host = static_ground_ip_;
    *tel_port = telemetry_port_;
    *cmd_port = command_port_;
    current_priority_ = 0;
    return true;
  }
  if (!configured_host_.empty()) {
    *host = configured_host_;
    *tel_port = telemetry_port_;
    *cmd_port = command_port_;
    current_priority_ = 0;
    return true;
  }
  return false;
}

bool TelemetryClient::ConnectLocked() {
  CloseLocked();

  std::string host;
  int tel_port = telemetry_port_;
  int cmd_port = command_port_;
  if (!PickTargetHostLocked(&host, &tel_port, &cmd_port)) {
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  const std::string port_str = std::to_string(tel_port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 ||
      result == nullptr) {
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
  active_host_ = host;
  command_port_ = cmd_port;
  recv_buffer_.clear();
  current_failover_candidate_host_.clear();
  current_failover_candidate_priority_ = 0;
  failover_requested_.store(false);
  return true;
}

void TelemetryClient::CloseLocked() {
  connected_ = false;
  recv_buffer_.clear();
  CloseSocket(&socket_fd_);
}

bool TelemetryClient::SendAllLocked(const std::string& payload) {
  const char* ptr = payload.c_str();
  std::size_t remaining = payload.size();

  while (remaining > 0) {
#ifdef _WIN32
    const int sent = send(static_cast<SOCKET>(socket_fd_), ptr,
                          static_cast<int>(remaining), 0);
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
    const int n = recv(static_cast<SOCKET>(socket_fd_), chunk,
                       static_cast<int>(sizeof(chunk)), 0);
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

bool TelemetryClient::SendFrameAwaitAck(const std::string& frame,
                                        TelemetryAck* ack) {
  // Always-on reconnect: we loop until we get a successful send+ack, OR the
  // caller disables transmit (radio silence) — in which case we return false
  // without burning cycles. The outer queue drain will retry on the next
  // tick, so there is no risk of deadlocking the main loop; each call is
  // still bounded by a single exponential-backoff wait on connect failure.
  std::lock_guard<std::mutex> lock(mu_);

  if (!transmit_enabled_) {
    if (socket_fd_ >= 0) {
      CloseLocked();
    }
    return false;
  }

  // Honor pending failover request before doing anything else.
  if (failover_requested_.exchange(false) && connected_) {
    std::cerr << "[telemetry] tearing down TCP for GS failover\n";
    CloseLocked();
  }

  if (!connected_) {
    if (!ConnectLocked()) {
      // Exponential backoff 0.5s -> 1s -> 2s -> ... capped at reconnect_ms_.
      static thread_local int backoff_ms = 500;
      const int cap = std::max(500, reconnect_ms_);
      const int wait_ms = std::min(backoff_ms, cap);
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
      backoff_ms = std::min(backoff_ms * 2, cap);
      return false;
    }
    // Reset backoff on successful connect by re-reading the thread-local.
    static thread_local int reset_backoff = 500;
    reset_backoff = 500;
    (void)reset_backoff;
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

GroundStationAdvert TelemetryClient::latest_gs() const {
  std::lock_guard<std::mutex> lock(mu_);
  return latest_gs_;
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
