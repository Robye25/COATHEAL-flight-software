#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace coatheal {

struct TelemetryAck {
  std::string session_id;
  std::uint64_t seq = 0;
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
                  std::string static_pi_ip);
  ~TelemetryClient();

  TelemetryClient(const TelemetryClient&) = delete;
  TelemetryClient& operator=(const TelemetryClient&) = delete;

  bool SendFrameAwaitAck(const std::string& frame, TelemetryAck* ack);
  bool is_connected() const;

  // When disabled, SendFrameAwaitAck returns false immediately without
  // touching the socket and the TX side is closed promptly (<1 s). Upstream
  // callers keep enqueuing frames into the on-disk queue so nothing is lost.
  void SetTransmitEnabled(bool enabled);
  bool transmit_enabled() const;

  std::string session_id() const;
  std::string current_host() const;

 private:
  bool ConnectLocked();
  void CloseLocked();

  bool DiscoverGroundHostLocked(std::string* discovered_host,
                                int* discovered_telemetry_port,
                                int* discovered_command_port);
  bool SendAllLocked(const std::string& payload);
  bool ReadLineLocked(std::string* line);
  static bool ParseAckLine(const std::string& line, TelemetryAck* ack);

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

  mutable std::mutex mu_;
  bool connected_ = false;
  bool transmit_enabled_ = true;
  int socket_fd_ = -1;
  std::string recv_buffer_;
  std::string session_id_;
};

}  // namespace coatheal
