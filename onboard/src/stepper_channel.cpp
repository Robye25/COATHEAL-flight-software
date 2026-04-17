#include "coatheal/stepper_channel.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace coatheal {

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
  microstep_ = std::max(1, cfg_.microstep);
  step_hz_ = cfg_.default_step_hz;

  if (driver_) {
    driver_->SetMicrostep(microstep_);
    driver_->Enable(cfg_.enable_on_boot);
    enabled_ = cfg_.enable_on_boot;
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
    if (!driver_->Step(forward)) break;
    position_ += forward ? 1 : -1;
    ++issued;
  }
  return issued;
}

void StepperChannel::Tick(double dt_s) {
  std::lock_guard<std::mutex> lock(mu_);
  if (dt_s <= 0.0) return;

  if (mode_ == Mode::kHolding) {
    hold_remaining_s_ = std::max(0.0, hold_remaining_s_ - dt_s);
    if (hold_remaining_s_ <= 0.0) {
      // Pull-cycle retract leg: aim back at retract_target_.
      target_ = retract_target_;
      mode_ = (position_ != target_) ? Mode::kRetracting : Mode::kIdle;
      moving_ = (mode_ != Mode::kIdle);
      fractional_steps_ = 0.0;
      current_step_hz_ = 0.0;
      if (mode_ == Mode::kIdle) {
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
      ReleaseLockIfHeld();
      return;
    }
    mode_ = Mode::kIdle;
    moving_ = false;
    current_step_hz_ = 0.0;
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
      ReleaseLockIfHeld();
    } else {
      mode_ = Mode::kIdle;
    }
  }
}

void StepperChannel::PulseThreadBody() {
  // Near-RT pulse scheduler. Runs a fine-grained loop; sleeps between pulses
  // by (1 / (current_step_hz × microstep)) seconds. We use steady_clock so
  // wall-clock jumps don't disturb spacing.
  using clock = std::chrono::steady_clock;
  auto next_pulse = clock::now();

  while (pulse_thread_run_.load()) {
    std::unique_lock<std::mutex> lock(mu_);
    if (mode_ != Mode::kMoving && mode_ != Mode::kRetracting) {
      // Nothing to pulse — sleep briefly and re-check.
      cv_.wait_for(lock, std::chrono::milliseconds(5));
      next_pulse = clock::now();
      continue;
    }
    const std::int64_t remaining_usteps = std::abs(target_ - position_);
    if (remaining_usteps == 0) {
      cv_.wait_for(lock, std::chrono::milliseconds(1));
      continue;
    }
    // Time-slice: update ramp at 1 kHz, so advance ~1 ms at a time.
    constexpr double dt_tick = 0.001;
    UpdateRampSpeed(dt_tick, remaining_usteps);
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
    std::this_thread::sleep_until(next_pulse);
  }
}

bool StepperChannel::MoveSteps(std::int64_t delta_usteps, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::int64_t new_target = target_ + delta_usteps;
  if (std::abs(new_target) > cfg_.max_position_steps) {
    if (error) *error = "target exceeds max_position_steps";
    return false;
  }
  target_ = new_target;
  hold_remaining_s_ = 0.0;
  retract_target_ = position_;  // fallback retract = current position
  mode_ = (position_ != target_) ? Mode::kMoving : Mode::kIdle;
  moving_ = (mode_ == Mode::kMoving);
  last_source_ = "cmd:MOVE";
  return true;
}

bool StepperChannel::MoveToSteps(std::int64_t absolute_usteps, double hold_s,
                                 std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (std::abs(absolute_usteps) > cfg_.max_position_steps) {
    if (error) *error = "target exceeds max_position_steps";
    return false;
  }
  if (hold_s < 0.0) {
    if (error) *error = "hold must be >= 0";
    return false;
  }
  target_ = absolute_usteps;
  hold_remaining_s_ = hold_s;
  retract_target_ = position_;
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

bool StepperChannel::Home(std::string* /*error*/) {
  std::lock_guard<std::mutex> lock(mu_);
  target_ = 0;
  hold_remaining_s_ = 0.0;
  retract_target_ = 0;
  mode_ = (position_ != 0) ? Mode::kMoving : Mode::kIdle;
  moving_ = (mode_ == Mode::kMoving);
  last_source_ = "cmd:HOME";
  return true;
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

bool StepperChannel::SetMicrostep(int divisor, std::string* error) {
  if (divisor <= 0) {
    if (error) *error = "microstep must be > 0";
    return false;
  }
  if (!cfg_.allow_extended_microstep) {
    if (divisor != 4 && divisor != 5) {
      if (error) *error = "microstep must be 4 or 5";
      return false;
    }
  } else if (divisor > 32) {
    if (error) *error = "microstep must be <= 32";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
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
  if (driver_) driver_->SetMicrostep(divisor);
  return true;
}

bool StepperChannel::SetEnabled(bool enable) {
  std::lock_guard<std::mutex> lock(mu_);
  enabled_ = enable;
  if (driver_) driver_->Enable(enable);
  if (!enable) {
    moving_ = false;
    mode_ = Mode::kIdle;
    current_step_hz_ = 0.0;
    ReleaseLockIfHeld();
  }
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
  s.enabled = enabled_;
  s.moving = moving_;
  s.holding = (mode_ == Mode::kHolding);
  s.hold_remaining_s = hold_remaining_s_;
  s.pulses_total = driver_ ? driver_->pulses_issued() : 0;
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
