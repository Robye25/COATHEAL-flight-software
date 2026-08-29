#include "coatheal/stepper_channel.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <utility>

namespace coatheal {

namespace {

bool IsSupportedMicrostep(int divisor) {
  return divisor == 1 || divisor == 2 || divisor == 4 || divisor == 8 ||
         divisor == 16 || divisor == 32 || divisor == 64 ||
         divisor == 128 || divisor == 256;
}

}  // namespace

// Kinematic brake distance: full-step distance to bring current_hz → 0 at
// accel (full-step/s²) is v² / (2a). Used implicitly in UpdateRampSpeed() via
// the inverse formula (max safe current_hz for a given remaining distance is
// sqrt(2 · a · d)).

StepperChannel::StepperChannel(StepperChannelConfig cfg,
                               std::unique_ptr<StepperDriver> driver,
                               MotionLock* lock)
    : cfg_(std::move(cfg)), driver_(std::move(driver)), lock_(lock) {
  if (cfg_.default_step_hz > cfg_.max_step_hz) {
    cfg_.default_step_hz = cfg_.max_step_hz;
  }
  if (cfg_.default_step_hz <= 0.0) {
    cfg_.default_step_hz = cfg_.max_step_hz;
  }
  microstep_ = IsSupportedMicrostep(cfg_.microstep) ? cfg_.microstep : 4;
  step_hz_ = cfg_.default_step_hz;

  if (driver_) {
    driver_->SetMicrostep(microstep_);
    enabled_ = cfg_.enable_on_boot && driver_->Enable(true);
  }

  if (cfg_.use_pulse_thread) {
    pulse_thread_run_.store(true);
    pulse_thread_ = std::thread(&StepperChannel::PulseThreadBody, this);
  }
}

StepperChannel::~StepperChannel() {
  pulse_thread_run_.store(false);
  cv_.notify_all();
  if (pulse_thread_.joinable()) {
    pulse_thread_.join();
  }
  std::lock_guard<std::mutex> lock(mu_);
  ReleaseLockIfHeld();
}

double StepperChannel::ClampHz(double hz) const {
  if (hz < 0.0) hz = 0.0;
  if (hz > cfg_.max_step_hz) hz = cfg_.max_step_hz;
  return hz;
}

void StepperChannel::ReleaseLockIfHeld() {
  if (lock_held_ && lock_ != nullptr) {
    lock_->Release(cfg_.channel_id);
    lock_held_ = false;
  }
}

bool StepperChannel::AcquireLockForMotion(std::string* error) {
  if (lock_held_ || lock_ == nullptr) {
    return true;
  }
  if (!lock_->TryAcquire(cfg_.channel_id)) {
    if (error) *error = "motion lock held by another motor";
    return false;
  }
  lock_held_ = true;
  return true;
}

void StepperChannel::UpdateRampSpeed(double dt_s,
                                     std::int64_t remaining_usteps) {
  if (dt_s <= 0.0) return;

  if (mode_ == Mode::kIdle || mode_ == Mode::kHolding) {
    current_step_hz_ = 0.0;
    return;
  }

  // Trapezoidal profile: accelerate toward step_hz_, but decelerate early if
  // we're closer than the brake distance would need.
  const double target_hz = step_hz_;
  const double brake_full_hz = std::sqrt(
      std::max(0.0, 2.0 * cfg_.accel_steps_per_s2 *
                        (static_cast<double>(remaining_usteps) /
                         static_cast<double>(std::max(1, microstep_)))));
  const double cruise_or_brake = std::min(target_hz, brake_full_hz);

  if (current_step_hz_ < cruise_or_brake) {
    current_step_hz_ += cfg_.accel_steps_per_s2 * dt_s;
    if (current_step_hz_ > cruise_or_brake) {
      current_step_hz_ = cruise_or_brake;
    }
  } else if (current_step_hz_ > cruise_or_brake) {
    current_step_hz_ -= cfg_.accel_steps_per_s2 * dt_s;
    if (current_step_hz_ < cruise_or_brake) {
      current_step_hz_ = cruise_or_brake;
    }
  }
  if (current_step_hz_ < 0.0) current_step_hz_ = 0.0;
  if (current_step_hz_ > cfg_.max_step_hz) current_step_hz_ = cfg_.max_step_hz;
}

std::int64_t StepperChannel::IssuePulses(std::int64_t allowed_usteps) {
  if (!driver_ || !enabled_ || allowed_usteps <= 0) return 0;
  if (position_ == target_) return 0;

  const bool forward = (target_ > position_);
  const std::int64_t remaining = std::abs(target_ - position_);
  if (allowed_usteps > remaining) allowed_usteps = remaining;

  std::int64_t issued = 0;
  for (std::int64_t i = 0; i < allowed_usteps; ++i) {
    if (!driver_->Step(forward)) {
      driver_->Enable(false);
      enabled_ = false;
      target_ = position_;
      moving_ = false;
      mode_ = Mode::kIdle;
      hold_remaining_s_ = 0.0;
      retract_after_hold_ = false;
      ReleaseLockIfHeld();
      break;
    }
    position_ += forward ? 1 : -1;
    ++issued;
  }
  return issued;
}

void StepperChannel::Tick(double dt_s) {
  std::lock_guard<std::mutex> lock(mu_);
  if (dt_s <= 0.0) return;
  if (driver_ != nullptr && !driver_->healthy() &&
      mode_ == Mode::kIdle) {
    const auto now = std::chrono::steady_clock::now();
    if (last_driver_retry_.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_driver_retry_).count() >= cfg_.driver_retry_ms) {
      last_driver_retry_ = now;
      if (driver_->ActiveCheck()) {
        driver_->SetMicrostep(microstep_);
        if (enabled_) {
          enabled_ = driver_->Enable(true);
        }
      }
    }
  } else if (driver_ != nullptr && enabled_ &&
             (mode_ == Mode::kIdle || mode_ == Mode::kHolding)) {
    // Once per tick while energised but not stepping: lets the driver
    // notice a chip that lost its configuration (supply dip) and restore
    // it, so an "enabled" motor really holds and the next move works.
    driver_->Poll();
  }

