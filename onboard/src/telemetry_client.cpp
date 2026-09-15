#include "coatheal/telemetry_client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "coatheal/telemetry_codec.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
// A ground station that reset the connection must not raise SIGPIPE in the
// flight process; EPIPE from send() is handled like any other send failure.
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool SetSocketSendTimeoutMs(int fd, int timeout_ms) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(timeout_ms);
  return setsockopt(static_cast<SOCKET>(fd),
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout),
                    sizeof(timeout)) == 0;
#else
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
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

bool SetSocketNonBlocking(int fd, bool enabled) {
#ifdef _WIN32
  u_long mode = enabled ? 1UL : 0UL;
  return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return fcntl(fd, F_SETFL, next) == 0;
#endif
}

bool ConnectWithTimeout(int fd,
                        const sockaddr* addr,
                        socklen_t addr_len,
                        int timeout_ms) {
  if (!SetSocketNonBlocking(fd, true)) {
    return connect(fd, addr, static_cast<int>(addr_len)) == 0;
  }

  bool connected = false;
  const int rc = connect(fd, addr, static_cast<int>(addr_len));
  if (rc == 0) {
    connected = true;
  } else {
#ifdef _WIN32
    const int err = WSAGetLastError();
    const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
    const bool in_progress = (errno == EINPROGRESS);
#endif
    if (in_progress) {
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(fd, &wfds);
      timeval tv{};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      const int ready = select(fd + 1, nullptr, &wfds, nullptr, &tv);
      if (ready > 0 && FD_ISSET(fd, &wfds)) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&so_error), &len) == 0) {
          connected = (so_error == 0);
        }
      }
    }
  }

  SetSocketNonBlocking(fd, false);
  return connected;
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

// How long after a charge's last byte our own delayed ACK can still leave
// (Linux delays a pure ACK by up to 40 ms). Holds are released this far in
// the future so that ACK is still covered.
constexpr auto kEmissionTail = std::chrono::milliseconds(50);
// The same once TCP_QUICKACK has pushed our ACK out right after the read.
constexpr auto kAckFlushTail = std::chrono::milliseconds(10);
// A ground station from before the codec never answers the HELLO; the HELLO
// segment itself is acknowledged by its kernel within the ACK deadline.
constexpr int kHelloTimeoutMs = 1500;
// The longest answer to the HELLO that is charged ("HELLO,z1\n", or
// "HELLO,plain\n" from a ground station without the dictionary).
constexpr std::size_t kHelloReplyMaxBytes = 16;
// Below the kernel's 1 s initial SYN timeout, so a connect attempt never
// retransmits its SYN.
constexpr int kConnectTimeoutMs = 900;

// The ACK line a conforming ground station answers `line` with, without the
// newline: DATA lines carry their own seq, EVT lines are acknowledged with 0.
// Empty for a line of unknown shape.
std::string ExpectedAckLine(const std::string& line) {
  const std::vector<std::string> tokens = SplitCsv(line);
  if (tokens.size() >= 3 && tokens[0] == "DATA") {
    return "ACK," + tokens[1] + "," + tokens[2];
  }
  if (tokens.size() >= 3 && tokens[0] == "EVT") {
    return "ACK," + tokens[2] + ",0";
  }
  return {};
}

