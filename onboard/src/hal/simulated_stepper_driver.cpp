#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

SimulatedStepperDriver::SimulatedStepperDriver() = default;

bool SimulatedStepperDriver::Enable(bool enable) {
  enabled_ = enable;
  return true;
}

bool SimulatedStepperDriver::Step(bool direction_forward) {
  if (!enabled_) {
    return false;
  }
  last_dir_ = direction_forward;
  ++pulses_;
  return true;
}

void SimulatedStepperDriver::SetMicrostep(int divisor) {
  if (divisor > 0) {
    microstep_ = divisor;
  }
}

bool SimulatedStepperDriver::SetRunCurrent(double a_rms, std::string* error) {
  if (a_rms <= 0.0) {
    if (error) *error = "run current must be > 0";
    return false;
  }
  run_current_a_rms_ = a_rms;
  return true;
}

}  // namespace coatheal
