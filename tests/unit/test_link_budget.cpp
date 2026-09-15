// The 24 kbps E-Link budget (docs/link-budget.md): the sliding-window ledger,
// the z1 telemetry codec, the bisection replay order of the durable queue, and
// the drain that combines them. The drain test replays a ten-minute outage at
// 1 Hz against a fake clock and checks every 1-second window of what the
// onboard puts on the wire.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coatheal/command_server.hpp"
#include "coatheal/link_budget.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_codec.hpp"
#include "coatheal/telemetry_drain.hpp"
#include "coatheal/telemetry_queue.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using Clock = coatheal::LinkBudget::Clock;
using namespace std::chrono_literals;

class FakeClock {
 public:
  Clock::time_point now() const { return Clock::time_point(offset_.load()); }
  void advance(Clock::duration d) { offset_ = offset_.load() + d; }
  void set(Clock::duration d) { offset_ = d; }
  coatheal::LinkBudget::NowFn fn() {
    return [this] { return now(); };
  }

 private:
  std::atomic<Clock::duration> offset_{Clock::duration(1000s)};
};

std::filesystem::path TempDir(const std::string& tag) {
  static int counter = 0;
  return std::filesystem::temp_directory_path() /
         ("coatheal_link_" + tag + "_" + std::to_string(coatheal::CurrentUnixEpochSeconds()) +
          "_" + std::to_string(++counter));
}

coatheal::QueuedTelemetryFrame DataFrame(const std::string& session, std::uint64_t seq) {
  coatheal::QueuedTelemetryFrame f;
  f.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
  f.session_id = session;
  f.seq = seq;
  f.frame = "DATA," + session + "," + std::to_string(seq) + ",2026-09-15T18:48:05Z,1,25.36";
  return f;
}

void TestWireModel() {
  assert(coatheal::wire::kPureAck == 90);
  assert(coatheal::wire::kSyn == 98);
  assert(coatheal::wire::TcpBytes(0) == 90);
  assert(coatheal::wire::TcpBytes(292) == 66 + 292 + 24);
  assert(coatheal::wire::TcpBytes(1448) == 90 + 1448);
  assert(coatheal::wire::TcpBytes(1449) == 2 * 90 + 1449);
  assert(coatheal::wire::UdpBytes(10) == 60 + 24);   // padded to the minimum frame
  assert(coatheal::wire::UdpBytes(100) == 142 + 24);
  assert(coatheal::kOnboardShareBytes + coatheal::kGroundShareBytes +
             coatheal::kUnaccountedBytes ==
         coatheal::kLinkCapBytes);
  assert(coatheal::kLinkCapBytes * 8 == 24000);
}

void TestLedgerSlidingWindow() {
  FakeClock clock;
  coatheal::LinkBudget budget(1000, 1s, clock.fn());
  assert(budget.TryCharge(600, coatheal::LinkPriority::kLive));
  assert(!budget.TryCharge(500, coatheal::LinkPriority::kLive));
  assert(budget.TryCharge(400, coatheal::LinkPriority::kLive));
  assert(budget.InWindow() == 1000);
  clock.advance(999ms);
  assert(!budget.TryCharge(1, coatheal::LinkPriority::kCommandReply));
  clock.advance(1ms);  // exactly one window after placement: gone
  assert(budget.InWindow() == 0);
  assert(budget.TryCharge(1000, coatheal::LinkPriority::kReplay));
  // Larger than the share: never admitted.
  clock.advance(2s);
  assert(!budget.TryCharge(1001, coatheal::LinkPriority::kCommandReply));
  coatheal::LinkBudget::Ticket t;
  assert(!budget.WaitHold(1001, coatheal::LinkPriority::kCommandReply, Clock::time_point::max(), &t));
}

