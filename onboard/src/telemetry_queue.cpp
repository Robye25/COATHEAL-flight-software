#include "coatheal/telemetry_queue.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace coatheal {
namespace {

constexpr char kSeparator = '\t';
constexpr std::uint64_t kLengthKeyBase = std::numeric_limits<std::uint64_t>::max();

bool IsEventFrame(const QueuedTelemetryFrame& frame) {
  return frame.frame.rfind("EVT,", 0) == 0;
}

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
                               std::uint64_t compact_max_live_bytes,
                               std::uint64_t compact_cheap_live_bytes)
    : queue_dir_(std::move(queue_dir)),
      retention_hours_(retention_hours),
      max_bytes_(max_bytes),
      compact_min_dead_bytes_(compact_min_dead_bytes),
      compact_max_live_bytes_(compact_max_live_bytes),
      compact_cheap_live_bytes_(compact_cheap_live_bytes) {
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
  by_key_.clear();
  events_.clear();
  runs_.clear();
  runs_by_length_.clear();
  next_index_ = 0;
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
    InsertLocked(std::move(frame));
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

bool TelemetryQueue::Enqueue(const QueuedTelemetryFrame& frame, std::string* error,
                             std::uint64_t* index) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::uint64_t assigned = next_index_;
  InsertLocked(frame);
  if (index != nullptr) *index = assigned;
  PruneLocked();
  RetryPersistenceLocked();
  if (!persistence_enabled_) return true;
  if (!AppendToFileLocked(frame)) {
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

  std::vector<std::uint64_t> acked;
  for (const auto& [index, frame] : frames_) {
    if (frame.session_id == session_id && frame.seq <= seq) acked.push_back(index);
  }
  if (acked.empty()) {
    return true;
  }
  for (const std::uint64_t index : acked) RemoveLocked(index);
  RetryPersistenceLocked();
  MaybeCompactLocked();
  return true;
}

bool TelemetryQueue::AcknowledgeExact(const QueuedTelemetryFrame& frame,
                                      std::string* error) {
  (void)error;
  std::lock_guard<std::mutex> lock(mu_);

  const auto key = by_key_.find({frame.session_id, frame.seq});
  if (key == by_key_.end()) {
    return true;
  }
  const auto pending = frames_.find(key->second);
  if (pending == frames_.end() || pending->second.frame != frame.frame) {
    return true;
  }
  RemoveLocked(pending->first);
  RetryPersistenceLocked();
  MaybeCompactLocked();
  return true;
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingFrames(
    std::size_t max_frames) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<QueuedTelemetryFrame> out;
  out.reserve(std::min(max_frames, frames_.size()));
  for (auto it = frames_.begin(); it != frames_.end() && out.size() < max_frames; ++it) {
    out.push_back(it->second);
  }
  return out;
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingFrames() const {
  return PendingFrames(std::numeric_limits<std::size_t>::max());
}

bool TelemetryQueue::FrameAt(std::uint64_t index, QueuedTelemetryFrame* frame) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = frames_.find(index);
  if (it == frames_.end()) return false;
  if (frame != nullptr) *frame = it->second;
  return true;
}

std::vector<QueuedTelemetryFrame> TelemetryQueue::PendingEvents(
    std::size_t max_frames) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<QueuedTelemetryFrame> out;
  for (auto it = events_.begin(); it != events_.end() && out.size() < max_frames; ++it) {
    out.push_back(frames_.at(*it));
  }
  return out;
}

bool TelemetryQueue::NextReplay(QueuedTelemetryFrame* frame) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (runs_by_length_.empty()) return false;
  const std::uint64_t start = runs_by_length_.begin()->second;
  const std::uint64_t end = runs_.at(start);
  const std::uint64_t middle = start + (end - start) / 2;
  if (frame != nullptr) *frame = frames_.at(middle);
  return true;
}

std::size_t TelemetryQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return frames_.size();
}