  // Thermal safety: a driver that latched over-temperature shutdown
  // (>= ~150 °C die — the chip has already cut its outputs) is stopped and
  // de-energised here rather than left "enabled" against a dead power
  // stage. Mirrors the IssuePulses failure teardown (SetEnabled would
  // re-take mu_). Self-limiting: once enabled_ drops this cannot repeat,
  // and the operator re-arms with STEPPER_ENABLE after cool-down.
  if (driver_ != nullptr && enabled_ && driver_->thermal_state() >= 2) {
    std::cerr << "[stepper] motor " << cfg_.channel_id
              << ": driver over-temperature shutdown -- stopping motion and"
              << " disabling the channel; STEPPER_ENABLE re-arms after"
              << " cool-down\n";
    driver_->Enable(false);
    enabled_ = false;
    target_ = position_;
    retract_target_ = position_;
    moving_ = false;
    mode_ = Mode::kIdle;
    hold_remaining_s_ = 0.0;
    retract_after_hold_ = false;
    current_step_hz_ = 0.0;
    fractional_steps_ = 0.0;
    last_source_ = "safety:OVERTEMP";
    ReleaseLockIfHeld();
    return;
  }

  if (mode_ == Mode::kHolding) {
    hold_remaining_s_ = std::max(0.0, hold_remaining_s_ - dt_s);
    if (hold_remaining_s_ <= 0.0) {
      if (retract_after_hold_) {
        target_ = retract_target_;
        mode_ = (position_ != target_) ? Mode::kRetracting : Mode::kIdle;
        moving_ = (mode_ != Mode::kIdle);
        fractional_steps_ = 0.0;
        current_step_hz_ = 0.0;
        if (mode_ == Mode::kIdle) {
          retract_after_hold_ = false;
          ReleaseLockIfHeld();
        }
      } else {
        mode_ = Mode::kIdle;
        moving_ = false;
        fractional_steps_ = 0.0;
        current_step_hz_ = 0.0;
        ReleaseLockIfHeld();
      }
    }
    return;
  }

  if (mode_ != Mode::kMoving && mode_ != Mode::kRetracting) {
    moving_ = false;
    current_step_hz_ = 0.0;
    return;
  }

