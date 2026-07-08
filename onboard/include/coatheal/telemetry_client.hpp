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

namespace coatheal {

struct TelemetryAck {
  std::string session_id;
  std::uint64_t seq = 0;
};

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

class TelemetryClient {
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
  ~TelemetryClient();

  TelemetryClient(const TelemetryClient&) = delete;
  TelemetryClient& operator=(const TelemetryClient&) = delete;

  // Starts the UDP discovery listener and the onboard-beacon sender. Safe to
  // call multiple times; subsequent calls are a no-op.
  void Start();
  // Stops both discovery threads and closes sockets. Idempotent.
  void Stop();

  bool SendFrameAwaitAck(const std::string& frame, TelemetryAck* ack);
  bool is_connected() const;

  void SetTransmitEnabled(bool enabled);
  bool transmit_enabled() const;

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

 private:
  bool ConnectLocked();
  void CloseLocked();

  bool SendAllLocked(const std::string& payload);
  bool ReadLineLocked(std::string* line);
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

  mutable std::mutex mu_;
  bool connected_ = false;
  bool transmit_enabled_ = true;
  int socket_fd_ = -1;
  std::string recv_buffer_;
  std::string session_id_;

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
