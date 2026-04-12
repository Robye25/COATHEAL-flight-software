#include "coatheal/stepper_controller.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace coatheal {

StepperController::StepperController(const StepperConfig& cfg,
                                     const BendScheduleConfig& schedule,
                                     std::unique_ptr<StepperDriver> driver)
    : cfg_(cfg),
      schedule_(schedule),
      driver_(std::move(driver)),
      step_hz_(cfg.default_step_hz),
      microstep_(cfg.microstep) {
  if (driver_) {
    driver_->SetMicrostep(microstep_);
    driver_->Enable(cfg_.enable_on_boot);
    enabled_ = cfg_.enable_on_boot;
  }
}

bool StepperController::ResolvePhaseBend(MissionPhase phase,
                                         std::int64_t* steps,
                                         double* hold_s) const {
  switch (phase) {
    case MissionPhase::kAscentHold:
      *steps = schedule_.ascent_steps;
      *hold_s = schedule_.ascent_hold_s;
      return true;
    case MissionPhase::kActivationRamp:
      *steps = schedule_.activation_steps;
      *hold_s = schedule_.activation_hold_s;
      return true;
    case MissionPhase::kFloatHold:
      *steps = schedule_.float_steps;
      *hold_s = schedule_.float_hold_s;
      return true;
    case MissionPhase::kDescentFloor:
      *steps = schedule_.descent_steps;
      *hold_s = schedule_.descent_hold_s;
      return true;
    case MissionPhase::kStopped:
      return false;
  }
  return false;
}

void StepperController::ApplyPhaseSetpoint(MissionPhase phase) {
  std::int64_t steps = 0;
  double hold_s = 0.0;
  if (!ResolvePhaseBend(phase, &steps, &hold_s)) {
    return;
  }
  target_ = steps;
  hold_remaining_s_ = hold_s;
  moving_ = (position_ != target_);
  std::ostringstream tag;
  tag << "phase:" << ToString(phase);
  last_source_ = tag.str();
}

void StepperController::Tick(MissionPhase phase, double dt_s) {
  std::lock_guard<std::mutex> lock(mu_);

  if (!last_phase_valid_ || phase != last_phase_) {
    ApplyPhaseSetpoint(phase);
    last_phase_ = phase;
    last_phase_valid_ = true;
  }

  if (!driver_ || !enabled_ || dt_s <= 0.0) {
    moving_ = (position_ != target_);
    return;
  }

  // Issue up to step_hz_ * dt_s pulses this tick. Fractional remainder
  // carries over to the next tick so the rate is preserved even when tick_hz
  // is faster than step_hz.
  if (position_ != target_) {
    const bool forward = (target_ > position_);
    const std::int64_t remaining = std::abs(target_ - position_);
    fractional_steps_ += step_hz_ * dt_s;
    std::int64_t to_issue = static_cast<std::int64_t>(fractional_steps_);
    if (to_issue < 0) {
      to_issue = 0;
    }
    fractional_steps_ -= static_cast<double>(to_issue);
    if (to_issue > remaining) {
      to_issue = remaining;
      fractional_steps_ = 0.0;
    }
    for (std::int64_t i = 0; i < to_issue; ++i) {
      if (!driver_->Step(forward)) {
        break;
      }
      position_ += forward ? 1 : -1;
    }
    moving_ = (position_ != target_);
  } else {
    moving_ = false;
    fractional_steps_ = 0.0;
    if (hold_remaining_s_ > 0.0) {
      hold_remaining_s_ = std::max(0.0, hold_remaining_s_ - dt_s);
    }
  }
}

bool StepperController::MoveSteps(std::int64_t delta_steps, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::int64_t new_target = target_ + delta_steps;
  if (std::abs(new_target) > cfg_.max_position_steps) {
    if (error != nullptr) *error = "target exceeds stepper.max_position_steps";
    return false;
  }
  target_ = new_target;
  hold_remaining_s_ = 0.0;
  moving_ = (position_ != target_);
  last_source_ = "cmd:MOVE";
  return true;
}

bool StepperController::MoveToSteps(std::int64_t absolute_steps, double hold_s,
                                    std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (std::abs(absolute_steps) > cfg_.max_position_steps) {
    if (error != nullptr) *error = "target exceeds stepper.max_position_steps";
    return false;
  }
  if (hold_s < 0.0) {
    if (error != nullptr) *error = "hold must be >= 0";
    return false;
  }
  target_ = absolute_steps;
  hold_remaining_s_ = hold_s;
  moving_ = (position_ != target_);
  last_source_ = "cmd:BEND";
  return true;
}

bool StepperController::Rotate(double revolutions, std::string* error) {
  if (cfg_.steps_per_rev <= 0) {
    if (error != nullptr) *error = "steps_per_rev invalid";
    return false;
  }
  const double total = revolutions * static_cast<double>(cfg_.steps_per_rev) *
                       static_cast<double>(microstep_);
  return MoveSteps(static_cast<std::int64_t>(std::llround(total)), error);
}

bool StepperController::Home(std::string* /*error*/) {
  std::lock_guard<std::mutex> lock(mu_);
  target_ = 0;
  hold_remaining_s_ = 0.0;
  moving_ = (position_ != 0);
  last_source_ = "cmd:HOME";
  return true;
}

void StepperController::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  target_ = position_;
  hold_remaining_s_ = 0.0;
  moving_ = false;
  fractional_steps_ = 0.0;
  last_source_ = "cmd:STOP";
}

bool StepperController::SetSpeed(double step_hz, std::string* error) {
  if (step_hz <= 0.0 || step_hz > cfg_.max_step_hz) {
    if (error != nullptr) *error = "step_hz out of range (0, max_step_hz]";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  step_hz_ = step_hz;
  return true;
}

bool StepperController::SetMicrostep(int divisor, std::string* error) {
  if (divisor <= 0 || divisor > 32) {
    if (error != nullptr) *error = "microstep must be in [1, 32]";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  microstep_ = divisor;
  if (driver_) {
    driver_->SetMicrostep(divisor);
  }
  return true;
}

bool StepperController::SetEnabled(bool enable) {
  std::lock_guard<std::mutex> lock(mu_);
  enabled_ = enable;
  if (driver_) {
    driver_->Enable(enable);
  }
  if (!enable) {
    moving_ = false;
  }
  return true;
}

StepperStatus StepperController::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  StepperStatus s;
  s.position_steps = position_;
  s.target_steps = target_;
  s.step_hz = step_hz_;
  s.microstep = microstep_;
  s.enabled = enabled_;
  s.moving = moving_;
  s.holding = (!moving_ && hold_remaining_s_ > 0.0);
  s.hold_remaining_s = hold_remaining_s_;
  s.pulses_total = driver_ ? driver_->pulses_issued() : 0;
  s.last_source = last_source_;
  return s;
}

}  // namespace coatheal