  if (position_ == target_) {
    // Reached a leg terminus. For a pull-cycle move leg, transition to hold.
    if (mode_ == Mode::kMoving && hold_remaining_s_ > 0.0) {
      mode_ = Mode::kHolding;
      moving_ = false;
      current_step_hz_ = 0.0;
      return;
    }
    if (mode_ == Mode::kRetracting) {
      mode_ = Mode::kIdle;
      moving_ = false;
      current_step_hz_ = 0.0;
      retract_after_hold_ = false;
      ReleaseLockIfHeld();
      return;
    }
    mode_ = Mode::kIdle;
    moving_ = false;
    current_step_hz_ = 0.0;
    ReleaseLockIfHeld();
    return;
  }

  if (cfg_.use_pulse_thread) {
    moving_ = true;
    cv_.notify_all();
    return;
  }

  const std::int64_t remaining_usteps = std::abs(target_ - position_);
  UpdateRampSpeed(dt_s, remaining_usteps);

  const double ustep_rate = current_step_hz_ * static_cast<double>(microstep_);
  fractional_steps_ += ustep_rate * dt_s;
  std::int64_t to_issue = static_cast<std::int64_t>(fractional_steps_);
  if (to_issue < 0) to_issue = 0;
  fractional_steps_ -= static_cast<double>(to_issue);

  const std::int64_t issued = IssuePulses(to_issue);
  (void)issued;

  moving_ = (position_ != target_);
  if (!moving_) {
    fractional_steps_ = 0.0;
    current_step_hz_ = 0.0;
    if (mode_ == Mode::kMoving && hold_remaining_s_ > 0.0) {
      mode_ = Mode::kHolding;
    } else if (mode_ == Mode::kRetracting) {
      mode_ = Mode::kIdle;
      retract_after_hold_ = false;
      ReleaseLockIfHeld();
    } else {
      mode_ = Mode::kIdle;
      ReleaseLockIfHeld();
    }
  }
}

void StepperChannel::PulseThreadBody() {
  // Near-RT pulse scheduler. Runs a fine-grained loop; sleeps between pulses
  // by (1 / (current_step_hz × microstep)) seconds. We use steady_clock so
  // wall-clock jumps don't disturb spacing.
  using clock = std::chrono::steady_clock;
  auto next_pulse = clock::now();
  auto last_ramp_update = clock::now();

  while (pulse_thread_run_.load()) {
    std::unique_lock<std::mutex> lock(mu_);
    if (mode_ != Mode::kMoving && mode_ != Mode::kRetracting) {
      // Nothing to pulse — sleep briefly and re-check.
      cv_.wait_for(lock, std::chrono::milliseconds(5));
      next_pulse = clock::now();
      last_ramp_update = next_pulse;
      continue;
    }
    const std::int64_t remaining_usteps = std::abs(target_ - position_);
    if (remaining_usteps == 0) {
      cv_.wait_for(lock, std::chrono::milliseconds(1));
      continue;
    }
    // Time-slice: update ramp at 1 kHz, so advance ~1 ms at a time.
    // The ramp integrates real elapsed time. This used to pass a fixed
    // 1 ms per iteration, but an iteration is one pulse followed by a sleep
    // of one pulse period, so the acceleration was applied per STEP, not
    // per second: from standstill the motor crawled at a few microsteps
    // per second and needed ~500 pulses (5-7 s) to reach 100 Hz -- the
    // "motors never move" seen on the bench, 2026-08-29. Capped so a
    // scheduling hiccup cannot jump the speed.
    const auto now_ramp = clock::now();
    double dt_s = std::chrono::duration<double>(now_ramp - last_ramp_update).count();
    last_ramp_update = now_ramp;
    if (dt_s < 0.0) dt_s = 0.0;
    if (dt_s > 0.05) dt_s = 0.05;
    UpdateRampSpeed(dt_s, remaining_usteps);
    const double ustep_rate =
        current_step_hz_ * static_cast<double>(microstep_);
    if (ustep_rate < 1.0) {
      cv_.wait_for(lock, std::chrono::milliseconds(2));
      continue;
    }
    IssuePulses(1);
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / ustep_rate));
    next_pulse += period;
    lock.unlock();
    const auto now = clock::now();
    if (now > next_pulse) {
      std::lock_guard<std::mutex> count_lock(mu_);
      ++missed_deadlines_;
      next_pulse = now + period;
    }
    std::this_thread::sleep_until(next_pulse);
  }
}

