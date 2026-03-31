#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace coatheal {

struct QueuedTelemetryFrame {
  std::int64_t queued_epoch_s = 0;
  std::string session_id;
  std::uint64_t seq = 0;
  std::string frame;
};

std::int64_t CurrentUnixEpochSeconds();

class TelemetryQueue {
 public:
  TelemetryQueue(std::string queue_dir, double retention_hours, std::uint64_t max_bytes);

  bool Initialize(std::string* error);
  bool Enqueue(const QueuedTelemetryFrame& frame, std::string* error);
  bool Acknowledge(const std::string& session_id, std::uint64_t seq, std::string* error);

  std::vector<QueuedTelemetryFrame> PendingFrames() const;
  std::size_t size() const;

 private:
  bool PersistLocked(std::string* error);
  void PruneLocked();

  static bool ParseLine(const std::string& line, QueuedTelemetryFrame* out);
  static std::string FormatLine(const QueuedTelemetryFrame& frame);
  std::uint64_t EstimatedBytesLocked() const;

  std::string queue_dir_;
  std::string queue_file_;
  double retention_hours_ = 72.0;
  std::uint64_t max_bytes_ = 0;

  mutable std::mutex mu_;
  std::vector<QueuedTelemetryFrame> frames_;
};

}  // namespace coatheal
