#include "coatheal/hal/stepper_driver.hpp"

#include <utility>

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
  // Ownership boundary: real GPIO acquisition + pulse timing are finalized in
  // the Pi bench backend. The TMC5160 path uses this as a STEP/DIR/EN fallback
  // when SPI current-control bring-up fails.
  healthy_ = true;
#else
  healthy_ = false;
#endif
}

bool GpioStepDirStepperDriver::Enable(bool /*enable*/) { return healthy_; }

bool GpioStepDirStepperDriver::Step(bool /*direction_forward*/) {
  if (!healthy_) {
    return false;
  }
  ++pulses_;
  return true;
}

void GpioStepDirStepperDriver::SetMicrostep(int divisor) {
  if (divisor > 0) {
    microstep_ = divisor;
  }
}

}  // namespace coatheal