bool StepperChannel::MoveSteps(std::int64_t delta_usteps, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  return MoveStepsUnlocked(delta_usteps, error);
}

bool StepperChannel::MoveStepsUnlocked(std::int64_t delta_usteps,
                                       std::string* error) {
  if (!enabled_) {
    if (error) *error = "channel disabled";
    return false;
  }
  const std::int64_t new_target = target_ + delta_usteps;
  if (std::abs(new_target) > cfg_.max_position_steps) {
    if (error) *error = "target exceeds max_position_steps";
    return false;
  }
  if (new_target != position_ && !AcquireLockForMotion(error)) {
    return false;
  }
  target_ = new_target;
  hold_remaining_s_ = 0.0;
  retract_target_ = position_;  // fallback retract = current position
  retract_after_hold_ = false;
  mode_ = (position_ != target_) ? Mode::kMoving : Mode::kIdle;
  moving_ = (mode_ == Mode::kMoving);
  last_source_ = "cmd:MOVE";
  return true;
}

bool StepperChannel::MoveToSteps(std::int64_t absolute_usteps, double hold_s,
                                 std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  return MoveToStepsUnlocked(absolute_usteps, hold_s, error);
}

bool StepperChannel::MoveToStepsUnlocked(std::int64_t absolute_usteps,
                                         double hold_s, std::string* error) {
  if (!enabled_) {
    if (error) *error = "channel disabled";
    return false;
  }
  if (std::abs(absolute_usteps) > cfg_.max_position_steps) {
    if (error) *error = "target exceeds max_position_steps";
    return false;
  }
  if (hold_s < 0.0) {
    if (error) *error = "hold must be >= 0";
    return false;
  }
  if ((position_ != absolute_usteps || hold_s > 0.0) && !AcquireLockForMotion(error)) {
    return false;
  }
  target_ = absolute_usteps;
  hold_remaining_s_ = hold_s;
  retract_target_ = position_;
  retract_after_hold_ = false;
  if (position_ != target_) {
    mode_ = Mode::kMoving;
  } else if (hold_s > 0.0) {
    mode_ = Mode::kHolding;
  } else {
    mode_ = Mode::kIdle;
  }
  moving_ = (mode_ == Mode::kMoving);
  last_source_ = "cmd:BEND";
  return true;
}

bool StepperChannel::MoveMillimeters(double delta_mm, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!std::isfinite(delta_mm)) {
    if (error) *error = "distance must be finite";
    return false;
  }
  const double usteps = delta_mm * UstepsPerMm();
  if (std::fabs(usteps) > 9.0e15) {  // llround overflow guard
    if (error) *error = "distance too large";
    return false;
  }
  if (!MoveStepsUnlocked(std::llround(usteps), error)) return false;
  last_source_ = "cmd:MOVE_MM";
  return true;
}

bool StepperChannel::MoveToMillimeters(double absolute_mm, double hold_s,
                                       std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!std::isfinite(absolute_mm)) {
    if (error) *error = "distance must be finite";
    return false;
  }
  const double usteps = absolute_mm * UstepsPerMm();
  if (std::fabs(usteps) > 9.0e15) {
    if (error) *error = "distance too large";
    return false;
  }
  if (!MoveToStepsUnlocked(std::llround(usteps), hold_s, error)) return false;
  last_source_ = "cmd:BEND_MM";
  return true;
}

bool StepperChannel::Rotate(double revolutions, std::string* error) {
  if (cfg_.full_steps_per_rev <= 0) {
    if (error) *error = "full_steps_per_rev invalid";
    return false;
  }
  const double total = revolutions *
                       static_cast<double>(cfg_.full_steps_per_rev) *
                       static_cast<double>(microstep_);
  return MoveSteps(static_cast<std::int64_t>(std::llround(total)), error);
}

