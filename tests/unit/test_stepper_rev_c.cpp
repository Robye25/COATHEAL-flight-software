// Rev C stepper unit tests.
//
// Covers the dual-motor motion requirements:
//   (a) trapezoidal accel/decel curve is monotonic ramp-up then ramp-down,
//   (b) commands with id argument parse correctly,
//   (c) id defaulting to 0 for legacy (no-id) form,
//   (d) max_step_hz ceiling is enforced by SetSpeed,
//   (e) MotionLock: two motors TryAcquire -> second returns false.

#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdint>
#include <iostream>
#include <limits>
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
// The pulse thread must ramp in REAL time. It used to feed the ramp a fixed
// 1 ms per iteration while each iteration is one pulse plus a sleep of one
// pulse period, so acceleration was applied per step: a 400-microstep move
// at 100 Hz / 200 steps/s^2 took ~7 s of crawl instead of ~1.3 s (bench,
// 2026-08-29: "the motors never move").
void TestPulseThreadRampsInRealTime() {
  StepperChannelConfig cfg = MakeChannelCfg(0);
  cfg.use_pulse_thread = true;
  auto ch = std::make_unique<StepperChannel>(cfg, std::make_unique<SimulatedStepperDriver>(), nullptr);
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  assert(ch->MoveToSteps(400, 0.0, &err));
  double elapsed = 0.0;
  while (elapsed < 6.0) {
    ch->Tick(0.05);  // the control loop only wakes the thread in this mode
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (ch->Snapshot().position_steps == 400) break;
  }
  assert(ch->Snapshot().position_steps == 400);
  // 100 full steps: 0.5 s ramp covering 25 steps, ~75 steps at 100 Hz, a
  // short brake -- about 1.3 s. Per-step acceleration needed ~7 s.
  assert(elapsed > 0.8);
  assert(elapsed < 3.0);
  // MUTATION: restore `UpdateRampSpeed(0.001, ...)` in PulseThreadBody and
  // confirm this test fails on `elapsed < 3.0`.
}

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

