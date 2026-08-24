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
                               std::uint64_t max_bytes,
                               std::uint64_t compact_min_dead_bytes,
                               std::uint64_t compact_max_live_bytes)
    : queue_dir_(std::move(queue_dir)),
      retention_hours_(retention_hours),
      max_bytes_(max_bytes),
      compact_min_dead_bytes_(compact_min_dead_bytes),
      compact_max_live_bytes_(compact_max_live_bytes) {
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
  live_bytes_ = 0;
  dead_bytes_ = 0;

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

  // Load what parses, skip what doesn't. A SIGABRT/SIGKILL between the two
  // halves of an appended line leaves a torn final line; treating any bad
  // line as fatal (the old behaviour) turned one torn append into a
  // permanently memory-only queue. The primary CSV log is the archival
  // record -- the queue only owes retransmission of what it can still read.
  const std::int64_t now = CurrentUnixEpochSeconds();
  const std::int64_t retention_s =
      static_cast<std::int64_t>(retention_hours_ * 3600.0);
  std::size_t skipped_lines = 0;
  std::size_t expired_frames = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    QueuedTelemetryFrame frame;
    if (!ParseLine(line, &frame)) {
      ++skipped_lines;
      continue;
    }
    if (retention_s > 0 && (now - frame.queued_epoch_s) > retention_s) {
      ++expired_frames;
      continue;
    }
    live_bytes_ += LineBytes(frame);
    frames_.push_back(std::move(frame));
  }
  in.close();

  // One startup compaction (before the systemd watchdog is armed) so the
  // on-disk file starts exactly equal to the live set.
  if (!CompactLocked(error)) {
    persistence_enabled_ = false;
    return false;
  }
  if ((skipped_lines > 0 || expired_frames > 0) && error != nullptr) {
    std::ostringstream oss;
    oss << "queue loaded with " << skipped_lines << " unparseable and "
        << expired_frames << " expired line(s) dropped";
    *error = oss.str();
  }
  return true;
}

bool TelemetryQueue::Enqueue(const QueuedTelemetryFrame& frame, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  frames_.push_back(frame);
  live_bytes_ += LineBytes(frame);
  PruneLocked();
  RetryPersistenceLocked();
  if (!persistence_enabled_) return true;
  if (!AppendLocked(frame)) {
    if (error != nullptr) {
      *error = "failed to persist queue frame";
    }
    persistence_enabled_ = false;
    return true;
  }
  MaybeCompactLocked();
  return true;
}

bool TelemetryQueue::Acknowledge(const std::string& session_id,
                                 std::uint64_t seq,
                                 std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(mu_);

  std::uint64_t removed_bytes = 0;
  auto remove_from = std::remove_if(
      frames_.begin(), frames_.end(),
      [&](const QueuedTelemetryFrame& frame) {
        const bool acked = frame.session_id == session_id && frame.seq <= seq;
        if (acked) removed_bytes += LineBytes(frame);
        return acked;
      });

  if (remove_from == frames_.end()) {
    return true;
  }

  frames_.erase(remove_from, frames_.end());
  live_bytes_ -= removed_bytes;
  dead_bytes_ += removed_bytes;
  RetryPersistenceLocked();
  MaybeCompactLocked();
  return true;
}

bool TelemetryQueue::AcknowledgeExact(const QueuedTelemetryFrame& frame,
                                      std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(mu_);

  std::uint64_t removed_bytes = 0;
  auto remove_from = std::remove_if(
      frames_.begin(), frames_.end(),
      [&](const QueuedTelemetryFrame& pending) {
        const bool acked = pending.session_id == frame.session_id &&
                           pending.seq == frame.seq &&
                           pending.frame == frame.frame;
        if (acked) removed_bytes += LineBytes(pending);
        return acked;
      });
  if (remove_from == frames_.end()) {
    return true;
  }

  frames_.erase(remove_from, frames_.end());
  live_bytes_ -= removed_bytes;
  dead_bytes_ += removed_bytes;
  RetryPersistenceLocked();
  MaybeCompactLocked();
  return true;
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingFrames(
    std::size_t max_frames) const {
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t count = std::min(max_frames, frames_.size());
  return std::vector<QueuedTelemetryFrame>(frames_.begin(),
                                           frames_.begin() + count);
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingFrames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::vector<QueuedTelemetryFrame>(frames_.begin(), frames_.end());
}

std::size_t TelemetryQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return frames_.size();
}

bool TelemetryQueue::AppendLocked(const QueuedTelemetryFrame& frame) {
  std::ofstream out(queue_file_, std::ios::app);
  if (!out.is_open()) {
    return false;
  }
  out << FormatLine(frame) << '\n';
  out.flush();
  return out.good();
}

bool TelemetryQueue::CompactLocked(std::string* error) {
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

  dead_bytes_ = 0;
  return true;
}

void TelemetryQueue::MaybeCompactLocked() {
  if (!persistence_enabled_) return;
  if (dead_bytes_ < compact_min_dead_bytes_) return;
  // A compaction rewrites every live frame in one go on the control-loop
  // thread. Cap the live set it is allowed to do that for, so the stall
  // stays well inside the systemd watchdog budget; a bigger live set keeps
  // its dead weight on disk until it drains down (or until the next
  // startup compaction), which only costs disk space.
  if (live_bytes_ > compact_max_live_bytes_) return;
  if (!CompactLocked(nullptr)) {
    persistence_enabled_ = false;
  }
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
  if (!CompactLocked(nullptr)) persistence_enabled_ = false;
}

void TelemetryQueue::PruneLocked() {
  const std::int64_t now = CurrentUnixEpochSeconds();
  const std::int64_t retention_s =
      static_cast<std::int64_t>(retention_hours_ * 3600.0);

  auto drop_front = [&]() {
    const std::uint64_t bytes = LineBytes(frames_.front());
    live_bytes_ -= bytes;
    // The dropped line may still be in the file until the next compaction.
    dead_bytes_ += bytes;
    frames_.pop_front();
  };

  // Frames past retention are dropped outright. The old code only pruned
  // when the queue was over max_bytes AND the frame was stale, so a backlog
  // under the (8 GB default) size cap was kept forever and replayed
  // weeks-old frames at the ground station whenever the link came up.
  if (retention_s > 0) {
    while (!frames_.empty() &&
           (now - frames_.front().queued_epoch_s) > retention_s) {
      drop_front();
    }
  }

  if (max_bytes_ == 0U) {
    return;
  }
  while (!frames_.empty() && live_bytes_ > max_bytes_) {
    drop_front();
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

std::uint64_t TelemetryQueue::LineBytes(const QueuedTelemetryFrame& frame) {
  return static_cast<std::uint64_t>(FormatLine(frame).size() + 1);
}

}  // namespace coatheal