bool StepperChannel::Home(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!enabled_) {
    if (error) *error = "channel disabled";
    return false;
  }
  if (position_ != 0 && !AcquireLockForMotion(error)) {
    return false;
  }
  target_ = 0;
  hold_remaining_s_ = 0.0;
  retract_target_ = 0;
  retract_after_hold_ = false;
  mode_ = (position_ != 0) ? Mode::kMoving : Mode::kIdle;
  moving_ = (mode_ == Mode::kMoving);
  last_source_ = "cmd:HOME";
  return true;
}

void StepperChannel::SetPositionZero() {
  std::lock_guard<std::mutex> lock(mu_);
  position_ = 0;
  target_ = 0;
  retract_target_ = 0;
  hold_remaining_s_ = 0.0;
  fractional_steps_ = 0.0;
  current_step_hz_ = 0.0;
  moving_ = false;
  mode_ = Mode::kIdle;
  retract_after_hold_ = false;
  last_source_ = "cmd:ZERO";
  ReleaseLockIfHeld();
}

void StepperChannel::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  target_ = position_;
  retract_target_ = position_;
  hold_remaining_s_ = 0.0;
  moving_ = false;
  fractional_steps_ = 0.0;
  current_step_hz_ = 0.0;
  mode_ = Mode::kIdle;
  retract_after_hold_ = false;
  last_source_ = "cmd:STOP";
  ReleaseLockIfHeld();
}