// Regression: STEPPER_BEND/STEPPER_MOVETO's (1,2) legacy/new arity ranges
// overlap at n == legacy_max + 1 (2 args) -- arity alone cannot tell
// "<target> <hold>" from "<id> <target>" apart, since both are two bare
// integers. Disambiguation is on plausibility: the leading token is only
// treated as a motor id if it also parses into [0, motor_count). A step
// count like 500 is never a plausible id (default motor_count == 2) and
// falls through to the legacy reading; a real motor id does not.
void TestBendArityDisambiguation() {
  CommandParser parser;
  // Legacy two-arg form: both tokens are payload, id defaults to 0.
  auto legacy = parser.ParseLine("STEPPER_BEND 500 10");
  assert(legacy.ok);
  assert(legacy.command.motor_id == 0);
  assert(legacy.command.args.size() == 2);
  assert(legacy.command.args[0] == "500");
  assert(legacy.command.args[1] == "10");

  // A two-digit leading token must still not be eaten in the legacy form
  // when it isn't a plausible motor id.
  auto legacy_small = parser.ParseLine("STEPPER_BEND 50 10");
  assert(legacy_small.ok);
  assert(legacy_small.command.motor_id == 0);
  assert(legacy_small.command.args.size() == 2);

  // Indexed three-arg form: leading token is the motor id.
  auto indexed = parser.ParseLine("STEPPER_BEND 1 500 10");
  assert(indexed.ok);
  assert(indexed.command.motor_id == 1);
  assert(indexed.command.args.size() == 2);
  assert(indexed.command.args[0] == "500");
  assert(indexed.command.args[1] == "10");

  // Indexed two-arg form with no hold -- this is what the ground-station
  // MOVETO button sends (panels_control.py:695) and what protocol.md
  // documents. Regression guard: an arity-only rule silently read this as
  // motor 0, 1 microstep, 800 s hold.
  auto indexed_no_hold = parser.ParseLine("STEPPER_MOVETO 1 800");
  assert(indexed_no_hold.ok);
  assert(indexed_no_hold.command.motor_id == 1);
  assert(indexed_no_hold.command.args.size() == 1);
  assert(indexed_no_hold.command.args[0] == "800");

  // Leading token outside the configured motor range is not an id, so the
  // three-arg form fails the arity check loudly instead of driving motor 0.
  auto out_of_range = parser.ParseLine("STEPPER_MOVETO 9 800 10");
  assert(!out_of_range.ok);

  // STEPPER_MOVE takes maybe_extract_id(1,1) -- verify the restored
  // inclusive band did not regress the unambiguous indexed form.
  auto move_indexed = parser.ParseLine("STEPPER_MOVE 1 400");
  assert(move_indexed.ok);
  assert(move_indexed.command.motor_id == 1);
  assert(move_indexed.command.args.size() == 1);
  assert(move_indexed.command.args[0] == "400");
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

  // Re-acquire by the same holder without releasing first MUST fail — this
  // is a hard flight rule (see motion_lock.hpp), not an idempotent no-op.
  // Matches the paranoid contract enforced in test_safety_rev_c.cpp.
  assert(!lock.TryAcquire(0));
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

// A driver that refuses to disable. SimulatedStepperDriver's Enable() always
// succeeds, so this is the only way to reach StepperChannel::SetEnabled's
// failure path; everything else mirrors the simulated driver.
class RefusesToDisableStepperDriver : public StepperDriver {
 public:
  bool Enable(bool enable) override {
    if (!enable) return false;  // disable always fails
    enabled_ = true;
    return true;
  }
  bool Step(bool direction_forward) override {
    (void)direction_forward;
    ++pulses_;
    return true;
  }
  void SetMicrostep(int divisor) override { (void)divisor; }
  bool healthy() const override { return true; }
  std::uint64_t pulses_issued() const override { return pulses_; }

 private:
  bool enabled_ = false;
  std::uint64_t pulses_ = 0;
};

// SetEnabled(false) must tear the channel down even when the driver refuses.
//
// HeaterScheduler clamps every heater duty to 0 whenever MotionLock is active,
// and the lock is released from SetEnabled(false) (and from motion
// completing). The old early-return left enabled_ true and the lock latched
// with no motion left to release it: all six heaters forced off and the other
// motor locked out indefinitely, because a driver already known to be broken
// returned false.
void TestSetEnabledFalseReleasesLockEvenWhenDriverRefuses() {
  MotionLock lock;
  auto cfg = MakeChannelCfg(0);
  auto ch = std::make_unique<StepperChannel>(
      std::move(cfg), std::make_unique<RefusesToDisableStepperDriver>(), &lock);

  std::string err;
  assert(ch->ArmPullCycle(&err));
  assert(lock.holder() == 0);
  assert(ch->Snapshot().enabled);

  // The driver refuses, so the call must still report failure...
  assert(!ch->SetEnabled(false));
  // ...but the channel is torn down anyway. The lock is asserted FIRST
  // because it is the consequence that matters: HeaterScheduler clamps every
  // duty while the lock is active, so a latched lock means all six heaters
  // off indefinitely.
  assert(lock.holder() == -1);
  assert(!lock.is_active());
  assert(!ch->Snapshot().enabled);
  assert(!ch->Snapshot().moving);

  // A second motor can now take the lock -- the observable consequence.
  auto ch1 = MakeChannel(1, &lock);
  assert(ch1->ArmPullCycle(&err));
  assert(lock.holder() == 1);
}

// Opposite direction: an Enable(TRUE) failure must NOT record the channel as
// enabled. Isolating this from the case above is the point -- the two
// directions have deliberately different policies.
void TestSetEnabledTrueFailureLeavesChannelDisabled() {
  class RefusesToEnableStepperDriver : public StepperDriver {
   public:
    bool Enable(bool enable) override { return enable ? false : true; }
    bool Step(bool) override { return true; }
    void SetMicrostep(int) override {}
    bool healthy() const override { return true; }
    std::uint64_t pulses_issued() const override { return 0; }
  };

  MotionLock lock;
  auto cfg = MakeChannelCfg(0);
  cfg.enable_on_boot = false;
  auto ch = std::make_unique<StepperChannel>(
      std::move(cfg), std::make_unique<RefusesToEnableStepperDriver>(), &lock);

  assert(!ch->Snapshot().enabled);
  assert(!ch->SetEnabled(true));
  assert(!ch->Snapshot().enabled);
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

  StepperController ctl(std::move(cfgs), std::move(drvs));
  assert(ctl.channel_count() == 2);
  assert(ctl.SamplesForMotor(0) == std::vector<std::size_t>({0, 1, 2, 3}));
  assert(ctl.SamplesForMotor(1) == std::vector<std::size_t>({4, 5, 6, 7}));

  std::string err;
  assert(ctl.MoveSteps(1, 400, &err));
  assert(ctl.Snapshot(1).target_steps == 400);
  assert(ctl.Snapshot(0).target_steps == 0);  // untouched

  // MotionLock serializes all motion, including ordinary manual jogs.
  assert(!ctl.MoveSteps(0, 200, &err));
  for (int i = 0; i < 10000 && ctl.Snapshot(1).moving; ++i) {
    ctl.Tick(MissionPhase::kAscent, 0.001);
  }
  assert(!ctl.Snapshot(1).moving);
  assert(ctl.MoveSteps(0, 200, &err));
  assert(ctl.Snapshot(0).target_steps == 200);
  assert(ctl.Snapshot(1).target_steps == 400);

  ctl.Stop(0, &err);
  assert(ctl.SetMicrostep(0, 64, &err));
  assert(ctl.Snapshot(0).microstep == 64);
  assert(!ctl.SetMicrostep(0, 5, &err));
  assert(ctl.SetPositionZero(1, &err));
  assert(ctl.Snapshot(1).position_steps == 0);
  assert(ctl.Snapshot(1).target_steps == 0);

  // Unknown motor id rejected.
  assert(!ctl.MoveSteps(9, 100, &err));
}

// A module the bus reaches perfectly but that cannot drive its motor --
// the shape of a TMC5160 strapped for STEP/DIR, or reporting a foreign
// VERSION byte. Every datagram succeeds; the motor is unusable.
class UnusableButReachableDriver : public StepperDriver {
 public:
  static constexpr const char* kReason =
      "SD_MODE strapped HIGH -- driver is in STEP/DIR mode";

  bool Enable(bool enable) override { (void)enable; return false; }
  std::string last_error() const override { return kReason; }
  bool Step(bool direction_forward) override {
    (void)direction_forward;
    return false;
  }
  void SetMicrostep(int divisor) override { (void)divisor; }
  bool healthy() const override { return false; }
  bool spi_bus_ok() const override { return true; }
  std::uint64_t pulses_issued() const override { return 0; }
};

// The other half: the bus itself is gone (device not open, CS line dead,
// ioctl failing), so nothing can be said about the motor at all.
class DeadBusDriver : public StepperDriver {
 public:
  bool Enable(bool enable) override { (void)enable; return false; }
  bool Step(bool direction_forward) override {
    (void)direction_forward;
    return false;
  }
  void SetMicrostep(int divisor) override { (void)divisor; }
  bool healthy() const override { return false; }
  bool spi_bus_ok() const override { return false; }
  std::uint64_t pulses_issued() const override { return 0; }
};

StepperChannelConfig BusTestChannelConfig() {
  StepperChannelConfig cfg;
  cfg.channel_id = 0;
  cfg.full_steps_per_rev = 200;
  cfg.microstep = 1;
  cfg.max_step_hz = 1000.0;
  cfg.default_step_hz = 100.0;
  cfg.max_position_steps = 100000;
  cfg.use_pulse_thread = false;
  return cfg;
}

std::unique_ptr<StepperController> MakeBusTestController(
    std::unique_ptr<StepperDriver> driver) {
  std::vector<StepperChannelConfig> cfgs{BusTestChannelConfig()};
  std::vector<std::unique_ptr<StepperDriver>> drivers;
  drivers.push_back(std::move(driver));
  return std::make_unique<StepperController>(std::move(cfgs),
                                             std::move(drivers));
}

// SPI_OK must describe the bus, not the motors hanging off it.
//
// This is the exact confusion that cost a bench session: motor0's module
// was strapped for STEP/DIR, so bring-up failed -- and because the SPI
// flag was wired to TMC bring-up, a perfectly working bus reported
// SPI_FAIL and sent debugging after the wiring. The motor fault has its
// own reporting (AllHealthy -> STEPPER_FAIL, motorN=FAILED); the bus flag
// must stay green while every datagram still gets through.
void TestUnusableMotorDoesNotReportBusFailure() {
  auto ctl = MakeBusTestController(
      std::make_unique<UnusableButReachableDriver>());

  assert(!ctl->AllHealthy());  // the motor is reported broken...
  assert(ctl->SpiBusOk());     // ...but the bus is not blamed for it
}

void TestDeadBusReportsBusFailure() {
  // The mirror case, and what makes the test above load-bearing: a genuine
  // transport failure must still turn SPI_FAIL red.
  auto ctl = MakeBusTestController(std::make_unique<DeadBusDriver>());

  assert(!ctl->AllHealthy());
  assert(!ctl->SpiBusOk());
}

// A refused enable must arrive at the operator with the driver's own
// diagnosis attached.
//
// The bench hit this the slow way: motor0's module is strapped for
// STEP/DIR, the driver identified that precisely and wrote it to the
// journal -- and the ground station still showed a bare "enable failed",
// so the cause was only discoverable by SSH-ing into the Pi.
void TestRefusedEnableCarriesDriverReason() {
  auto ctl = MakeBusTestController(
      std::make_unique<UnusableButReachableDriver>());
  std::string err;

  assert(!ctl->SetEnabled(0, true, &err));
  assert(err == UnusableButReachableDriver::kReason);
  assert(ctl->LastDriverError(0) == UnusableButReachableDriver::kReason);
}

// A backend with nothing to say must not invent a reason -- the caller's
// own fallback wording has to survive.
void TestRefusedEnableWithoutReasonLeavesErrorUntouched() {
  auto ctl = MakeBusTestController(std::make_unique<DeadBusDriver>());
  std::string err;

  assert(!ctl->SetEnabled(0, true, &err));
  assert(err.empty());
}

// A simulated backend has no bus of its own to break.
void TestSimulatedBackendReportsBusHealthy() {
  auto ctl = MakeBusTestController(
      std::make_unique<SimulatedStepperDriver>());

  assert(ctl->AllHealthy());
  assert(ctl->SpiBusOk());
}

// 2026-08-29 drive-settings surface: mm move commands, per-motor current,
// runtime accel.

// Parser: the mm and drive-settings commands take the same optional-id
// shape as their microstep siblings.
void TestParserMmAndDriveCommands() {
  CommandParser parser;

  auto r = parser.ParseLine("STEPPER_MOVE_MM 1 -2.5");
  assert(r.ok);
  assert(r.command.type == CommandType::kStepperMoveMm);
  assert(r.command.motor_id == 1);
  assert(r.command.args.size() == 1);
  assert(r.command.args[0] == "-2.5");

  // Legacy no-id form: a decimal payload is never a plausible motor id.
  auto r2 = parser.ParseLine("STEPPER_MOVE_MM 1.5");
  assert(r2.ok);
  assert(r2.command.motor_id == 0);
  assert(r2.command.args.size() == 1);
  assert(r2.command.args[0] == "1.5");

  auto r3 = parser.ParseLine("STEPPER_MOVETO_MM 0 4.0 5");
  assert(r3.ok);
  assert(r3.command.type == CommandType::kStepperMoveToMm);
  assert(r3.command.motor_id == 0);
  assert(r3.command.args.size() == 2);
  assert(r3.command.args[0] == "4.0");
  assert(r3.command.args[1] == "5");

  auto r4 = parser.ParseLine("STEPPER_SET_CURRENT 1 0.4");
  assert(r4.ok);
  assert(r4.command.type == CommandType::kStepperSetCurrent);
  assert(r4.command.motor_id == 1);
  assert(r4.command.args.size() == 1);
  assert(r4.command.args[0] == "0.4");

  auto r5 = parser.ParseLine("STEPPER_SET_ACCEL 0 400");
  assert(r5.ok);
  assert(r5.command.type == CommandType::kStepperSetAccel);
  assert(r5.command.motor_id == 0);
  assert(r5.command.args.size() == 1);
  assert(r5.command.args[0] == "400");
}

// mm -> microstep conversion goes through the ball-screw lead at the
// CURRENT divisor, and tracks a microstep change.
void TestMoveMillimetersConversion() {
  auto ch = MakeChannel(0);  // lead 2 mm/rev, 200 full-steps, u4
  std::string err;

  // 2 mm = 1 revolution = 200 x 4 = 800 microsteps.
  assert(ch->MoveMillimeters(2.0, &err));
  assert(ch->Snapshot().target_steps == 800);
  assert(ch->Snapshot().last_source == "cmd:MOVE_MM");

  ch->Stop();
  ch->SetPositionZero();

  // Absolute: -1 mm = -400 microsteps.
  assert(ch->MoveToMillimeters(-1.0, 0.0, &err));
  assert(ch->Snapshot().target_steps == -400);
  assert(ch->Snapshot().last_source == "cmd:BEND_MM");

  ch->Stop();
  ch->SetPositionZero();

  // After a microstep change the same distance lands on the same shaft
  // angle: 1 mm at u8 = 800 microsteps.
  assert(ch->SetMicrostep(8, &err));
  assert(ch->MoveToMillimeters(1.0, 0.0, &err));
  assert(ch->Snapshot().target_steps == 800);

  // Non-finite distance is refused.
  assert(!ch->MoveMillimeters(std::numeric_limits<double>::infinity(), &err));

  // Distance beyond max_position_steps is refused by the shared core.
  ch->Stop();
  assert(!ch->MoveToMillimeters(1e6, 0.0, &err));
}

// Snapshot reports the lead-derived linear position once a move completes.
void TestSnapshotReportsMillimeters() {
  auto ch = MakeChannel(0);
  std::string err;
  assert(ch->MoveMillimeters(2.0, &err));
  for (int i = 0; i < 20000; ++i) {
    ch->Tick(0.001);
    if (!ch->Snapshot().moving) break;
  }
  const StepperStatus s = ch->Snapshot();
  assert(s.position_steps == 800);
  assert(std::fabs(s.position_mm - 2.0) < 1e-9);
  assert(std::fabs(s.target_mm - 2.0) < 1e-9);
}

// SetAccel: applied within (0, max_accel_steps_per_s2], rejected outside,
// and visible in the snapshot (and thus telemetry).
void TestSetAccelBounds() {
  auto ch = MakeChannel(0);  // max_accel default 5000
  std::string err;

  assert(ch->Snapshot().accel_steps_per_s2 == 200.0);
  assert(ch->SetAccel(500.0, &err));
  assert(ch->Snapshot().accel_steps_per_s2 == 500.0);

  assert(!ch->SetAccel(0.0, &err));
  assert(!ch->SetAccel(-10.0, &err));
  assert(!ch->SetAccel(5001.0, &err));
  assert(ch->Snapshot().accel_steps_per_s2 == 500.0);  // unchanged by rejects
}

// Controller-level current dispatch: routed by id, stored by the backend,
// reported in the snapshot.
void TestControllerSetRunCurrent() {
  std::vector<StepperChannelConfig> cfgs;
  cfgs.push_back(MakeChannelCfg(0, {0, 1, 2, 3}));
  cfgs.push_back(MakeChannelCfg(1, {4, 5, 6, 7}));
  std::vector<std::unique_ptr<StepperDriver>> drvs;
  drvs.emplace_back(std::make_unique<SimulatedStepperDriver>());
  drvs.emplace_back(std::make_unique<SimulatedStepperDriver>());
  StepperController ctl(std::move(cfgs), std::move(drvs));

  std::string err;
  assert(ctl.SetRunCurrent(1, 0.4, &err));
  assert(std::fabs(ctl.Snapshot(1).run_current_a_rms - 0.4) < 1e-12);
  assert(ctl.Snapshot(0).run_current_a_rms == 0.0);  // untouched

  assert(!ctl.SetRunCurrent(0, -0.1, &err));
  assert(!ctl.SetRunCurrent(9, 0.4, &err));  // unknown id
}

}  // namespace

int main() {
  TestUnusableMotorDoesNotReportBusFailure();
  TestDeadBusReportsBusFailure();
  TestSimulatedBackendReportsBusHealthy();
  TestRefusedEnableCarriesDriverReason();
  TestRefusedEnableWithoutReasonLeavesErrorUntouched();
  TestTrapezoidalRamp();
  TestPulseThreadRampsInRealTime();
  TestParserIdArgument();
  TestParserLegacyDefault();
  TestBendArityDisambiguation();
  TestMaxStepHzCeiling();
  TestMotionLockExclusion();
  TestSetEnabledFalseReleasesLockEvenWhenDriverRefuses();
  TestSetEnabledTrueFailureLeavesChannelDisabled();
  TestPullCycleAcquiresLock();
  TestControllerMultiChannelDispatch();
  TestParserMmAndDriveCommands();
  TestMoveMillimetersConversion();
  TestSnapshotReportsMillimeters();
  TestSetAccelBounds();
  TestControllerSetRunCurrent();
  std::cout << "Rev C stepper tests passed" << std::endl;
  return 0;
}
