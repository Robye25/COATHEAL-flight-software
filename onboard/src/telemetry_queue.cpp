#include "coatheal/telemetry_queue.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace coatheal {
namespace {

constexpr char kSeparator = '\t';

}  // namespace

std::int64_t CurrentUnixEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

TelemetryQueue::TelemetryQueue(std::string queue_dir,
                               double retention_hours,
                               std::uint64_t max_bytes)
    : queue_dir_(std::move(queue_dir)),
      retention_hours_(retention_hours),
      max_bytes_(max_bytes) {
  std::filesystem::path p(queue_dir_);
  queue_file_ = (p / "pending.queue").string();
}

bool TelemetryQueue::Initialize(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);

  std::error_code ec;
  std::filesystem::create_directories(queue_dir_, ec);
  if (ec) {
    persistence_enabled_ = false;
    if (error != nullptr) {
      *error = "unable to create queue directory: " + queue_dir_;
    }
    return false;
  }

  frames_.clear();

  std::ifstream in(queue_file_);
  if (!in.is_open()) {
    std::ofstream create(queue_file_, std::ios::app);
    if (!create.is_open()) {
      persistence_enabled_ = false;
      if (error != nullptr) {
        *error = "unable to initialize queue file: " + queue_file_;
      }
      return false;
    }
    return true;
  }

  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty()) {
      continue;
    }

    QueuedTelemetryFrame frame;
    if (!ParseLine(line, &frame)) {
      persistence_enabled_ = false;
      if (error != nullptr) {
        *error = "failed to parse queue line " + std::to_string(line_no);
      }
      return false;
    }
    frames_.push_back(std::move(frame));
  }

  return true;
}

bool TelemetryQueue::Enqueue(const QueuedTelemetryFrame& frame, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  frames_.push_back(frame);
  PruneLocked();
  RetryPersistenceLocked();
  if (!persistence_enabled_) return true;
  if (!PersistLocked(error)) {
    persistence_enabled_ = false;
    return true;
  }
  return true;
}

bool TelemetryQueue::Acknowledge(const std::string& session_id,
                                 std::uint64_t seq,
                                 std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);

  auto remove_from = std::remove_if(frames_.begin(),
                                    frames_.end(),
                                    [&](const QueuedTelemetryFrame& frame) {
                                      return frame.session_id == session_id && frame.seq <= seq;
                                    });

  if (remove_from == frames_.end()) {
    return true;
  }

  frames_.erase(remove_from, frames_.end());
  RetryPersistenceLocked();
  if (!persistence_enabled_) return true;
  if (!PersistLocked(error)) {
    persistence_enabled_ = false;
    return true;
  }
  return true;
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingFrames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return frames_;
}

std::size_t TelemetryQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return frames_.size();
}

bool TelemetryQueue::PersistLocked(std::string* error) {
  const std::string tmp_file = queue_file_ + ".tmp";
  {
    std::ofstream out(tmp_file, std::ios::trunc);
    if (!out.is_open()) {
      if (error != nullptr) {
        *error = "unable to write temporary queue file";
      }
      return false;
    }

    for (const QueuedTelemetryFrame& frame : frames_) {
      out << FormatLine(frame) << '\n';
      if (!out.good()) {
        if (error != nullptr) {
          *error = "failed to persist queue frame";
        }
        return false;
      }
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp_file, queue_file_, ec);
  if (ec) {
    std::filesystem::remove(queue_file_, ec);
    ec.clear();
    std::filesystem::rename(tmp_file, queue_file_, ec);
  }

  if (ec) {
    if (error != nullptr) {
      *error = "unable to commit queue file";
    }
    return false;
  }

  return true;
}

void TelemetryQueue::RetryPersistenceLocked() {
  if (persistence_enabled_) return;
  const auto now = std::chrono::steady_clock::now();
  if (next_persistence_retry_.time_since_epoch().count() != 0 &&
      now < next_persistence_retry_) {
    return;
  }
  next_persistence_retry_ = now + std::chrono::seconds(5);
  std::error_code ec;
  std::filesystem::create_directories(queue_dir_, ec);
  if (ec) return;
  persistence_enabled_ = true;
  if (!PersistLocked(nullptr)) persistence_enabled_ = false;
}

void TelemetryQueue::PruneLocked() {
  if (max_bytes_ == 0U) {
    return;
  }

  const std::int64_t now = CurrentUnixEpochSeconds();
  const std::int64_t retention_s = static_cast<std::int64_t>(retention_hours_ * 3600.0);

  while (!frames_.empty() && EstimatedBytesLocked() > max_bytes_) {
    const QueuedTelemetryFrame& oldest = frames_.front();
    if ((now - oldest.queued_epoch_s) <= retention_s) {
      break;
    }
    frames_.erase(frames_.begin());
  }
}

bool TelemetryQueue::ParseLine(const std::string& line, QueuedTelemetryFrame* out) {
  if (out == nullptr) {
    return false;
  }

  const std::size_t first_sep = line.find(kSeparator);
  if (first_sep == std::string::npos) {
    return false;
  }

  const std::size_t second_sep = line.find(kSeparator, first_sep + 1);
  if (second_sep == std::string::npos) {
    return false;
  }

  const std::size_t third_sep = line.find(kSeparator, second_sep + 1);
  if (third_sep == std::string::npos) {
    return false;
  }

  try {
    out->queued_epoch_s = std::stoll(line.substr(0, first_sep));
    out->session_id = line.substr(first_sep + 1, second_sep - first_sep - 1);
    out->seq = static_cast<std::uint64_t>(
        std::stoull(line.substr(second_sep + 1, third_sep - second_sep - 1)));
    out->frame = line.substr(third_sep + 1);
  } catch (...) {
    return false;
  }

  return !out->session_id.empty() && !out->frame.empty();
}

std::string TelemetryQueue::FormatLine(const QueuedTelemetryFrame& frame) {
  std::ostringstream oss;
  oss << frame.queued_epoch_s << kSeparator << frame.session_id << kSeparator << frame.seq
      << kSeparator << frame.frame;
  return oss.str();
}

std::uint64_t TelemetryQueue::EstimatedBytesLocked() const {
  std::uint64_t total = 0;
  for (const QueuedTelemetryFrame& frame : frames_) {
    total += static_cast<std::uint64_t>(FormatLine(frame).size() + 1);
  }
  return total;
}

}  // namespace coatheal
