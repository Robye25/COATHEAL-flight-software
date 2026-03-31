#include "coatheal/pid_controller.hpp"

#include <algorithm>

namespace coatheal {

PidController::PidController(PidGains gains, double output_min, double output_max,
                             double integral_min, double integral_max)
    : gains_(gains),
      output_min_(output_min),
      output_max_(output_max),
      integral_min_(integral_min),
      integral_max_(integral_max) {}

void PidController::SetGains(PidGains gains) {
  gains_ = gains;
}

void PidController::Reset() {
  integral_ = 0.0;
  prev_error_ = 0.0;
  has_prev_ = false;
}

double PidController::Update(double setpoint, double measured, double dt_seconds) {
  const double safe_dt = dt_seconds <= 1e-6 ? 1e-3 : dt_seconds;
  const double error = setpoint - measured;

  integral_ += error * safe_dt;
  integral_ = std::clamp(integral_, integral_min_, integral_max_);

  double derivative = 0.0;
  if (has_prev_) {
    derivative = (error - prev_error_) / safe_dt;
  }

  prev_error_ = error;
  has_prev_ = true;

  double out = (gains_.kp * error) + (gains_.ki * integral_) + (gains_.kd * derivative);
  out = std::clamp(out, output_min_, output_max_);
  return out;
}

}  // namespace coatheal