void TelemetryQueue::InsertLocked(QueuedTelemetryFrame frame) {
  const std::uint64_t index = next_index_++;
  frame.index = index;
  live_bytes_ += LineBytes(frame);
  by_key_[{frame.session_id, frame.seq}] = index;
  if (IsEventFrame(frame)) events_.insert(index);

  // The new frame extends the newest run when the frame before it is still
  // pending; otherwise it starts a run of its own.
  if (!runs_.empty()) {
    auto last = std::prev(runs_.end());
    if (last->second + 1 == index) {
      const std::uint64_t start = last->first;
      EraseRunLocked(last);
      AddRunLocked(start, index);
      frames_.emplace(index, std::move(frame));
      return;
    }
  }
  AddRunLocked(index, index);
  frames_.emplace(index, std::move(frame));
}

void TelemetryQueue::RemoveLocked(std::uint64_t index) {
  const auto it = frames_.find(index);
  if (it == frames_.end()) return;
  const std::uint64_t bytes = LineBytes(it->second);
  live_bytes_ -= bytes;
  // The line stays in the file until the next compaction.
  dead_bytes_ += bytes;
  const auto key = by_key_.find({it->second.session_id, it->second.seq});
  if (key != by_key_.end() && key->second == index) by_key_.erase(key);
  events_.erase(index);
  frames_.erase(it);

  // Split the run that held `index` around it.
  auto run = runs_.upper_bound(index);
  if (run == runs_.begin()) return;
  --run;
  const std::uint64_t start = run->first;
  const std::uint64_t end = run->second;
  if (index > end) return;
  EraseRunLocked(run);
  if (start < index) AddRunLocked(start, index - 1);
  if (index < end) AddRunLocked(index + 1, end);
}

void TelemetryQueue::AddRunLocked(std::uint64_t start, std::uint64_t end) {
  runs_[start] = end;
  runs_by_length_.insert({kLengthKeyBase - (end - start + 1), start});
}

void TelemetryQueue::EraseRunLocked(
    std::map<std::uint64_t, std::uint64_t>::iterator run) {
  runs_by_length_.erase({kLengthKeyBase - (run->second - run->first + 1), run->first});
  runs_.erase(run);
}

bool TelemetryQueue::AppendToFileLocked(const QueuedTelemetryFrame& frame) {
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

    for (const auto& entry : frames_) {
      out << FormatLine(entry.second) << '\n';
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
  if (dead_bytes_ == 0U) return;

  // A compaction rewrites every live frame in one go on the control-loop
  // thread, so what governs is how much LIVE data has to be written --
  // dead bytes cost nothing to drop. Two independent reasons to do it:
  //
  //  (a) The live set is small enough that the rewrite is free. This is
  //      the healthy steady state: each tick enqueues one frame and the
  //      drain acks it, so the queue sits at or near empty. Compacting
  //      here costs microseconds and leaves nothing already-acked on
  //      disk -- which is what stops an unclean shutdown (power cut, or a
  //      watchdog kill) from replaying every frame acked since the last
  //      compaction. The ground station deduplicates those, so nothing is
  //      lost, but it spends downlink re-sending frames that already
  //      landed and fills the operator's event log with "[dup] dropped".
  //
  //  (b) Dead weight has built up past the point worth carrying, and the
  //      live set is still small enough for the stall to stay bounded.
  //
  // Above compact_max_live_bytes_ neither applies: the backlog keeps its
  // dead weight until it drains (or until the next startup compaction),
  // which only costs disk space. That is the case the watchdog budget
  // cares about, and the one where at-least-once redelivery survives.
  const bool rewrite_is_cheap = live_bytes_ <= compact_cheap_live_bytes_;
  const bool dead_weight_worth_dropping =
      dead_bytes_ >= compact_min_dead_bytes_ &&
      live_bytes_ <= compact_max_live_bytes_;
  if (!rewrite_is_cheap && !dead_weight_worth_dropping) return;

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

  // Frames past retention are dropped outright. The old code only pruned
  // when the queue was over max_bytes AND the frame was stale, so a backlog
  // under the (8 GB default) size cap was kept forever and replayed
  // weeks-old frames at the ground station whenever the link came up.
  if (retention_s > 0) {
    while (!frames_.empty() &&
           (now - frames_.begin()->second.queued_epoch_s) > retention_s) {
      RemoveLocked(frames_.begin()->first);
    }
  }

  if (max_bytes_ == 0U) {
    return;
  }
  while (!frames_.empty() && live_bytes_ > max_bytes_) {
    RemoveLocked(frames_.begin()->first);
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