void TestLedgerHoldsCountUntilReleasedPlusWindow() {
  FakeClock clock;
  coatheal::LinkBudget budget(1000, 1s, clock.fn());
  coatheal::LinkBudget::Ticket hold;
  assert(budget.TryHold(700, coatheal::LinkPriority::kLive, &hold));
  clock.advance(5s);
  assert(budget.InWindow() == 700);  // open holds never age out
  assert(!budget.TryCharge(400, coatheal::LinkPriority::kLive));
  // Refunding bytes that never left frees them at once.
  budget.Refund(hold, 300);
  assert(budget.TryCharge(400, coatheal::LinkPriority::kLive));
  // Released 50 ms in the future: still counting 1.05 s from now.
  budget.ReleaseAt(hold, clock.now() + 50ms);
  clock.advance(1049ms);
  assert(budget.InWindow() == 400);
  clock.advance(1ms);
  assert(budget.InWindow() == 0);

  // A part released now ages out a window later; the rest stays held.
  coatheal::LinkBudget::Ticket frame;
  assert(budget.TryHold(600, coatheal::LinkPriority::kLive, &frame));
  budget.ReleasePart(frame, 380, clock.now());
  clock.advance(1s);
  assert(budget.InWindow() == 220);
  budget.Release(frame);
  clock.advance(1s);
  assert(budget.InWindow() == 0);
}

void TestLedgerPriorityWaiterBlocksLessUrgent() {
  FakeClock clock;
  coatheal::LinkBudget budget(1000, 1s, clock.fn());
  assert(budget.TryCharge(900, coatheal::LinkPriority::kLive));
  std::atomic<bool> admitted{false};
  std::thread reply([&] {
    coatheal::LinkBudget::Ticket t;
    const bool ok = budget.WaitHold(500, coatheal::LinkPriority::kCommandReply,
                                    Clock::time_point::max(), &t);
    admitted = ok;
  });
  // Wait until the reply is registered as a waiter: a replay frame that
  // would fit (100 B) must now be refused.
  for (int i = 0; i < 200; ++i) {
    std::this_thread::sleep_for(1ms);
    if (!budget.TryCharge(0, coatheal::LinkPriority::kReplay)) break;
  }
  assert(!budget.TryCharge(100, coatheal::LinkPriority::kReplay));
  clock.advance(1s);  // the 900 B charge ages out
  budget.Notify();
  reply.join();
  assert(admitted);
  assert(budget.TryCharge(100, coatheal::LinkPriority::kReplay));
}

