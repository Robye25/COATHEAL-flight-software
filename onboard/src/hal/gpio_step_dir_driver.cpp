#include "coatheal/hal/stepper_driver.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "coatheal/hal/gpio_output.hpp"

namespace coatheal {

GpioStepDirStepperDriver::GpioStepDirStepperDriver(std::string chip,
                                                   std::size_t step_line,
                                                   std::size_t dir_line,
                                                   std::size_t enable_line,
                                                   bool invert_direction,
                                                   bool enable_active_low)
    : chip_(std::move(chip)),
      step_line_(step_line),
      dir_line_(dir_line),
      enable_line_(enable_line),
      invert_direction_(invert_direction),
      enable_active_low_(enable_active_low) {
#ifdef COATHEAL_HAS_LIBGPIOD
  const int disabled = enable_active_low_ ? 1 : 0;
  step_handle_ =
      RequestGpioOutput(chip_, step_line_, "coatheal-step", false);
  dir_handle_ =
      RequestGpioOutput(chip_, dir_line_, "coatheal-dir", false);
  enable_handle_ = RequestGpioOutput(
      chip_, enable_line_, "coatheal-enable", disabled != 0);
  if (step_handle_ == nullptr || dir_handle_ == nullptr ||
      enable_handle_ == nullptr) {
    ReleaseGpioOutput(static_cast<GpioOutput*>(step_handle_));
    ReleaseGpioOutput(static_cast<GpioOutput*>(dir_handle_));
    ReleaseGpioOutput(static_cast<GpioOutput*>(enable_handle_));
    step_handle_ = nullptr;
    dir_handle_ = nullptr;
    enable_handle_ = nullptr;
    return;
  }
  healthy_ = true;
#else
  healthy_ = false;
#endif
}

GpioStepDirStepperDriver::~GpioStepDirStepperDriver() {
  Enable(false);
#ifdef COATHEAL_HAS_LIBGPIOD
  if (step_handle_ != nullptr) {
    SetGpioOutput(static_cast<GpioOutput*>(step_handle_), false);
  }
  ReleaseGpioOutput(static_cast<GpioOutput*>(step_handle_));
  ReleaseGpioOutput(static_cast<GpioOutput*>(dir_handle_));
  ReleaseGpioOutput(static_cast<GpioOutput*>(enable_handle_));
#endif
}

bool GpioStepDirStepperDriver::Enable(bool enable) {
  if (!healthy_) return false;
#ifdef COATHEAL_HAS_LIBGPIOD
  const int value = enable ? (enable_active_low_ ? 0 : 1)
                           : (enable_active_low_ ? 1 : 0);
  if (!SetGpioOutput(static_cast<GpioOutput*>(enable_handle_), value != 0)) {
    healthy_ = false;
    return false;
  }
#endif
  enabled_ = enable;
  return true;
}

bool GpioStepDirStepperDriver::Step(bool direction_forward) {
  if (!healthy_ || !enabled_) {
    return false;
  }
#ifdef COATHEAL_HAS_LIBGPIOD
  const bool physical_direction = direction_forward != invert_direction_;
  if (physical_direction != last_direction_forward_) {
    if (!SetGpioOutput(static_cast<GpioOutput*>(dir_handle_),
                       physical_direction)) {
      healthy_ = false;
      return false;
    }
    last_direction_forward_ = physical_direction;
    std::this_thread::sleep_for(std::chrono::microseconds(2));
  }
  if (!SetGpioOutput(static_cast<GpioOutput*>(step_handle_), true)) {
    healthy_ = false;
    return false;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(2));
  if (!SetGpioOutput(static_cast<GpioOutput*>(step_handle_), false)) {
    healthy_ = false;
    return false;
  }
#endif
  ++pulses_;
  return true;
}

void GpioStepDirStepperDriver::SetMicrostep(int divisor) {
  if (divisor > 0) {
    microstep_ = divisor;
  }
}

}  // namespace coatheal
