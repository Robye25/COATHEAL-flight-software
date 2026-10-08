#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

#include "coatheal/link_budget.hpp"
#include "coatheal/telemetry_queue.hpp"

namespace coatheal {

struct TelemetryAck {
  std::string session_id;
  std::uint64_t seq = 0;
};

enum class SendStatus {
  kSent,          // on the wire and acknowledged
  kNoBudget,      // the link budget has no room right now; nothing was sent
  kNotConnected,  // no ground station reachable (or waiting out a backoff)
  kSilent,        // radio silence
  kFailed,        // not answered in time, or not with this frame's ACK; the
                  // connection was reset
};

// How long a tick's backlog replay may keep sending (docs/link-budget.md
// "Replay order"). A replayed frame keeps counting against the budget for
// about a second after its exchange, so replay stops early enough that the
// next tick's live frame still finds its room within kLiveFrameSlack of that
// tick's start: at 1 Hz, replay starts only in the first 150 ms of a tick;
// faster than 1 Hz, nothing is replayed; and never past 70 % of the tick.
constexpr std::chrono::milliseconds kLiveFrameSlack{150};
std::chrono::steady_clock::time_point ReplayDeadline(std::chrono::steady_clock::time_point tick_start,
                                                     std::chrono::steady_clock::duration period);

// What the drain needs from the telemetry link. TelemetryClient implements it
// against a socket; tests implement it against a fake clock.
class FrameSender {
 public:
  virtual ~FrameSender() = default;
  // Sends one text line (a TX-stamped DATA line or an EVT line) and waits for
  // the ground station's ACK, charging the link budget at `priority`. Waits
  // for room in the budget until `budget_deadline`, then gives up with
  // kNoBudget (the frame stays queued).
  virtual SendStatus SendFrame(const std::string& line, LinkPriority priority,
                               std::chrono::steady_clock::time_point budget_deadline,
                               TelemetryAck* ack) = 0;
  virtual bool is_connected() const = 0;
};

struct DrainResult {
  // True once an ACK arrived this tick, or when the link is up but the budget
  // held frames back -- a budget-deferred frame is not a lost link.
  bool link_ok = false;
  bool error = false;
  std::string error_text;
  std::size_t sent = 0;
  std::size_t replayed = 0;
};

// One tick of telemetry delivery (docs/link-budget.md "Replay order"): this
// tick's DATA frame and pending EVT frames (waiting for budget room until
// `live_deadline`), then backlog frames by bisection until `replay_deadline`.
// Anything the budget holds back stays queued for a later tick.
class TelemetryDrain {
 public:
  static constexpr std::uint64_t kNoLiveFrame = std::numeric_limits<std::uint64_t>::max();
  static constexpr std::size_t kMaxEventsPerTick = 8;

  using NowFn = std::function<std::chrono::steady_clock::time_point()>;

  // `now` is the clock the deadlines are on (tests pass a fake one).
  TelemetryDrain(TelemetryQueue* queue, FrameSender* sender, NowFn now = nullptr);

  DrainResult Drain(std::uint64_t live_index, std::int64_t now_epoch_s,
                    std::chrono::steady_clock::time_point live_deadline,
                    std::chrono::steady_clock::time_point replay_deadline);

 private:
  SendStatus SendOne(const QueuedTelemetryFrame& frame, LinkPriority priority,
                     std::int64_t now_epoch_s,
                     std::chrono::steady_clock::time_point budget_deadline,
                     DrainResult* result);

  TelemetryQueue* queue_;
  FrameSender* sender_;
  NowFn now_;
};

}  // namespace coatheal