void TestCodec() {
#ifdef COATHEAL_HAS_ZLIB
  assert(coatheal::TelemetryCodecAvailable());
  // The embedded dictionary is byte-identical to the file the ground
  // station reads (CRC computed there with Python's zlib).
  assert(coatheal::TelemetryDictionaryZ1CrcHex() == "85904299");
  assert(coatheal::TelemetryDictionaryZ1().size() == 2375);

  const std::string line =
      "DATA,coatheal-1789498045-582267,547,2026-09-15T18:48:05Z,1,25.36,1007.96,0.00,26.10,24.29,"
      "24.33,24.37,24.01,24.51,24.25,24.06,HEATER_DUTY=1.000|1.000|1.000|0.000|0.000|0.000,"
      "RESISTANCE=-|-|-|-|-|-|-|-,PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|"
      "LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_FAIL|OVERTEMP_OK|ENERGY_OK|PWM_OK|"
      "STEPPER_OK|SAMPLE_TEMP_OK|REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK,"
      "SENSOR_VALID=AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1,"
      "SENSOR_AGE_MS=AT:692|AP:692|UV:430|S0:698|S1:698|S2:698|S3:698|S4:698|S5:698|S6:698|"
      "S7:698,COMPONENT_STATE=DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK,"
      "CTRL=fallback:0|link_loss_s:0.0|energy_wh:1.02|budget_wh:130.0|budget_exhausted:0|"
      "heaters_active:3|queue:0|plan:none|debug:0|tune:-,STEPPER0=pos:0|tgt:0|hz:50.00|us:4|"
      "ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:4800|missed:0|src:cmd:BEND_MM|zeroed:1|seq:-|"
      "seqst:idle|amps:0.00|acc:200.0|mm:0.000|mm_tgt:0.000|therm:ok,STEPPER1=pos:800|tgt:800|"
      "hz:50.00|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:4000|missed:0|src:cmd:BEND_MM|"
      "zeroed:1|seq:-|seqst:idle|amps:0.00|acc:200.0|mm:2.000|mm_tgt:2.000|therm:ok,TX=0";
  const std::string wire = coatheal::EncodeTelemetryLineZ1(line);
  assert(wire.rfind("Z1,", 0) == 0);
  assert(wire.size() < 320);  // 1156 B -> ~260 B
  std::string decoded;
  assert(coatheal::DecodeTelemetryLineZ1(wire, &decoded));
  assert(decoded == line);

  // Encoded by the ground station's Python zlib with the same parameters.
  const std::string from_python =
      "Z1,1dZNC8IwDAbgP6SSpk3W7uZQT26C4NmDXv3/V5e0w9UPHGUIXvpCGWkOy0Pe+BM8OFqSR+RqQa7K+PG18zVQ4scO/HD"
      "ih1cGHpuOtXrq7gPKD0WESG8448doc+OzgJ/d/rBO/hxP3VR7+pwkjxr15/ZwQCkj0RdyFqQUB115NDCGjeFiUAyOUUn8Uh"
      "zIuOn/DizgxiZuYBZrzLfxdR6eJvhyu9bNttuc23YYZPNxkKFUHXlWvpEsbR1mbR3HreOrQXc=";
  assert(coatheal::DecodeTelemetryLineZ1(from_python, &decoded));
  assert(decoded == line);

  assert(!coatheal::DecodeTelemetryLineZ1("Z1,@@@@", &decoded));
  assert(!coatheal::DecodeTelemetryLineZ1("Z1,", &decoded));
  assert(!coatheal::DecodeTelemetryLineZ1("DATA,plain", &decoded));
#else
  assert(!coatheal::TelemetryCodecAvailable());
  assert(coatheal::EncodeTelemetryLineZ1("DATA,x").empty());
#endif

  std::string bytes;
  assert(coatheal::Base64Encode("") == "");
  assert(coatheal::Base64Encode("f") == "Zg==");
  assert(coatheal::Base64Encode("fo") == "Zm8=");
  assert(coatheal::Base64Encode("foo") == "Zm9v");
  assert(coatheal::Base64Encode("foobar") == "Zm9vYmFy");
  assert(coatheal::Base64Decode("Zm9vYmE=", &bytes) && bytes == "fooba");
  assert(coatheal::Base64Decode("Zg==", &bytes) && bytes == "f");
  assert(!coatheal::Base64Decode("Zg=", &bytes));
  assert(!coatheal::Base64Decode("Z===", &bytes));
  assert(!coatheal::Base64Decode("Zg=a", &bytes));
}

