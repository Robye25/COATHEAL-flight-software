#include "coatheal/hal/stepper_driver.hpp"

#include <chrono>
#include <thread>
#include <utility>

#ifdef COATHEAL_HAS_LIBGPIOD
#include <gpiod.h>
#endif

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
  auto* chip_ptr = gpiod_chip_open(chip_.c_str());
  if (chip_ptr == nullptr) return;
  chip_handle_ = chip_ptr;
  auto* step_ptr = gpiod_chip_get_line(chip_ptr, static_cast<unsigned int>(step_line_));
  auto* dir_ptr = gpiod_chip_get_line(chip_ptr, static_cast<unsigned int>(dir_line_));
  auto* enable_ptr =
      gpiod_chip_get_line(chip_ptr, static_cast<unsigned int>(enable_line_));
  if (step_ptr == nullptr || dir_ptr == nullptr || enable_ptr == nullptr) return;
  const int disabled = enable_active_low_ ? 1 : 0;
  if (gpiod_line_request_output(step_ptr, "coatheal-step", 0) < 0) return;
  step_handle_ = step_ptr;
  if (gpiod_line_request_output(dir_ptr, "coatheal-dir", 0) < 0) return;
  dir_handle_ = dir_ptr;
  if (gpiod_line_request_output(enable_ptr, "coatheal-enable", disabled) < 0) return;
  enable_handle_ = enable_ptr;
  healthy_ = true;
#else
  healthy_ = false;
#endif
}

GpioStepDirStepperDriver::~GpioStepDirStepperDriver() {
  Enable(false);
#ifdef COATHEAL_HAS_LIBGPIOD
  if (step_handle_ != nullptr) {
    gpiod_line_set_value(static_cast<gpiod_line*>(step_handle_), 0);
    gpiod_line_release(static_cast<gpiod_line*>(step_handle_));
  }
  if (dir_handle_ != nullptr) {
    gpiod_line_release(static_cast<gpiod_line*>(dir_handle_));
  }
  if (enable_handle_ != nullptr) {
    gpiod_line_release(static_cast<gpiod_line*>(enable_handle_));
  }
  if (chip_handle_ != nullptr) {
    gpiod_chip_close(static_cast<gpiod_chip*>(chip_handle_));
  }
#endif
}

bool GpioStepDirStepperDriver::Enable(bool enable) {
  if (!healthy_) return false;
#ifdef COATHEAL_HAS_LIBGPIOD
  const int value = enable ? (enable_active_low_ ? 0 : 1)
                           : (enable_active_low_ ? 1 : 0);
  if (gpiod_line_set_value(static_cast<gpiod_line*>(enable_handle_), value) < 0) {
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
    if (gpiod_line_set_value(static_cast<gpiod_line*>(dir_handle_),
                             physical_direction ? 1 : 0) < 0) {
      healthy_ = false;
      return false;
    }
    last_direction_forward_ = physical_direction;
    std::this_thread::sleep_for(std::chrono::microseconds(2));
  }
  if (gpiod_line_set_value(static_cast<gpiod_line*>(step_handle_), 1) < 0) {
    healthy_ = false;
    return false;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(2));
  if (gpiod_line_set_value(static_cast<gpiod_line*>(step_handle_), 0) < 0) {
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
