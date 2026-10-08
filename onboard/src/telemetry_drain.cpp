#include "coatheal/telemetry_drain.hpp"

#include <algorithm>
#include <utility>

#include "coatheal/telemetry.hpp"

namespace coatheal {

std::chrono::steady_clock::time_point ReplayDeadline(std::chrono::steady_clock::time_point tick_start,
                                                     std::chrono::steady_clock::duration period) {
  const auto by_fraction = tick_start + period * 7 / 10;
  const auto by_next_live = tick_start + period - std::chrono::seconds(1) + kLiveFrameSlack;
  return std::min(by_fraction, by_next_live);
}

TelemetryDrain::TelemetryDrain(TelemetryQueue* queue, FrameSender* sender, NowFn now)
    : queue_(queue), sender_(sender), now_(std::move(now)) {
  if (!now_) now_ = [] { return std::chrono::steady_clock::now(); };
}

DrainResult TelemetryDrain::Drain(std::uint64_t live_index, std::int64_t now_epoch_s,
                                  std::chrono::steady_clock::time_point live_deadline,
                                  std::chrono::steady_clock::time_point replay_deadline) {
  DrainResult result;
  const auto finish = [&](SendStatus status) {
    switch (status) {
      case SendStatus::kNoBudget:
        result.link_ok = result.link_ok || sender_->is_connected();
        break;
      case SendStatus::kNotConnected:
        result.error = true;
        result.error_text = "no ground station connection";
        break;
      case SendStatus::kFailed:
        result.error = true;
        if (result.error_text.empty()) result.error_text = "failed to send telemetry frame";
        break;
      case SendStatus::kSilent:
      case SendStatus::kSent:
        break;
    }
    return result;
  };

  QueuedTelemetryFrame frame;
  if (live_index != kNoLiveFrame && queue_->FrameAt(live_index, &frame)) {
    const SendStatus status =
        SendOne(frame, LinkPriority::kLive, now_epoch_s, live_deadline, &result);
    if (status != SendStatus::kSent) return finish(status);
  }

  for (const QueuedTelemetryFrame& event : queue_->PendingEvents(kMaxEventsPerTick)) {
    const SendStatus status =
        SendOne(event, LinkPriority::kLive, now_epoch_s, live_deadline, &result);
    if (status != SendStatus::kSent) return finish(status);
  }

  while (now_() < replay_deadline && queue_->NextReplay(&frame)) {
    const SendStatus status =
        SendOne(frame, LinkPriority::kReplay, now_epoch_s, replay_deadline, &result);
    if (status != SendStatus::kSent) return finish(status);
    ++result.replayed;
  }

  if (result.sent == 0) {
    // Nothing was due: the link is as good as the connection.
    result.link_ok = sender_->is_connected();
  }
  return result;
}

SendStatus TelemetryDrain::SendOne(const QueuedTelemetryFrame& frame, LinkPriority priority,
                                   std::int64_t now_epoch_s,
                                   std::chrono::steady_clock::time_point budget_deadline,
                                   DrainResult* result) {
  TelemetryAck ack;
  const SendStatus status = sender_->SendFrame(
      TagFrameForTransmit(frame.frame, frame.queued_epoch_s, now_epoch_s), priority,
      budget_deadline, &ack);
  if (status != SendStatus::kSent) return status;

  // EVT frames are acknowledged with seq 0; DATA frames with their own seq.
  const bool is_event = frame.frame.rfind("EVT,", 0) == 0;
  const std::uint64_t expected_seq = is_event ? 0U : frame.seq;
  if (ack.session_id != frame.session_id || ack.seq != expected_seq) {
    result->error = true;
    result->error_text = "received mismatched telemetry ACK";
    return SendStatus::kFailed;
  }
  queue_->AcknowledgeExact(frame, nullptr);
  result->link_ok = true;
  ++result->sent;
  return SendStatus::kSent;
}

}  // namespace coatheal
