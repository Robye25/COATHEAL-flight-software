#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

#include "coatheal/link_budget.hpp"
#include "coatheal/telemetry_drain.hpp"

namespace coatheal {

// Most-recently-heard ground-station advertisement, populated by the discovery
// listener thread. Guarded by the client mutex.
struct GroundStationAdvert {
  std::string host;
  int telemetry_port = 0;
  int command_port = 0;
  int priority = 0;
  std::chrono::steady_clock::time_point last_seen{};
  bool valid = false;
};

class TelemetryClient : public FrameSender {
 public:
  TelemetryClient(std::string host,
                  int telemetry_port,
                  int command_port,
                  int reconnect_ms,
                  bool discovery_enabled,
                  int discovery_port,
                  std::string static_ground_ip,
                  std::string static_pi_ip,
                  int discovery_period_ms = 2000,
                  int rediscover_period_s = 30,
                  int failover_grace_s = 5,
                  int priority = 100);
  ~TelemetryClient() override;

  TelemetryClient(const TelemetryClient&) = delete;
  TelemetryClient& operator=(const TelemetryClient&) = delete;

  // The onboard share of the E-Link budget (docs/link-budget.md). Every
  // frame, connection attempt, beacon and hello reply is charged to it. Set
  // before Start(); without one nothing is limited (unit tests).
  void SetLinkBudget(LinkBudget* budget);

  // Starts the UDP discovery listener and the onboard-beacon sender. Safe to
  // call multiple times; subsequent calls are a no-op.
  void Start();
  // Stops both discovery threads and closes sockets. Idempotent.
  void Stop();

  SendStatus SendFrame(const std::string& line, LinkPriority priority,
                       std::chrono::steady_clock::time_point budget_deadline,
                       TelemetryAck* ack) override;
  bool is_connected() const override;
  // "z1" or "plain" for the open connection, "" while disconnected.
  std::string link_codec() const;
  // Frames whose ACK missed the deadline (each reset the connection).
  std::uint64_t ack_timeouts() const;

  void SetTransmitEnabled(bool enabled);
  bool transmit_enabled() const;

  // Radio-silence seams (redesign spec §9). `beacon_allowed()` is what the
  // beacon thread consults before every broadcast; `hello_reply_allowed()`
  // is what the listener consults before answering a legacy GS_HELLO. Both
  // are false whenever transmit is disabled, so a silent onboard originates
  // no datagram at all, and both are false while a telemetry connection is
  // up -- discovery has nothing left to find, and the link budget has no
  // bytes to spare for it. The counters let tests observe what the two send
  // helpers actually did without opening sockets.
  bool beacon_allowed() const;
  bool hello_reply_allowed() const;
  std::uint64_t beacons_sent() const { return beacons_sent_.load(); }
  std::uint64_t hello_replies_sent() const { return hello_replies_sent_.load(); }

  // Treat a successful command connection as authoritative evidence of the
  // ground-station return path. This is the plug-and-play fallback when UDP
  // broadcast discovery is blocked or unreliable on link-local Ethernet.
  void ObserveGroundStation(const std::string& host,
                            int telemetry_port,
                            int command_port,
                            int priority);

  std::string session_id() const;
  std::string current_host() const;

  // Parse a single discovery line (newline already stripped). Populates the
  // latest-GS advertisement if the line is a well-formed GS_BEACON. Returns
  // true if parsed, false otherwise. Exposed for unit tests so sockets are
  // not required.
  bool ProcessIncomingDiscoveryLine(const std::string& line,
                                    const std::string& sender_ip);

  // Snapshot of the latest heard GS advert (for tests/observability).
  GroundStationAdvert latest_gs() const;

  // Bytes held for one telemetry frame of `payload` bytes (newline included):
  // the segment; the ground station's TCP ACK of it (a frame of its own when
  // its kernel answers before the ACK line is written) and the reset our
  // kernel answers that ACK with if it arrives after we gave up; the ACK line
  // for `line` and our ACK of it (or the reset answering a late one); and our
  // reset if the ACK line does not come in time or is not the one expected.
  // No retransmission is budgeted because none can reach the wire (see
  // wire::kAckDeadline). Public so the budget tests use the same model.
  static std::uint32_t FrameCostBytes(const std::string& line, std::size_t payload);

