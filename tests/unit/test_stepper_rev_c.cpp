// Rev C stepper unit tests.
//
// Covers the dual-motor motion requirements:
//   (a) trapezoidal accel/decel curve is monotonic ramp-up then ramp-down,
//   (b) commands with id argument parse correctly,
//   (c) id defaulting to 0 for legacy (no-id) form,
//   (d) max_step_hz ceiling is enforced by SetSpeed,
//   (e) MotionLock: two motors TryAcquire -> second returns false.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coatheal/command.hpp"
#include "coatheal/command_parser.hpp"
#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/motion_lock.hpp"
#include "coatheal/stepper_channel.hpp"
#include "coatheal/stepper_controller.hpp"

using namespace coatheal;

namespace {

StepperChannelConfig MakeChannelCfg(int id,
                                    std::vector<std::size_t> samples = {0, 1, 2, 3}) {
  StepperChannelConfig c;
  c.channel_id = id;
  c.full_steps_per_rev = 200;
  c.max_step_hz = 100.0;
  c.default_step_hz = 100.0;
  c.accel_steps_per_s2 = 200.0;  // 0.5 s ramp to 100 Hz
  c.microstep = 4;
  c.max_position_steps = 200000;
  c.samples = std::move(samples);
  c.pull_travel_full_steps = 200;
  c.pull_hold_s = 0.1;  // short hold so tests finish fast
  c.enable_on_boot = true;
  c.use_pulse_thread = false;
  return c;
}

std::unique_ptr<StepperChannel> MakeChannel(int id, MotionLock* lock = nullptr) {
  auto drv = std::make_unique<SimulatedStepperDriver>();
  return std::make_unique<StepperChannel>(MakeChannelCfg(id), std::move(drv), lock);
}

// (a) Trapezoidal accel/decel — ramp up then ramp down, monotonic both sides.
//
// We ticks the channel at 1 kHz (dt=0.001 s) and bucket pulse counts into
// 50 ms windows. Each window's pulse count approximates the instantaneous
// ustep rate, which under a trapezoidal profile should: rise, plateau near
// max, then fall. We assert the (smoothed) rate profile is non-decreasing
// up to a peak and non-increasing afterwards.
void TestTrapezoidalRamp() {
  auto ch = MakeChannel(0);
  std::string err;
  const std::int64_t target_usteps = 800;  // 200 full-steps × 4 microstep
  assert(ch->MoveToSteps(target_usteps, 0.0, &err));

  // Run for up to 5 s at 1 ms ticks.
  constexpr double dt = 0.001;
  constexpr int kWindow = 50;  // 50 ms = 50 ticks
  std::vector<int> window_counts;
  std::int64_t last_pos = 0;
  int window_pulses = 0;
  int ticks_in_window = 0;
  int total_ticks = 0;
  for (int i = 0; i < 5000; ++i) {
    ch->Tick(dt);
    auto s = ch->Snapshot();
    const std::int64_t delta = s.position_steps - last_pos;
    last_pos = s.position_steps;
    window_pulses += static_cast<int>(delta);
    ++ticks_in_window;
    ++total_ticks;
    if (ticks_in_window == kWindow) {
      window_counts.push_back(window_pulses);
      window_pulses = 0;
      ticks_in_window = 0;
    }
    if (!s.moving && s.position_steps == target_usteps) break;
  }

  assert(last_pos == target_usteps);

  // Expected peak window count: 100 full-step × 4 microstep × 0.05 s = 20.
  // Compute the smoothed profile as a sliding 3-window average to further
  // damp quantisation noise.
  std::vector<double> smoothed;
  for (std::size_t i = 1; i + 1 < window_counts.size(); ++i) {
    smoothed.push_back((window_counts[i - 1] + window_counts[i] +
                         window_counts[i + 1]) /
                        3.0);
  }
  assert(!smoothed.empty());

  std::size_t peak = 0;
  for (std::size_t i = 1; i < smoothed.size(); ++i) {
    if (smoothed[i] > smoothed[peak]) peak = i;
  }
  // A trapezoid must have a ramp-up and a ramp-down — the peak can't be
  // at either end.
  assert(peak > 0);
  assert(peak < smoothed.size() - 1);

  // Non-strict monotonicity on each side (tolerate ≤1.0 jitter for
  // quantisation). Allow the peak plateau to span several windows (cruise).
  for (std::size_t i = 1; i <= peak; ++i) {
    if (smoothed[i] + 1.0 < smoothed[i - 1]) {
      std::cerr << "[ramp-up] violation at i=" << i
                << " prev=" << smoothed[i - 1]
                << " cur=" << smoothed[i] << '\n';
      assert(false);
    }
  }
  for (std::size_t i = peak + 1; i < smoothed.size(); ++i) {
    if (smoothed[i] > smoothed[i - 1] + 1.0) {
      std::cerr << "[ramp-down] violation at i=" << i
                << " prev=" << smoothed[i - 1]
                << " cur=" << smoothed[i] << '\n';
      assert(false);
    }
  }

  // Peak smoothed count should be near the theoretical 20 ustep/window.
  // Be generous: 15..25 covers quantisation jitter across 3 averaged bins.
  assert(smoothed[peak] >= 15.0);
  assert(smoothed[peak] <= 25.0);
}

// (b) Parser: commands with explicit id argument.
void TestParserIdArgument() {
  CommandParser parser;

  auto r = parser.ParseLine("STEPPER_MOVE 1 400");
  assert(r.ok);
  assert(r.command.type == CommandType::kStepperMove);
  assert(r.command.motor_id == 1);
  assert(r.command.args.size() == 1);
  assert(r.command.args[0] == "400");

  auto r2 = parser.ParseLine("STEPPER_MOVETO 0 1600 5.0");
  assert(r2.ok);
  assert(r2.command.type == CommandType::kStepperMoveTo);
  assert(r2.command.motor_id == 0);
  assert(r2.command.args.size() == 2);
  assert(r2.command.args[0] == "1600");
  assert(r2.command.args[1] == "5.0");

  auto r3 = parser.ParseLine("STEPPER_SET_SPEED 1 50");
  assert(r3.ok);
  assert(r3.command.motor_id == 1);
  assert(r3.command.args.size() == 1);
  assert(r3.command.args[0] == "50");

  auto r4 = parser.ParseLine("STEPPER_HOME 1");
  assert(r4.ok);
  assert(r4.command.motor_id == 1);
  assert(r4.command.args.empty());

  auto r5 = parser.ParseLine("PULL_ARM 0");
  assert(r5.ok);
  assert(r5.command.type == CommandType::kPullArm);
  assert(r5.command.motor_id == 0);

  auto r6 = parser.ParseLine("PULL_EXECUTE 1");
  assert(r6.ok);
  assert(r6.command.type == CommandType::kPullExecute);
  assert(r6.command.motor_id == 1);
}

// (c) Parser: legacy form (no id) defaults to id=0.
void TestParserLegacyDefault() {
  CommandParser parser;

  auto r = parser.ParseLine("STEPPER_MOVE 400");
  assert(r.ok);
  assert(r.command.motor_id == 0);
  assert(r.command.args.size() == 1);
  assert(r.command.args[0] == "400");

  auto r2 = parser.ParseLine("STEPPER_MOVETO 1600");
  assert(r2.ok);
  assert(r2.command.motor_id == 0);
  assert(r2.command.args.size() == 1);
  assert(r2.command.args[0] == "1600");

  auto r3 = parser.ParseLine("STEPPER_BEND 500 10");
  assert(r3.ok);
  assert(r3.command.motor_id == 0);
  assert(r3.command.args.size() == 2);
  assert(r3.command.args[0] == "500");
  assert(r3.command.args[1] == "10");

  auto r4 = parser.ParseLine("STEPPER_HOME");
  assert(r4.ok);
  assert(r4.command.motor_id == 0);
  assert(r4.command.args.empty());

  auto r5 = parser.ParseLine("PULL_ARM");
  assert(r5.ok);
  assert(r5.command.motor_id == 0);
}

// (d) max_step_hz ceiling — SetSpeed clamps (does not reject) and Snapshot
// reflects the clamped value.
void TestMaxStepHzCeiling() {
  auto ch = MakeChannel(0);
  std::string err;

  // Below ceiling — accepted and stored verbatim.
  assert(ch->SetSpeed(50.0, &err));
  assert(ch->Snapshot().step_hz == 50.0);

  // Request above ceiling — clamped to cfg.max_step_hz (=100).
  assert(ch->SetSpeed(500.0, &err));
  assert(ch->Snapshot().step_hz == 100.0);

  // Zero or negative rejected with an error.
  assert(!ch->SetSpeed(0.0, &err));
  assert(!ch->SetSpeed(-10.0, &err));
}

// (e) MotionLock: one motor acquires, second attempt fails until release.
void TestMotionLockExclusion() {
  MotionLock lock;
  assert(lock.holder() == -1);

  assert(lock.TryAcquire(0));
  assert(lock.holder() == 0);

  assert(!lock.TryAcquire(1));  // second motor must be rejected
  assert(lock.holder() == 0);

  // Same holder re-acquire is idempotent.
  assert(lock.TryAcquire(0));
  assert(lock.holder() == 0);

  lock.Release(0);
  assert(lock.holder() == -1);

  // Now motor 1 can take it.
  assert(lock.TryAcquire(1));
  assert(lock.holder() == 1);
  // Motor 0 is now locked out.
  assert(!lock.TryAcquire(0));
  lock.Release(1);
  assert(lock.holder() == -1);
}

// Bonus coverage: StepperChannel integrates with MotionLock for pull cycles,
// and samples() returns the configured mapping.
void TestPullCycleAcquiresLock() {
  MotionLock lock;
  auto ch0 = MakeChannel(0, &lock);
  auto ch1 = MakeChannel(1, &lock);

  // Verify samples mapping — we built both with 0..3 by default; overwrite
  // channel 1's config here to model the Rev C 4..7 split.
  // (We re-create ch1 with explicit samples to keep the test self-contained.)
  auto drv1 = std::make_unique<SimulatedStepperDriver>();
  auto cfg1 = MakeChannelCfg(1, {4, 5, 6, 7});
  auto ch1b = std::make_unique<StepperChannel>(std::move(cfg1),
                                               std::move(drv1), &lock);
  assert(ch0->samples() == std::vector<std::size_t>({0, 1, 2, 3}));
  assert(ch1b->samples() == std::vector<std::size_t>({4, 5, 6, 7}));

  // Arm a pull on motor 0 — lock is taken.
  std::string err;
  assert(ch0->ArmPullCycle(&err));
  assert(lock.holder() == 0);

  // Motor 1 cannot arm until motor 0's cycle completes.
  assert(!ch1b->ArmPullCycle(&err));

  // Pump ticks on motor 0 to completion.
  for (int i = 0; i < 30000; ++i) {
    ch0->Tick(0.001);
    if (lock.holder() == -1) break;
  }
  assert(lock.holder() == -1);

  // Now motor 1 can take the lock.
  assert(ch1b->ArmPullCycle(&err));
  assert(lock.holder() == 1);
}

// Bonus: controller multi-motor dispatch routes by id.
void TestControllerMultiChannelDispatch() {
  std::vector<StepperChannelConfig> cfgs;
  cfgs.push_back(MakeChannelCfg(0, {0, 1, 2, 3}));
  cfgs.push_back(MakeChannelCfg(1, {4, 5, 6, 7}));
  std::vector<std::unique_ptr<StepperDriver>> drvs;
  drvs.emplace_back(std::make_unique<SimulatedStepperDriver>());
  drvs.emplace_back(std::make_unique<SimulatedStepperDriver>());

  BendScheduleConfig sched;  // default zeros; no phase interference
  StepperController ctl(std::move(cfgs), std::move(drvs), sched);
  assert(ctl.channel_count() == 2);
  assert(ctl.SamplesForMotor(0) == std::vector<std::size_t>({0, 1, 2, 3}));
  assert(ctl.SamplesForMotor(1) == std::vector<std::size_t>({4, 5, 6, 7}));

  std::string err;
  assert(ctl.MoveSteps(1, 400, &err));
  assert(ctl.Snapshot(1).target_steps == 400);
  assert(ctl.Snapshot(0).target_steps == 0);  // untouched

  assert(ctl.MoveSteps(0, 200, &err));
  assert(ctl.Snapshot(0).target_steps == 200);
  assert(ctl.Snapshot(1).target_steps == 400);

  // Unknown motor id rejected.
  assert(!ctl.MoveSteps(9, 100, &err));
}

}  // namespace

int main() {
  TestTrapezoidalRamp();
  TestParserIdArgument();
  TestParserLegacyDefault();
  TestMaxStepHzCeiling();
  TestMotionLockExclusion();
  TestPullCycleAcquiresLock();
  TestControllerMultiChannelDispatch();
  std::cout << "Rev C stepper tests passed" << std::endl;
  return 0;
}
