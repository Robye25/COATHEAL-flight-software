#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/motion_lock.hpp"
#include "coatheal/stepper_controller.hpp"  // for StepperStatus

namespace coatheal {

// Per-motor configuration. Everything a channel needs to own one motor
// end-to-end: driver handle, full-step kinematic limits, microstep divisor,
// accel/decel profile, and the sample indices it pulls.
struct StepperChannelConfig {
  int channel_id = 0;

  // Full-step motor nameplate. 200 for NEMA-17.
  int full_steps_per_rev = 200;

  // Pull-rate ceiling in full-steps per second. Rev C defaults to 100 Hz
  // (≈30 rpm) — ClampHz() enforces it against commanded speeds.
  double max_step_hz = 100.0;

  // Initial full-step rate used when no explicit SetSpeed has been called.
  // Must be <= max_step_hz. Clamped on construction.
  double default_step_hz = 100.0;

  // Trapezoidal acceleration in full-steps/s². Default 200 gives a 0.5 s
  // ramp from 0 to 100 Hz, matching the Rev C mechanical envelope.
  double accel_steps_per_s2 = 200.0;

  // Microstep divisor. 4 (Rev C default) or 5 are the only values accepted by
  // StepperChannel::SetMicrostep unless `allow_extended_microstep` is true
  // (which enables any divisor in [1, 32]).
  int microstep = 4;
  bool allow_extended_microstep = false;

  // Absolute travel limit in *microsteps* (=full_steps × microstep). Any
  // absolute target with |pos| > this is rejected.
  std::int64_t max_position_steps = 200000;

  // Sample indices this motor pulls. Motor 0 default 0..3, motor 1 default
  // 4..7. Exposed via samples() for Agent D's heater-scheduler interlock.
  std::vector<std::size_t> samples = {0, 1, 2, 3};

  // Pull cycle parameters. One pull = forward pull_travel_full_steps
  // (microstep-scaled), hold pull_hold_s, retract to 0.
  int pull_travel_full_steps = 200;  // 1 revolution ~= 1–2 mm downward
  double pull_hold_s = 5.0;

  bool enable_on_boot = false;

  // If true, StepperChannel launches a near-RT pulse-generation thread on
  // construction. Off for unit tests / bench runs (pulses still issue through
  // Tick() using the same trapezoidal math).
  bool use_pulse_thread = false;
};

// Owns one motor: driver, position, target, speed, microstep schedule. Pulse
// timing is shaped by a trapezoidal accel/decel curve with ceiling
// cfg.max_step_hz (full-step) and slope cfg.accel_steps_per_s2. The class
// supports two pulse-generation paths:
//   1. Tick()-driven: integrates the accel curve over dt_s, emits up to
//      current_rate × microstep × dt microsteps per call. Fully deterministic,
//      used by tests and by the existing system_controller tick loop.
//   2. RT-thread-driven: a dedicated thread sleeps (std::chrono::steady_clock)
//      between driver.Step() calls so microstep spacing is precise even when
//      the main tick loop runs at 1 Hz. Enabled by cfg.use_pulse_thread.
class StepperChannel {
 public:
  StepperChannel(StepperChannelConfig cfg,
                 std::unique_ptr<StepperDriver> driver,
                 MotionLock* lock = nullptr);
  ~StepperChannel();

  StepperChannel(const StepperChannel&) = delete;
  StepperChannel& operator=(const StepperChannel&) = delete;

  int channel_id() const { return cfg_.channel_id; }
  const std::vector<std::size_t>& samples() const { return cfg_.samples; }
  const StepperChannelConfig& config() const { return cfg_; }

  // Commanded motion — all operate in *microsteps* except SetSpeed which is
  // in *full-step Hz* (clamped to cfg.max_step_hz).
  bool MoveSteps(std::int64_t delta_usteps, std::string* error);
  bool MoveToSteps(std::int64_t absolute_usteps, double hold_s,
                   std::string* error);
  bool Rotate(double revolutions, std::string* error);
  bool Home(std::string* error);
  void Stop();
  bool SetSpeed(double full_step_hz, std::string* error);
  bool SetMicrostep(int divisor, std::string* error);
  bool SetEnabled(bool enable);

  // Pull cycle — acquires MotionLock, runs one full pull+hold+retract, then
  // releases the lock. ArmPullCycle just queues the motion (non-blocking);
  // ExecutePullCycle drives it to completion synchronously by pumping Tick()
  // until the retract finishes.
  bool ArmPullCycle(std::string* error);
  bool ExecutePullCycle(std::string* error);

  // Tick the trapezoidal ramp + pulse schedule by dt_s. Called every main
  // control-loop tick by system_controller.
  void Tick(double dt_s);

  StepperStatus Snapshot() const;

  // "[pull] cycle complete id=<id> samples=0,1,2,3"
  std::string FormatPullCompleteLog() const;

 private:
  enum class Mode {
    kIdle,
    kMoving,     // in-progress move toward target_
    kHolding,    // hold_remaining_s_ counting down
    kRetracting  // pull-cycle retract leg toward retract_target_
  };

  void PulseThreadBody();
  double ClampHz(double hz) const;
  std::int64_t FullStepsPerRev() const { return cfg_.full_steps_per_rev; }
  void ReleaseLockIfHeld();  // caller holds mu_
  // Core pulse-issuing worker used by both Tick() and the RT thread. Called
  // with mu_ held. Issues up to `allowed_usteps` microsteps toward target_
  // and updates position_/mode_. Returns # of microsteps issued.
  std::int64_t IssuePulses(std::int64_t allowed_usteps);
  // Update current_step_hz_ based on the trapezoidal profile given a
  // remaining-microsteps-to-target count and dt_s. Sets it to 0 when idle.
  void UpdateRampSpeed(double dt_s, std::int64_t remaining_usteps);

  StepperChannelConfig cfg_;
  std::unique_ptr<StepperDriver> driver_;
  MotionLock* lock_ = nullptr;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  // Guarded by mu_:
  std::int64_t position_ = 0;         // microsteps
  std::int64_t target_ = 0;           // microsteps (active leg)
  double step_hz_ = 0.0;              // commanded full-step Hz
  double current_step_hz_ = 0.0;      // actual during ramp (full-step Hz)
  int microstep_ = 1;
  bool enabled_ = false;
  bool moving_ = false;
  double hold_remaining_s_ = 0.0;
  std::string last_source_ = "init";
  Mode mode_ = Mode::kIdle;
  bool lock_held_ = false;
  std::int64_t retract_target_ = 0;
  double fractional_steps_ = 0.0;  // sub-integer accumulator

  std::thread pulse_thread_;
  std::atomic<bool> pulse_thread_run_{false};
};

}  // namespace coatheal