void TestQueueBisectionOrder() {
  const auto dir = TempDir("bisect");
  std::string error;
  {
    coatheal::TelemetryQueue queue(dir.string(), 72.0, 16 * 1024 * 1024);
    assert(queue.Initialize(&error));
    assert(!queue.NextReplay(nullptr));
    for (std::uint64_t seq = 0; seq < 15; ++seq) {
      std::uint64_t index = 99;
      assert(queue.Enqueue(DataFrame("s", seq), &error, &index));
      assert(index == seq);
    }
    std::vector<std::uint64_t> order;
    coatheal::QueuedTelemetryFrame next;
    while (queue.NextReplay(&next)) {
      order.push_back(next.seq);
      assert(queue.AcknowledgeExact(next, &error));
    }
    // Middle, quarters, eighths, then the rest oldest first.
    const std::vector<std::uint64_t> expected = {7, 3, 11, 1, 5, 9, 13, 0, 2, 4, 6, 8, 10, 12, 14};
    assert(order == expected);
    // MUTATION: pick `start` instead of the middle in NextReplay and this
    // order becomes 0, 1, 2, ...
    assert(queue.size() == 0);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void TestQueueLiveFrameAndEventsAroundAnOutage() {
  const auto dir = TempDir("live");
  std::string error;
  coatheal::TelemetryQueue queue(dir.string(), 72.0, 16 * 1024 * 1024);
  assert(queue.Initialize(&error));
  for (std::uint64_t seq = 0; seq < 7; ++seq) assert(queue.Enqueue(DataFrame("s", seq), &error));
  coatheal::QueuedTelemetryFrame event;
  event.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
  event.session_id = "s";
  event.seq = 7;
  event.frame = "EVT,PULL,s,1,0,2026-09-15T18:48:05Z,800,5.00,0|1|2|3,4";
  std::uint64_t event_index = 0;
  assert(queue.Enqueue(event, &error, &event_index));
  std::uint64_t live_index = 0;
  assert(queue.Enqueue(DataFrame("s", 8), &error, &live_index));
  assert(live_index == 8);

  // The drain sends this tick's frame and the event first.
  coatheal::QueuedTelemetryFrame live;
  assert(queue.FrameAt(live_index, &live) && live.seq == 8);
  assert(queue.AcknowledgeExact(live, &error));
  assert(!queue.FrameAt(live_index, nullptr));
  const auto events = queue.PendingEvents(8);
  assert(events.size() == 1 && events[0].index == event_index);
  assert(queue.AcknowledgeExact(events[0], &error));
  assert(queue.PendingEvents(8).empty());

  // Left: the seven-frame outage, bisected from its middle.
  coatheal::QueuedTelemetryFrame next;
  assert(queue.NextReplay(&next) && next.seq == 3);
  // A frame acknowledged twice, or a stale copy with other text, changes nothing.
  coatheal::QueuedTelemetryFrame stale = next;
  stale.frame += ",changed";
  assert(queue.AcknowledgeExact(stale, &error));
  assert(queue.size() == 7);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void TestQueueReloadKeepsPendingFramesInOrder() {
  const auto dir = TempDir("reload");
  std::string error;
  {
    coatheal::TelemetryQueue queue(dir.string(), 72.0, 16 * 1024 * 1024);
    assert(queue.Initialize(&error));
    for (std::uint64_t seq = 0; seq < 5; ++seq) assert(queue.Enqueue(DataFrame("s", seq), &error));
    coatheal::QueuedTelemetryFrame middle;
    assert(queue.FrameAt(2, &middle));
    assert(queue.AcknowledgeExact(middle, &error));
  }
  {
    coatheal::TelemetryQueue queue(dir.string(), 72.0, 16 * 1024 * 1024);
    assert(queue.Initialize(&error));
    const auto pending = queue.PendingFrames();
    assert(pending.size() == 4);
    assert(pending[0].seq == 0 && pending[1].seq == 1 && pending[2].seq == 3 && pending[3].seq == 4);
    assert(pending[3].index == 3);
    coatheal::QueuedTelemetryFrame next;
    assert(queue.NextReplay(&next) && next.seq == 1);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// Stands in for TelemetryClient: charges the real ledger with the real frame
// cost, models what reaches the wire and when, and acknowledges every frame
// 5 ms later. Waiting for budget room advances the fake clock (live frames
// until 400 ms into the tick, replay until 700 ms) instead of sleeping.
// The wire as TelemetryClient::SendFrame drives it, on a fake clock: the
// frame leaves the moment its hold is admitted and the ground station's answer
// comes back 5 ms later -- the ACK line and our ACK of it, plus the ground
// station's TCP ACK as a frame of its own unless `merged`. Charges, refunds
// and releases follow SendFrame.
class WireModelSender : public coatheal::FrameSender {
 public:
  struct Emission {
    Clock::time_point at;
    std::uint32_t bytes;
  };

  WireModelSender(coatheal::LinkBudget* budget, FakeClock* clock, bool merged)
      : budget_(budget), clock_(clock), merged_(merged) {}

  coatheal::SendStatus SendFrame(const std::string& line, coatheal::LinkPriority priority,
                                 std::chrono::steady_clock::time_point budget_deadline,
                                 coatheal::TelemetryAck* ack) override {
    std::string payload = coatheal::TelemetryCodecAvailable()
                              ? coatheal::EncodeTelemetryLineZ1(line)
                              : line;
    payload.push_back('\n');
    const std::uint32_t segment = coatheal::wire::TcpBytes(payload.size());
    const std::uint32_t cost = coatheal::TelemetryClient::FrameCostBytes(line, payload.size());
    coatheal::LinkBudget::Ticket ticket;
    while (!budget_->TryHold(cost, priority, &ticket)) {
      if (clock_->now() >= budget_deadline) return coatheal::SendStatus::kNoBudget;
      clock_->advance(5ms);
    }
    emissions.push_back({clock_->now(), segment});
    budget_->ReleasePart(ticket, segment, clock_->now());
    clock_->advance(5ms);
    const std::uint32_t gs_ack = merged_ ? 0U : coatheal::wire::kPureAck;
    emissions.push_back({clock_->now(), cost - segment - 2 * coatheal::wire::kReset -
                                            coatheal::wire::kPureAck + gs_ack});
    budget_->Refund(ticket, 2 * coatheal::wire::kReset);
    if (merged_) budget_->Refund(ticket, coatheal::wire::kPureAck);
    budget_->ReleaseAt(ticket, clock_->now() + 10ms);

    std::vector<std::string> parts;
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
      const std::size_t comma = line.find(',', start);
      parts.push_back(line.substr(start, comma - start));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (parts[0] == "EVT") {
      ack->session_id = parts[2];
      ack->seq = 0;
    } else {
      ack->session_id = parts[1];
      ack->seq = std::stoull(parts[2]);
      sent_seqs.push_back(ack->seq);
    }
    return coatheal::SendStatus::kSent;
  }

  bool is_connected() const override { return true; }

  std::vector<Emission> emissions;
  std::vector<std::uint64_t> sent_seqs;

 private:
  coatheal::LinkBudget* budget_;
  FakeClock* clock_;
  bool merged_;
};

std::string RealisticLine(const std::string& session, std::uint64_t seq, int tick) {
  return "DATA," + session + "," + std::to_string(seq) + ",2026-09-15T18:" +
         std::to_string(10 + tick / 60) + ":" + std::to_string(10 + tick % 50) +
         "Z,1,25.36,1007.96,0.00,26.10,24.29,24.33,24.37,24.01,24.51,24.25,24.06,"
         "HEATER_DUTY=1.000|1.000|1.000|0.000|0.000|0.000,RESISTANCE=-|-|-|-|-|-|-|-,"
         "PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|"
         "P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|PWM_OK|STEPPER_OK|SAMPLE_TEMP_OK|"
         "REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK,SENSOR_VALID=AT:1|AP:1|UV:1|S0:1|"
         "S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1,SENSOR_AGE_MS=AT:692|AP:692|UV:430|S0:698|S1:698|"
         "S2:698|S3:698|S4:698|S5:698|S6:698|S7:698,COMPONENT_STATE=DPS310:OK|ADS1115:OK|"
         "SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK,CTRL=fallback:0|link_loss_s:0.0|"
         "energy_wh:1.02|budget_wh:130.0|budget_exhausted:0|heaters_active:3|queue:" +
         std::to_string(seq) + "|plan:none|debug:0|tune:-,STEPPER0=pos:0|tgt:0|hz:100.00|us:4|"
         "ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:4800|missed:0|src:cmd:BEND_MM|zeroed:1|seq:-|"
         "seqst:idle|amps:0.80|acc:400.0|mm:0.000|mm_tgt:0.000|therm:ok,STEPPER1=pos:0|tgt:0|"
         "hz:100.00|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:0|missed:0|src:init|zeroed:0|"
         "seq:-|seqst:idle|amps:0.80|acc:400.0|mm:0.000|mm_tgt:0.000|therm:ok";
}

void TestReplayDeadline() {
  const Clock::time_point tick(Clock::duration(1000s));
  assert(coatheal::ReplayDeadline(tick, 1s) == tick + 150ms);
  assert(coatheal::ReplayDeadline(tick, 2s) == tick + 1150ms);
  assert(coatheal::ReplayDeadline(tick, 10s) == tick + 7s);  // 70 % of a slow tick
  assert(coatheal::ReplayDeadline(tick, 500ms) < tick);      // faster than 1 Hz: no replay
}

void TestFrameCostCoversEveryAnswer() {
  const std::string line = "DATA,coatheal-1789498045-582267,123456,2026-09-15T18:48:05Z,1,25.36";
  const std::uint32_t ack_line = coatheal::wire::TcpBytes(std::string("ACK,coatheal-1789498045-582267,123456\n").size());
  // frame, its TCP ACK alone and the reset for a late one, the ACK line and
  // our ACK of it, our reset
  assert(coatheal::TelemetryClient::FrameCostBytes(line, 300) ==
         coatheal::wire::TcpBytes(300) + 90 + 84 + ack_line + 90 + 84);
  const std::string event = "EVT,PULL,coatheal-1789498045-582267,7,1,800,5.0,4|5";
  assert(coatheal::TelemetryClient::FrameCostBytes(event, 100) ==
         coatheal::wire::TcpBytes(100) + 90 + 84 +
             coatheal::wire::TcpBytes(std::string("ACK,coatheal-1789498045-582267,0\n").size()) +
             90 + 84);
}

void TestDrainKeepsEveryWindowUnderTheShare(bool merged) {
  const auto dir = TempDir(merged ? "drain_merged" : "drain_separate");
  std::string error;
  FakeClock clock;
  coatheal::LinkBudget budget(coatheal::kOnboardShareBytes, 1s, clock.fn());
  coatheal::TelemetryQueue queue(dir.string(), 72.0, 64 * 1024 * 1024);
  assert(queue.Initialize(&error));
  WireModelSender sender(&budget, &clock, merged);
  coatheal::TelemetryDrain drain(&queue, &sender, clock.fn());

  // A ten-minute outage: 600 frames queued with nothing sent.
  const std::string session = "coatheal-1789498045-582267";
  std::uint64_t seq = 0;
  for (; seq < 600; ++seq) {
    assert(queue.Enqueue({coatheal::CurrentUnixEpochSeconds(), session, seq,
                          RealisticLine(session, seq, 0), 0},
                         &error));
  }

  // Then 180 ticks at 1 Hz with a live link.
  std::size_t live_sent = 0;
  std::vector<std::uint64_t> replay_order;
  for (int tick = 0; tick < 180; ++tick) {
    const Clock::time_point tick_start(Clock::duration(1000s) + std::chrono::seconds(tick));
    assert(clock.now() <= tick_start + 60ms);
    clock.set(tick_start.time_since_epoch() + 60ms);  // after sensors and control
    std::uint64_t live_index = 0;
    const std::uint64_t live_seq = seq++;
    assert(queue.Enqueue({coatheal::CurrentUnixEpochSeconds(), session, live_seq,
                          RealisticLine(session, live_seq, tick), 0},
                         &error, &live_index));
    const std::size_t before = sender.sent_seqs.size();
    const coatheal::DrainResult result =
        drain.Drain(live_index, coatheal::CurrentUnixEpochSeconds(), tick_start + 400ms,
                    coatheal::ReplayDeadline(tick_start, 1s));
    if (!(result.link_ok && !result.error && sender.sent_seqs.size() > before)) {
      std::cerr << "tick " << tick << " link_ok=" << result.link_ok << " error=" << result.error
                << " sent=" << result.sent << " in_window=" << budget.InWindow()
                << " fake_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count()
                << " pending=" << queue.size() << "\n";
    }
    assert(result.link_ok);
    assert(!result.error);
    assert(sender.sent_seqs.size() > before);
    if (sender.sent_seqs[before] == live_seq) ++live_sent;
    for (std::size_t i = before + 1; i < sender.sent_seqs.size(); ++i) {
      replay_order.push_back(sender.sent_seqs[i]);
    }
  }

  // Every tick's frame went out on its own tick: replay never took its room.
  assert(live_sent == 180);
  // The replay kept moving with the spare budget...
  assert(replay_order.size() >= 90);
  // ...in bisection order: the middle of the outage, then the middles of
  // the two halves (the longer, later half first).
  assert(replay_order[0] == 299);
  assert(replay_order[1] == 449 && replay_order[2] == 149);

  // No 1-second window on the wire carried more than the onboard share.
  std::uint64_t worst = 0;
  std::deque<WireModelSender::Emission> window;
  std::uint64_t in_window = 0;
  for (const auto& e : sender.emissions) {
    window.push_back(e);
    in_window += e.bytes;
    while (window.front().at <= e.at - 1s) {
      in_window -= window.front().bytes;
      window.pop_front();
    }
    worst = std::max(worst, in_window);
  }
  std::cout << "[link_budget] " << (merged ? "ACK in the ACK line" : "separate TCP ACK")
            << ": worst 1-s window " << worst << " B of " << coatheal::kOnboardShareBytes
            << " B, replayed " << replay_order.size() << " frames in 180 s\n";
  assert(worst <= coatheal::kOnboardShareBytes);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

#ifndef _WIN32
// A scripted ground station on loopback for the real TelemetryClient socket
// path: answers (or ignores) the HELLO, then ACKs (or sits on) one frame.
class LoopbackGround {
 public:
  enum class Mode { kCodec, kOldGround, kSlowAck, kWrongAck };

  explicit LoopbackGround(Mode mode) : mode_(mode) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(listen_fd_, 1) == 0);
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~LoopbackGround() {
    thread_.join();
    close(listen_fd_);
  }

  int port() const { return port_; }
  std::string hello;
  std::string frame_line;

 private:
  std::string ReadLine(int fd) {
    std::string line;
    char c = 0;
    while (recv(fd, &c, 1, 0) == 1) {
      if (c == '\n') return line;
      line.push_back(c);
    }
    return line;
  }

  void Run() {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    assert(fd >= 0);
    hello = ReadLine(fd);
    if (mode_ != Mode::kOldGround) {
      const std::string reply = "HELLO,z1\n";
      send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    }
    frame_line = ReadLine(fd);
    if (frame_line.empty()) {
      // The onboard reset the connection instead of sending a frame.
      close(fd);
      return;
    }
    if (mode_ == Mode::kSlowAck) {
      std::this_thread::sleep_for(400ms);  // past the 180 ms ACK deadline
    } else {
      std::string decoded = frame_line;
      if (frame_line.rfind("Z1,", 0) == 0) {
        assert(coatheal::DecodeTelemetryLineZ1(frame_line, &decoded));
      }
      const std::size_t a = decoded.find(',');
      const std::size_t b = decoded.find(',', a + 1);
      std::string seq = decoded.substr(b + 1, decoded.find(',', b + 1) - b - 1);
      if (mode_ == Mode::kWrongAck) seq = std::to_string(std::stoull(seq) - 1);
      const std::string ack = "ACK," + decoded.substr(a + 1, b - a - 1) + "," + seq + "\n";
      send(fd, ack.data(), ack.size(), MSG_NOSIGNAL);
    }
    std::this_thread::sleep_for(50ms);
    close(fd);
  }

  Mode mode_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
};

void TestTelemetryClientOverLoopback() {
  const std::string line = "DATA,coatheal-1789498045-582267,12,2026-09-15T18:48:05Z,1,25.36,TX=0";
  for (const auto mode : {LoopbackGround::Mode::kCodec, LoopbackGround::Mode::kOldGround,
                          LoopbackGround::Mode::kSlowAck, LoopbackGround::Mode::kWrongAck}) {
    LoopbackGround ground(mode);
    coatheal::LinkBudget budget(coatheal::kOnboardShareBytes);
    coatheal::TelemetryClient client("127.0.0.1", ground.port(), 5000, 1000,
                                     /*discovery_enabled=*/false, 4100, "", "");
    client.SetLinkBudget(&budget);
    coatheal::TelemetryAck ack;
    const auto status = client.SendFrame(line, coatheal::LinkPriority::kLive,
                                         std::chrono::steady_clock::now() + 500ms, &ack);
    assert(ground.hello.rfind("HELLO," + client.session_id() + ",z1:", 0) == 0);
    if (mode == LoopbackGround::Mode::kSlowAck) {
      assert(status == coatheal::SendStatus::kFailed);
      assert(client.ack_timeouts() == 1);
      assert(!client.is_connected());
      // The frame and the reset stay counted; nothing else was spent.
      assert(budget.InWindow() > 0 && budget.InWindow() <= coatheal::kOnboardShareBytes);
      continue;
    }
    if (mode == LoopbackGround::Mode::kWrongAck) {
      // An answer to another frame: the stream is out of step, so the
      // connection is reset rather than read on.
      assert(status == coatheal::SendStatus::kFailed);
      assert(client.ack_timeouts() == 0);
      assert(!client.is_connected());
      continue;
    }
    assert(status == coatheal::SendStatus::kSent);
    assert(ack.session_id == "coatheal-1789498045-582267" && ack.seq == 12);
#ifdef COATHEAL_HAS_ZLIB
    if (mode == LoopbackGround::Mode::kCodec) {
      assert(client.link_codec() == "z1");
      assert(ground.frame_line.rfind("Z1,", 0) == 0);
    } else {
      // A ground station that never answers the HELLO gets plain lines.
      assert(client.link_codec() == "plain");
      assert(ground.frame_line == line);
    }
#endif
    assert(budget.InWindow() > 0);
  }
}

// A ground station that does not take the codec cannot carry a full DATA
// line at all: the link is dropped rather than held open carrying nothing.
// (Holding it would also hide a ground station that has gone away, whose
// close is only seen on a write.)
void TestPlainGroundStationLosesTheLink() {
  LoopbackGround ground(LoopbackGround::Mode::kOldGround);  // never answers the HELLO
  coatheal::LinkBudget budget(coatheal::kOnboardShareBytes);
  coatheal::TelemetryClient client("127.0.0.1", ground.port(), 5000, 1000,
                                   /*discovery_enabled=*/false, 4100, "", "");
  client.SetLinkBudget(&budget);
  const std::string line = "DATA,coatheal-1789498045-582267,12,2026-09-15T18:48:05Z,1," +
                           std::string(1100, 'x');
  coatheal::TelemetryAck ack;
  const auto status = client.SendFrame(line, coatheal::LinkPriority::kLive,
                                       std::chrono::steady_clock::now() + 500ms, &ack);
  assert(status == coatheal::SendStatus::kFailed);
  assert(!client.is_connected());
  assert(client.ack_timeouts() == 0);  // nothing was sent, so nothing timed out
  assert(ground.frame_line.empty());
  // The retry waits, so the link does not churn against a ground station
  // that cannot take the codec.
  assert(client.SendFrame(line, coatheal::LinkPriority::kLive,
                          std::chrono::steady_clock::now() + 100ms,
                          &ack) == coatheal::SendStatus::kNotConnected);
  assert(budget.InWindow() <= coatheal::kOnboardShareBytes);
}

// A reply longer than one chunk goes out in paced chunks: complete, but only
// as fast as the onboard share lets it (the third chunk waits for the first
// one's second to pass).
void TestCommandReplyIsPacedInChunks() {
  int probe = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  socklen_t len = sizeof(addr);
  getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  close(probe);

  const std::string reply = "ACK,BIG," + std::string(1492, 'x');  // 1 501 B with the newline
  coatheal::LinkBudget budget(coatheal::kOnboardShareBytes);
  coatheal::CommandServer server(port);
  server.SetLinkBudget(&budget);
  server.SetPaceLoopbackForTesting(true);
  std::string error;
  assert(server.Start([&](const std::string&, const std::string&) { return reply; }, &error));

  int fd = -1;
  for (int attempt = 0; attempt < 50 && fd < 0; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      fd = -1;
      std::this_thread::sleep_for(20ms);
    }
  }
  assert(fd >= 0);
  const auto started = std::chrono::steady_clock::now();
  const std::string request = "BIG\n";
  assert(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
  std::string received;
  char buf[512];
  for (ssize_t n; (n = recv(fd, buf, sizeof(buf), 0)) > 0;) received.append(buf, buf + n);
  close(fd);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  server.Stop();

  assert(received == reply + "\n");
  // 600 + 90, 600 + 180 and 301 + 180 settled bytes do not fit one second.
  assert(elapsed >= 900ms);
  assert(budget.InWindow() <= coatheal::kOnboardShareBytes);
}
#endif

}  // namespace

int main() {
  TestWireModel();
  TestLedgerSlidingWindow();
  TestLedgerHoldsCountUntilReleasedPlusWindow();
  TestLedgerPriorityWaiterBlocksLessUrgent();
  TestCodec();
  TestQueueBisectionOrder();
  TestQueueLiveFrameAndEventsAroundAnOutage();
  TestQueueReloadKeepsPendingFramesInOrder();
  TestReplayDeadline();
  TestFrameCostCoversEveryAnswer();
  TestDrainKeepsEveryWindowUnderTheShare(/*merged=*/true);
  TestDrainKeepsEveryWindowUnderTheShare(/*merged=*/false);
#ifndef _WIN32
  TestTelemetryClientOverLoopback();
  TestPlainGroundStationLosesTheLink();
  TestCommandReplyIsPacedInChunks();
#endif
  std::cout << "[link_budget] all tests passed\n";
  return 0;
}
