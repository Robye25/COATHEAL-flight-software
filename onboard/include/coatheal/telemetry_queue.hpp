#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coatheal {

struct QueuedTelemetryFrame {
  std::int64_t queued_epoch_s = 0;
  std::string session_id;
  std::uint64_t seq = 0;
  std::string frame;
  // Position in the queue, assigned by Enqueue/Initialize in arrival order
  // (never persisted). Consecutive indices are consecutive frames.
  std::uint64_t index = 0;
};

std::int64_t CurrentUnixEpochSeconds();

// Durable retransmission buffer between the 1 Hz control loop and the ground
// link. Every mutation used to rewrite the whole backing file, which is
// O(backlog) disk I/O on the control-loop thread: with a ~100 MB backlog a
// single Enqueue took 5-8 s on the Pi's SD card and every drained ACK added
// another full rewrite, so a connected ground station pushed one tick far
// past WatchdogSec=10 and systemd killed the process mid-link. The contract
// now is:
//   - Enqueue appends one line to pending.queue (O(1) disk work per tick).
//   - Acknowledge* only drops frames from memory and counts the on-disk
//     bytes they occupied as dead; the file is compacted (rewritten without
//     dead lines) only when enough dead bytes accumulate AND the live set is
//     small enough for the rewrite to be a bounded stall.
//   - Whenever the live set is small enough for the rewrite to be free,
//     compaction happens immediately, so the healthy steady state leaves
//     nothing acked on disk.
//   - Only with a backlog too large to rewrite cheaply does a crash
//     between an ACK and the next compaction re-deliver those frames on
//     restart (at-least-once); the ground station already deduplicates by
//     (session, seq) / (session, pull_id).
//   - Initialize drops frames older than retention_hours outright and
//     compacts once, so a stale backlog can never accrete across reboots.
//
// Frames leave in any order (docs/link-budget.md "Replay order"): each is
// acknowledged exactly, and the backlog is replayed by bisection -- the
// middle frame of the longest run of consecutive unacknowledged frames
// first -- so a replay cut short by the 24 kbps budget or another outage
// still leaves an even picture of the gap.
class TelemetryQueue {
 public:
  static constexpr std::uint64_t kDefaultCompactMinDeadBytes = 512ULL * 1024;
  static constexpr std::uint64_t kDefaultCompactMaxLiveBytes =
      16ULL * 1024 * 1024;
  // Below this much live data a compaction rewrite is trivially cheap --
  // and the healthy steady state sits far below it, because every frame
  // enqueued on a tick is acked on the same tick and the queue drains to
  // empty. Compacting there costs microseconds and is what keeps an
  // unclean shutdown from replaying hundreds of already-acked frames.
  static constexpr std::uint64_t kDefaultCompactCheapLiveBytes =
      64ULL * 1024;

  TelemetryQueue(std::string queue_dir, double retention_hours,
                 std::uint64_t max_bytes,
                 std::uint64_t compact_min_dead_bytes =
                     kDefaultCompactMinDeadBytes,
                 std::uint64_t compact_max_live_bytes =
                     kDefaultCompactMaxLiveBytes,
                 std::uint64_t compact_cheap_live_bytes =
                     kDefaultCompactCheapLiveBytes);

  bool Initialize(std::string* error);
  // `index` (optional) receives the queue index given to the frame.
  bool Enqueue(const QueuedTelemetryFrame& frame, std::string* error,
               std::uint64_t* index = nullptr);
  // Cumulative: every frame of `session_id` with seq <= `seq`.
  bool Acknowledge(const std::string& session_id, std::uint64_t seq,
                   std::string* error);
  // Exactly this frame (same session, seq and text).
  bool AcknowledgeExact(const QueuedTelemetryFrame& frame, std::string* error);

  // Oldest-first copy of at most `max_frames` pending frames.
  std::vector<QueuedTelemetryFrame> PendingFrames(std::size_t max_frames) const;
  std::vector<QueuedTelemetryFrame> PendingFrames() const;
  // The pending frame at `index`, if it has not been acknowledged or pruned.
  bool FrameAt(std::uint64_t index, QueuedTelemetryFrame* frame) const;
  // Oldest-first pending EVT frames, at most `max_frames`.
  std::vector<QueuedTelemetryFrame> PendingEvents(std::size_t max_frames) const;
  // The next backlog frame in bisection order: the middle frame of the
  // longest run of consecutive pending frames (the oldest such run on a tie).
  // A 15-frame gap replays as 7, 3, 11, 1, 5, 9, 13, 0, 2, 4, ...
  bool NextReplay(QueuedTelemetryFrame* frame) const;
  std::size_t size() const;

 private:
  bool AppendToFileLocked(const QueuedTelemetryFrame& frame);
  void InsertLocked(QueuedTelemetryFrame frame);
  void RemoveLocked(std::uint64_t index);
  void AddRunLocked(std::uint64_t start, std::uint64_t end);
  void EraseRunLocked(std::map<std::uint64_t, std::uint64_t>::iterator run);
  bool CompactLocked(std::string* error);
  void MaybeCompactLocked();
  void RetryPersistenceLocked();
  void PruneLocked();

  static bool ParseLine(const std::string& line, QueuedTelemetryFrame* out);
  static std::string FormatLine(const QueuedTelemetryFrame& frame);
  static std::uint64_t LineBytes(const QueuedTelemetryFrame& frame);

  std::string queue_dir_;
  std::string queue_file_;
  double retention_hours_ = 72.0;
  std::uint64_t max_bytes_ = 0;
  std::uint64_t compact_min_dead_bytes_ = kDefaultCompactMinDeadBytes;
  std::uint64_t compact_max_live_bytes_ = kDefaultCompactMaxLiveBytes;
  std::uint64_t compact_cheap_live_bytes_ = kDefaultCompactCheapLiveBytes;

  mutable std::mutex mu_;
  // index -> frame, in arrival order.
  std::map<std::uint64_t, QueuedTelemetryFrame> frames_;
  // (session, seq) -> index, for exact acknowledgement.
  std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> by_key_;
  std::set<std::uint64_t> events_;
  // Maximal runs of consecutive pending indices: start -> end (inclusive),
  // and the same runs ordered longest first, then oldest.
  std::map<std::uint64_t, std::uint64_t> runs_;
  std::set<std::pair<std::uint64_t, std::uint64_t>> runs_by_length_;
  std::uint64_t next_index_ = 0;
  // Bytes the live frames occupy in file format, maintained incrementally
  // (the old code re-serialised every frame per Enqueue just to size the
  // queue -- another O(backlog) pass on the control loop).
  std::uint64_t live_bytes_ = 0;
  // Bytes of acked/pruned lines still sitting in pending.queue.
  std::uint64_t dead_bytes_ = 0;
  bool persistence_enabled_ = true;
  std::chrono::steady_clock::time_point next_persistence_retry_{};
};

}  // namespace coatheal
