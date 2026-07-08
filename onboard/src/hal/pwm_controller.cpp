#include "coatheal/hal/pwm_controller.hpp"

#include <algorithm>
#include <utility>

namespace coatheal {

SimulatedPwmController::SimulatedPwmController(std::size_t channels) : duty_(channels, 0.0) {}

bool SimulatedPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size()) {
    return false;
  }
  duty_[channel] = std::clamp(duty, 0.0, 1.0);
  return true;
}

LibgpiodPwmController::LibgpiodPwmController(
    std::string chip,
    std::size_t channels,
    std::vector<std::size_t> output_lines,
    double pwm_frequency_hz,
    bool active_high)
    : chip_(std::move(chip)),
      output_lines_(std::move(output_lines)),
      pwm_frequency_hz_(pwm_frequency_hz),
      active_high_(active_high),
      duty_(channels, 0.0) {
#ifdef COATHEAL_HAS_LIBGPIOD
  healthy_ = output_lines_.empty() || output_lines_.size() == channels;
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
  // This class currently keeps the configured GPIO ownership boundary for the
  // Electrokit EKM014 MOSFET channels. Real-time PWM waveform generation is
  // still provided by the hardware-specific Pi integration backend.
  (void)pwm_frequency_hz_;
  (void)active_high_;
  return healthy_;
}

}  // namespace coatheal