 private:
  bool ConnectLocked();
  void NegotiateCodecLocked();
  // What a connection attempt can put on the wire, the HELLO exchange
  // included (docs/link-budget.md).
  std::uint32_t ConnectCostBytesLocked() const;
  void ReleaseConnectHoldLocked(std::chrono::steady_clock::time_point when);
  // When a charge covering a connection being reset now may be released:
  // after wire::kAbortTail plus the connection's round trip.
  std::chrono::steady_clock::time_point AbortTailLocked() const;
  void QuickAckLocked();
  void CloseLocked();
  // Close with a reset (SO_LINGER 0): nothing unacknowledged is
  // retransmitted after this, which is what keeps retransmissions off the
  // E-Link when an ACK misses its deadline.
  void AbortLocked();

  bool SendAllLocked(const std::string& payload);
  bool ReadLineLocked(std::string* line, int timeout_ms);
  static bool ParseAckLine(const std::string& line, TelemetryAck* ack);

  // Background threads.
  void DiscoveryListenerLoop();
  void BeaconSenderLoop();

  // Helpers used by both worker threads.
  int OpenDiscoverySocket();  // bound to 0.0.0.0:discovery_port, broadcast ok
  void SendOnboardBeacon(int fd);
  void SendOnboardHelloReply(int fd, const struct sockaddr_in& to, int to_len,
                             const std::string& nonce);

  // Select the best host to dial right now. Returns true if something is
  // available. Caller holds mu_.
  bool PickTargetHostLocked(std::string* host, int* tel_port, int* cmd_port);

  static std::string BuildSessionId();
  static std::string Hostname();

  std::string configured_host_;
  std::string active_host_;
  int telemetry_port_ = 0;
  int command_port_ = 0;
  int reconnect_ms_ = 2000;
  bool discovery_enabled_ = true;
  int discovery_port_ = 4100;
  std::string static_ground_ip_;
  std::string static_pi_ip_;
  int discovery_period_ms_ = 2000;
  int rediscover_period_s_ = 30;
  int failover_grace_s_ = 5;
  int priority_ = 100;

  LinkBudget* budget_ = nullptr;

  mutable std::mutex mu_;
  bool connected_ = false;
  bool codec_z1_ = false;
  std::uint64_t ack_timeouts_ = 0;
  bool ack_timeout_logged_ = false;
  bool oversize_logged_ = false;
  // Held from before the SYN until the HELLO exchange is over (or, for a
  // HELLO left unanswered, until the connection shows it never will be).
  LinkBudget::Ticket connect_hold_;
  bool connect_hold_open_ = false;
  bool transmit_enabled_ = true;
  std::atomic<std::uint64_t> beacons_sent_{0};
  std::atomic<std::uint64_t> hello_replies_sent_{0};
  int socket_fd_ = -1;
  std::string recv_buffer_;
  std::string session_id_;
  // Exponential connect backoff, expressed as a deadline instead of a
  // sleep: SendFrame runs on the control-loop thread, and sleeping there
  // stretched every tick by up to reconnect_ms_ whenever the ground station
  // was away.
  std::chrono::steady_clock::time_point next_connect_attempt_{};
  int connect_backoff_ms_ = 500;

  // Currently-connected GS metadata (copy of latest_gs_ snapshot at
  // connect-time) so we can detect strictly-higher-priority beacons.
  int current_priority_ = 0;
  std::string current_failover_candidate_host_;
  int current_failover_candidate_priority_ = 0;
  std::chrono::steady_clock::time_point current_failover_first_seen_{};
  // Set to true when a confirmed higher-priority GS has been observed twice
  // inside the failover grace window. The TCP client tears down the current
  // socket on the next send.
  std::atomic<bool> failover_requested_{false};

  GroundStationAdvert latest_gs_;

  // Worker threads and their shared shutdown flag.
  std::atomic<bool> running_{false};
  std::thread listener_thread_;
  std::thread beacon_thread_;
  std::condition_variable beacon_cv_;
  std::mutex beacon_mu_;
};

}  // namespace coatheal