std::size_t AckLinePayload(const std::string& line) {
  const std::string expected = ExpectedAckLine(line);
  if (!expected.empty()) return expected.size() + 1;
  return 4 + 64 + 1 + 20 + 1;  // unknown shape: the longest ACK we accept
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

void TelemetryClient::SetLinkBudget(LinkBudget* budget) {
  std::lock_guard<std::mutex> lock(mu_);
  budget_ = budget;
}

std::uint32_t TelemetryClient::FrameCostBytes(const std::string& line, std::size_t payload) {
  return wire::TcpBytes(payload) +                                // the frame
         wire::kPureAck + wire::kReset +                          // its TCP ACK alone, and
                                                                  // our reset if that is late
         wire::TcpBytes(AckLinePayload(line)) + wire::kPureAck +  // the ACK line, our ACK
                                                                  // (or reset) of it
         wire::kReset;                                            // our reset of the exchange
}

std::uint32_t TelemetryClient::ConnectCostBytesLocked() const {
  // SYN, SYN-ACK and our ACK; the HELLO line, the ground station's ACK of it
  // and the reset our kernel answers that ACK with if it comes after we gave
  // up; the answer and our ACK (or reset) of it; and our own reset. Nothing
  // is retransmitted: the connect times out before the SYN would be, and an
  // unacknowledged HELLO resets the connection.
  const std::string hello = "HELLO," + session_id_ + ",z1:00000000\n";
  return 2 * wire::kSyn + wire::kPureAck + wire::TcpBytes(hello.size()) + wire::kPureAck +
         wire::kReset + wire::TcpBytes(kHelloReplyMaxBytes) + wire::kPureAck + wire::kReset;
}

void TelemetryClient::ReleaseConnectHoldLocked(std::chrono::steady_clock::time_point when) {
  if (budget_ != nullptr && connect_hold_open_) budget_->ReleaseAt(connect_hold_, when);
  connect_hold_open_ = false;
}

std::chrono::steady_clock::time_point TelemetryClient::AbortTailLocked() const {
  wire::TcpState state;
  const auto srtt = wire::ReadTcpState(socket_fd_, &state) ? state.srtt : std::chrono::microseconds(0);
  return std::chrono::steady_clock::now() + wire::kAbortTail +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(srtt);
}

void TelemetryClient::QuickAckLocked() {
#if defined(__linux__) && defined(TCP_QUICKACK)
  // Set before a read, received data is ACKed on arrival; set after it, a
  // delayed ACK still pending leaves now.
  const int quickack = 1;
  setsockopt(socket_fd_, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
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
  if (budget_ != nullptr &&
      !budget_->TryCharge(wire::UdpBytes(payload.size()), LinkPriority::kDiscovery)) {
    return;  // no room this period; the next beacon is two seconds away
  }

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
  beacons_sent_.fetch_add(1);
}

void TelemetryClient::SendOnboardHelloReply(int fd, const sockaddr_in& to,
                                            int to_len, const std::string& nonce) {
  std::ostringstream oss;
  oss << "ONBOARD_HELLO," << nonce << ',' << session_id_ << ',' << Hostname()
      << ',' << command_port_ << ',' << telemetry_port_;
  const std::string payload = oss.str();
  if (budget_ != nullptr &&
      !budget_->TryCharge(wire::UdpBytes(payload.size()), LinkPriority::kDiscovery)) {
    return;
  }

#ifdef _WIN32
  sendto(static_cast<SOCKET>(fd), payload.c_str(),
         static_cast<int>(payload.size()), 0,
         reinterpret_cast<const sockaddr*>(&to), to_len);
#else
  sendto(fd, payload.c_str(), payload.size(), 0,
         reinterpret_cast<const sockaddr*>(&to), static_cast<socklen_t>(to_len));
#endif
  hello_replies_sent_.fetch_add(1);
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
        // Radio silence (redesign spec §9): never answer while silent, but
        // still remember who asked so RADIO_RESUME can dial them.
        if (hello_reply_allowed()) {
          SendOnboardHelloReply(fd, sender, static_cast<int>(sender_len), nonce);
        }
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
    // Radio silence (redesign spec §9): a silent onboard must not announce
    // itself either. beacon_allowed() folds transmit_enabled_ into the
    // "not connected" condition this loop used to check on its own.
    if (beacon_allowed()) {
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
  // Every path that clears connected_ has closed the socket already; the
  // connect hold placed for this attempt is released by the paths below.
  connected_ = false;
  codec_z1_ = false;
  recv_buffer_.clear();
  CloseSocket(&socket_fd_);

  std::string host;
  int tel_port = telemetry_port_;
  int cmd_port = command_port_;
  if (!PickTargetHostLocked(&host, &tel_port, &cmd_port)) {
    ReleaseConnectHoldLocked(std::chrono::steady_clock::now());  // nothing sent
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
    ReleaseConnectHoldLocked(std::chrono::steady_clock::now());  // nothing sent
    return false;
  }

  int fd = -1;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = static_cast<int>(socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
    if (fd < 0) {
      continue;
    }
    const int timeout_ms = std::max(100, std::min(reconnect_ms_, kConnectTimeoutMs));
    if (ConnectWithTimeout(fd, rp->ai_addr,
                           static_cast<socklen_t>(rp->ai_addrlen),
                           timeout_ms)) {
      break;
    }
    CloseSocket(&fd);
  }

  freeaddrinfo(result);

  if (fd < 0) {
    connected_ = false;
    // A SYN-ACK still on its way would draw a reset from our kernel.
    ReleaseConnectHoldLocked(std::chrono::steady_clock::now() + wire::kAbortTail);
    return false;
  }

  socket_fd_ = fd;
  connected_ = true;
  // The control loop sends from under mu_: a ground station that stops
  // reading must time the send out, not block the tick (and the watchdog).
  SetSocketSendTimeoutMs(socket_fd_, std::max(500, reconnect_ms_));
  active_host_ = host;
  command_port_ = cmd_port;
  recv_buffer_.clear();
  current_failover_candidate_host_.clear();
  current_failover_candidate_priority_ = 0;
  failover_requested_.store(false);
  oversize_logged_ = false;
  NegotiateCodecLocked();
  return connected_;
}

void TelemetryClient::NegotiateCodecLocked() {
  // docs/link-budget.md "Telemetry framing": offer z1 with the dictionary's
  // CRC; only an identical dictionary on the ground may switch it on. A
  // ground station from before the codec never answers the HELLO (it logs
  // one unparseable line), and the link simply stays plain.
  codec_z1_ = false;
  if (!TelemetryCodecAvailable()) {
    ReleaseConnectHoldLocked(std::chrono::steady_clock::now() + kEmissionTail);
    return;
  }
  const std::string hello =
      "HELLO," + session_id_ + ",z1:" + TelemetryDictionaryZ1CrcHex() + "\n";
  if (!SendAllLocked(hello) ||
      !wire::WaitAllAcknowledged(socket_fd_, wire::AckDeadline(socket_fd_))) {
    AbortLocked();  // releases the connect hold with the abort tail
    return;
  }
  std::string reply;
  if (!ReadLineLocked(&reply, kHelloTimeoutMs)) {
    // Silence is the old ground station's answer, not a dead link. A slow
    // ground station may still answer, though: that answer and our ACK of
    // it stay charged until a frame's ACK shows the HELLO was passed over,
    // or the connection closes.
    if (budget_ != nullptr && connect_hold_open_) {
      const std::uint32_t late_answer = wire::TcpBytes(kHelloReplyMaxBytes) + wire::kPureAck;
      budget_->ReleasePart(connect_hold_, ConnectCostBytesLocked() - late_answer,
                           std::chrono::steady_clock::now() + kEmissionTail);
    }
    return;
  }
  QuickAckLocked();
  codec_z1_ = (reply == "HELLO,z1");
  ReleaseConnectHoldLocked(std::chrono::steady_clock::now() + kAckFlushTail);
}

void TelemetryClient::CloseLocked() {
  // Anything the ground station still had on its way for this connection
  // draws resets from our kernel (docs/link-budget.md).
  if (connect_hold_open_) ReleaseConnectHoldLocked(AbortTailLocked());
  connected_ = false;
  codec_z1_ = false;
  recv_buffer_.clear();
  CloseSocket(&socket_fd_);
}

void TelemetryClient::AbortLocked() {
  if (socket_fd_ >= 0) {
#ifndef _WIN32
    const linger reset_on_close{1, 0};
    setsockopt(socket_fd_, SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close));
#endif
  }
  CloseLocked();
}

bool TelemetryClient::SendAllLocked(const std::string& payload) {
  const char* ptr = payload.c_str();
  std::size_t remaining = payload.size();

  while (remaining > 0) {
#ifdef _WIN32
    const int sent = send(static_cast<SOCKET>(socket_fd_), ptr,
                          static_cast<int>(remaining), 0);
#else
    const int sent =
        static_cast<int>(send(socket_fd_, ptr, remaining, kSendFlags));
#endif
    if (sent <= 0) {
      return false;
    }
    ptr += sent;
    remaining -= static_cast<std::size_t>(sent);
  }

  return true;
}

bool TelemetryClient::ReadLineLocked(std::string* line, int timeout_ms) {
  if (line == nullptr) {
    return false;
  }

  // One deadline for the whole line, however it is split into segments.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::size_t newline = recv_buffer_.find('\n');
  while (newline == std::string::npos) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    // SO_RCVTIMEO 0 would mean "wait forever".
    if (remaining.count() <= 0 ||
        !SetSocketRecvTimeoutMs(socket_fd_, static_cast<int>(remaining.count()))) {
      return false;
    }
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

SendStatus TelemetryClient::SendFrame(const std::string& line, LinkPriority priority,
                                      std::chrono::steady_clock::time_point budget_deadline,
                                      TelemetryAck* ack) {
  // The budget wait happens without the client mutex, so a command that
  // retargets the link (ObserveGroundStation) is never stuck behind a frame
  // waiting for room.
  std::unique_lock<std::mutex> lock(mu_);

  if (!transmit_enabled_) {
    if (socket_fd_ >= 0) {
      AbortLocked();
    }
    return SendStatus::kSilent;
  }

  // Honor pending failover request before doing anything else.
  if (failover_requested_.exchange(false) && connected_) {
    std::cerr << "[telemetry] tearing down TCP for GS failover\n";
    AbortLocked();
  }

  if (!connected_) {
    // Exponential backoff 0.5s -> 1s -> 2s -> ... capped at reconnect_ms_,
    // enforced as a deadline rather than a sleep so the caller (the control
    // loop) returns immediately and keeps ticking at rate.
    const auto now = std::chrono::steady_clock::now();
    if (now < next_connect_attempt_) {
      return SendStatus::kNotConnected;
    }
    // ConnectLocked releases the hold on every path, or keeps the part for a
    // HELLO answer that may still come (NegotiateCodecLocked).
    if (budget_ != nullptr) {
      ReleaseConnectHoldLocked(std::chrono::steady_clock::now() + wire::kAbortTail);  // none left open
      if (!budget_->TryHold(ConnectCostBytesLocked(), priority, &connect_hold_)) {
        return SendStatus::kNoBudget;
      }
      connect_hold_open_ = true;
    }
    if (!ConnectLocked()) {
      const int cap = std::max(500, reconnect_ms_);
      const int wait_ms = std::min(connect_backoff_ms_, cap);
      next_connect_attempt_ = now + std::chrono::milliseconds(wait_ms);
      connect_backoff_ms_ = std::min(connect_backoff_ms_ * 2, cap);
      return SendStatus::kNotConnected;
    }
    connect_backoff_ms_ = 500;
    next_connect_attempt_ = {};
  }

  const bool z1 = codec_z1_;
  const int fd = socket_fd_;
  std::string payload = z1 ? EncodeTelemetryLineZ1(line) : line;
  if (payload.empty()) payload = line;  // the ground inflates Z1 lines and passes plain ones
  payload.push_back('\n');
  const std::uint32_t cost = FrameCostBytes(line, payload.size());

  LinkBudget::Ticket ticket;
  if (budget_ != nullptr) {
    if (cost > budget_->share_bytes()) {
      // Only a plain DATA line is this large: the ground station did not
      // take the codec, and uncompressed frames cannot fit the budget.
      if (!oversize_logged_) {
        std::cerr << "[telemetry] a " << payload.size() << "-byte plain frame needs " << cost
                  << " B of the " << budget_->share_bytes()
                  << "-B link share; the ground station must answer HELLO,z1"
                  << " (docs/link-budget.md)\n";
        oversize_logged_ = true;
      }
      return SendStatus::kNoBudget;
    }
    lock.unlock();
    const bool admitted = budget_->WaitHold(cost, priority, budget_deadline, &ticket);
    lock.lock();
    if (!admitted) return SendStatus::kNoBudget;
    if (!connected_ || socket_fd_ != fd || codec_z1_ != z1 || !transmit_enabled_) {
      // The link changed while we waited: nothing was sent.
      budget_->Refund(ticket, cost);
      budget_->Release(ticket);
      return transmit_enabled_ ? SendStatus::kNotConnected : SendStatus::kSilent;
    }
  }

  // Gives up on the exchange with a reset. The charge stays open while
  // answers already on their way can still draw resets from our kernel; the
  // reset for a late TCP ACK is refunded when the frame is acknowledged.
  const auto abort_exchange = [&]() {
    wire::TcpState state;
    if (budget_ != nullptr && wire::ReadTcpState(socket_fd_, &state) && state.unacked == 0) {
      budget_->Refund(ticket, wire::kReset);
    }
    const auto release_at = AbortTailLocked();
    AbortLocked();
    if (budget_ != nullptr) budget_->ReleaseAt(ticket, release_at);
  };

  // The segment counters show whether the ground station's TCP ACK of the
  // frame came as a frame of its own (see the refund below).
  wire::TcpState before;
  const bool counted = recv_buffer_.empty() && wire::ReadTcpState(fd, &before);
  const std::chrono::milliseconds ack_deadline = wire::AckDeadline(fd);
  if (!SendAllLocked(payload)) {
    abort_exchange();
    return SendStatus::kFailed;
  }
  // The segment is on the wire now and cannot be retransmitted (see
  // wire::kAckDeadline): only what it provokes stays held.
  if (budget_ != nullptr) {
    budget_->ReleasePart(ticket, wire::TcpBytes(payload.size()), std::chrono::steady_clock::now());
  }

  QuickAckLocked();
  std::string ack_line;
  if (!ReadLineLocked(&ack_line, static_cast<int>(ack_deadline.count()))) {
    // Reset before the kernel could retransmit; the reset is part of the
    // charge.
    if (!ack_timeout_logged_) {
      std::cerr << "[telemetry] no ACK within " << ack_deadline.count()
                << " ms; connection reset so nothing is retransmitted\n";
      ack_timeout_logged_ = true;
    }
    ++ack_timeouts_;
    abort_exchange();
    return SendStatus::kFailed;
  }
  ack_timeout_logged_ = false;

  TelemetryAck parsed;
  const std::string expected_ack = ExpectedAckLine(line);
  if (!ParseAckLine(ack_line, &parsed) ||
      (!expected_ack.empty() &&
       "ACK," + parsed.session_id + "," + std::to_string(parsed.seq) != expected_ack)) {
    // Not the answer to this frame: the stream is out of step, and the real
    // answer may still be coming. Reset.
    abort_exchange();
    return SendStatus::kFailed;
  }
  QuickAckLocked();
  const auto acked_at = std::chrono::steady_clock::now();
  if (budget_ != nullptr) {
    // Answered in time: no reset of ours, and the answer carried the frame's
    // TCP ACK, so no late one can draw a reset either.
    budget_->Refund(ticket, 2 * wire::kReset);
    // One data segment in since the send: the answer itself carried that
    // ACK, and no pure ACK went out for the frame.
    wire::TcpState after;
    if (counted && wire::ReadTcpState(fd, &after) &&
        after.segments_in - before.segments_in == 1 &&
        after.data_segments_in - before.data_segments_in == 1) {
      budget_->Refund(ticket, wire::kPureAck);
    }
    budget_->ReleaseAt(ticket, acked_at + kAckFlushTail);
  }
  // The ground station answered a frame sent after the HELLO, so it passed
  // over the HELLO and will not answer it any more.
  ReleaseConnectHoldLocked(acked_at + kAckFlushTail);

  if (ack != nullptr) {
    *ack = parsed;
  }

  return SendStatus::kSent;
}

bool TelemetryClient::is_connected() const {
  std::lock_guard<std::mutex> lock(mu_);
  return connected_;
}

void TelemetryClient::SetTransmitEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  transmit_enabled_ = enabled;
  if (!enabled) {
    AbortLocked();
  }
}

void TelemetryClient::ObserveGroundStation(const std::string& host,
                                           int telemetry_port,
                                           int command_port,
                                           int priority) {
  if (host.empty() || host == "0.0.0.0") {
    return;
  }

  std::lock_guard<std::mutex> lock(mu_);
  latest_gs_.host = host;
  latest_gs_.telemetry_port = telemetry_port > 0 ? telemetry_port : telemetry_port_;
  latest_gs_.command_port = command_port > 0 ? command_port : command_port_;
  latest_gs_.priority = priority;
  latest_gs_.last_seen = std::chrono::steady_clock::now();
  latest_gs_.valid = true;

  if (connected_ && host != active_host_ && priority >= current_priority_) {
    AbortLocked();
  }
  // A lower-priority peer never displaces a known target, connected or not
  // -- a transient send failure must not let a loopback bench command
  // redirect telemetry away from the real ground station.
  if (!connected_ && (active_host_.empty() || priority >= current_priority_)) {
    active_host_ = host;
    current_priority_ = priority;
    // A ground station just made itself known; don't make it wait out a
    // backoff deadline accrued while nobody was there.
    connect_backoff_ms_ = 500;
    next_connect_attempt_ = {};
  }
}

bool TelemetryClient::transmit_enabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return transmit_enabled_;
}

bool TelemetryClient::beacon_allowed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return transmit_enabled_ && !connected_;
}

bool TelemetryClient::hello_reply_allowed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return transmit_enabled_ && !connected_;
}

std::uint64_t TelemetryClient::ack_timeouts() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ack_timeouts_;
}

std::string TelemetryClient::link_codec() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!connected_) return {};
  return codec_z1_ ? "z1" : "plain";
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
