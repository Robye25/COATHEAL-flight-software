#include "coatheal/hal/pwm_controller.hpp"

#include <algorithm>

namespace coatheal {

SimulatedPwmController::SimulatedPwmController(std::size_t channels) : duty_(channels, 0.0) {}

bool SimulatedPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size()) {
    return false;
  }
  duty_[channel] = std::clamp(duty, 0.0, 1.0);
  return true;
}

LibgpiodPwmController::LibgpiodPwmController(std::string chip, std::size_t channels)
    : chip_(std::move(chip)), duty_(channels, 0.0) {
#ifdef COATHEAL_HAS_LIBGPIOD
  healthy_ = true;
#else
  healthy_ = false;
#endif
}

LibgpiodPwmController::~LibgpiodPwmController() = default;

bool LibgpiodPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size()) {
    return false;
  }
  duty_[channel] = std::clamp(duty, 0.0, 1.0);
  // This class currently exposes a libgpiod-compatible ownership boundary.
  // Real-time PWM waveform generation is expected to be provided by a hardware-
  // specific backend during Pi integration.
  return healthy_;
}

}  // namespace coatheal