bool StepperChannel::SetSpeed(double full_step_hz, std::string* error) {
  if (full_step_hz <= 0.0) {
    if (error) *error = "step_hz must be > 0";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  step_hz_ = ClampHz(full_step_hz);
  return true;
}

bool StepperChannel::SetAccel(double accel_steps_per_s2, std::string* error) {
  if (!std::isfinite(accel_steps_per_s2) || accel_steps_per_s2 <= 0.0) {
    if (error) *error = "accel must be > 0 full-steps/s^2";
    return false;
  }
  if (accel_steps_per_s2 > cfg_.max_accel_steps_per_s2) {
    if (error) {
      std::ostringstream msg;
      msg << "accel exceeds stepper.max_accel_steps_per_s2 ("
          << cfg_.max_accel_steps_per_s2 << ")";
      *error = msg.str();
    }
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  cfg_.accel_steps_per_s2 = accel_steps_per_s2;
  return true;
}

bool StepperChannel::SetRunCurrent(double a_rms, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (driver_ == nullptr) {
    if (error) *error = "no driver";
    return false;
  }
  return driver_->SetRunCurrent(a_rms, error);
}

bool StepperChannel::SetMicrostep(int divisor, std::string* error) {
  if (!IsSupportedMicrostep(divisor)) {
    if (error) {
      *error = "microstep must be 1, 2, 4, 8, 16, 32, 64, 128, or 256";
    }
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (driver_) {
    driver_->SetMicrostep(divisor);
    if (!driver_->healthy()) {
      if (error) *error = "driver microstep configuration failed";
      return false;
    }
  }
  // Re-scale position_ / target_ proportionally so absolute travel in
  // real-world distance is preserved across a microstep change.
  if (microstep_ != divisor && microstep_ > 0) {
    const double scale = static_cast<double>(divisor) /
                         static_cast<double>(microstep_);
    position_ = static_cast<std::int64_t>(std::llround(position_ * scale));
    target_ = static_cast<std::int64_t>(std::llround(target_ * scale));
    retract_target_ = static_cast<std::int64_t>(std::llround(retract_target_ * scale));
  }
  microstep_ = divisor;
  return true;
}

bool StepperChannel::SetEnabled(bool enable) {
  std::lock_guard<std::mutex> lock(mu_);
  if (driver_ == nullptr) return false;
  const bool driver_ok = driver_->Enable(enable);

  if (!enable) {
    // Tear the channel down even when the driver reports failure.
    //
    // A driver that could not disable itself is unhealthy either way, and
    // that is not the failure worth optimising for. The one that is: the
    // heater interlock and the MotionLock are both released from HERE.
    // HeaterScheduler clamps every duty to 0 whenever MotionLock::is_active()
    // (heater_scheduler.cpp), and the lock is only ever released by a motion
    // path finishing. Returning early on an Enable(false) failure left
    // enabled_ true and the lock latched with no motion left to release it:
    // all six heaters forced off and the other motor locked out,
    // indefinitely, because a driver we already know is broken said no.
    //
    // So the local state is cleared on both paths and only the return value
    // carries the driver's verdict -- the caller still learns it failed.
    enabled_ = false;
    moving_ = false;
    mode_ = Mode::kIdle;
    current_step_hz_ = 0.0;
    ReleaseLockIfHeld();
    return driver_ok;
  }

  // Enable(true) keeps the early return: a driver that could not energise
  // must never be recorded as enabled -- that would let motion be commanded
  // into a driver that is not driving.
  if (!driver_ok) return false;
  enabled_ = true;
  return true;
}

bool StepperChannel::ArmPullCycle(std::string* error) {
  // Read `enabled_` under mu_ — it is not atomic and another thread may be
  // flipping it via SetEnabled(). Grab the motion lock AFTER confirming we are
  // enabled so we don't briefly hold the lock only to release it.
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_) {
      if (error) *error = "channel disabled";
      return false;
    }
  }
  if (lock_ != nullptr) {
    if (!lock_->TryAcquire(cfg_.channel_id)) {
      if (error) *error = "motion lock held by another motor";
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  lock_held_ = (lock_ != nullptr);
  retract_target_ = 0;  // pull cycle always retracts to home
  retract_after_hold_ = true;
  const std::int64_t pull_usteps =
      static_cast<std::int64_t>(cfg_.pull_travel_full_steps) * microstep_;
  if (std::abs(pull_usteps) > cfg_.max_position_steps) {
    if (error) *error = "pull target exceeds max_position_steps";
    ReleaseLockIfHeld();
    return false;
  }
  target_ = pull_usteps;
  hold_remaining_s_ = cfg_.pull_hold_s;
  mode_ = (position_ != target_) ? Mode::kMoving : Mode::kHolding;
  moving_ = (mode_ == Mode::kMoving);
  step_hz_ = cfg_.max_step_hz;  // pull at ceiling with accel/decel ramp
  last_source_ = "cmd:PULL";
  return true;
}

bool StepperChannel::ExecutePullCycle(std::string* error) {
  if (!ArmPullCycle(error)) return false;
  // Pump Tick() synchronously at ~1 kHz until the retract completes. Used by
  // the PULL_EXECUTE command when the caller wants blocking semantics. The
  // real flight loop calls Tick() itself and doesn't need this helper, but
  // bench/unit flows do.
  constexpr double dt = 0.001;
  constexpr int kMaxIters = 60'000;  // 60 s ceiling
  for (int i = 0; i < kMaxIters; ++i) {
    Tick(dt);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (mode_ == Mode::kIdle) break;
    }
  }
  return true;
}

StepperStatus StepperChannel::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  StepperStatus s;
  s.position_steps = position_;
  s.target_steps = target_;
  s.step_hz = step_hz_;
  s.microstep = microstep_;
  s.accel_steps_per_s2 = cfg_.accel_steps_per_s2;
  s.run_current_a_rms = driver_ ? driver_->run_current_a_rms() : 0.0;
  s.thermal_state = driver_ ? driver_->thermal_state() : 0;
  const double usteps_per_mm = UstepsPerMm();
  if (usteps_per_mm > 0.0) {
    s.position_mm = static_cast<double>(position_) / usteps_per_mm;
    s.target_mm = static_cast<double>(target_) / usteps_per_mm;
  }
  s.enabled = enabled_;
  s.healthy = driver_ != nullptr && driver_->healthy();
  s.moving = moving_;
  s.holding = (mode_ == Mode::kHolding);
  s.hold_remaining_s = hold_remaining_s_;
  s.pulses_total = driver_ ? driver_->pulses_issued() : 0;
  s.missed_deadlines = missed_deadlines_;
  s.last_source = last_source_;
  return s;
}

std::string StepperChannel::FormatPullCompleteLog() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream oss;
  oss << "[pull] cycle complete id=" << cfg_.channel_id << " samples=";
  for (std::size_t i = 0; i < cfg_.samples.size(); ++i) {
    if (i) oss << ',';
    oss << cfg_.samples[i];
  }
  return oss.str();
}

}  // namespace coatheal
