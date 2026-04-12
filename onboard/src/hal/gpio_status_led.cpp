#include "coatheal/hal/status_led.hpp"

#include <utility>

#ifdef COATHEAL_HAS_LIBGPIOD
#include <gpiod.h>
#endif

namespace coatheal {

GpioStatusLed::GpioStatusLed(std::string chip, std::size_t line, std::string label)
    : chip_(std::move(chip)), line_(line), label_(std::move(label)) {
#ifdef COATHEAL_HAS_LIBGPIOD
  auto* chip_ptr = gpiod_chip_open(chip_.c_str());
  if (chip_ptr != nullptr) {
    auto* line_ptr = gpiod_chip_get_line(chip_ptr, static_cast<unsigned>(line_));
    if (line_ptr != nullptr &&
        gpiod_line_request_output(line_ptr, label_.c_str(), 0) == 0) {
      chip_handle_ = chip_ptr;
      line_handle_ = line_ptr;
      healthy_ = true;
    } else {
      gpiod_chip_close(chip_ptr);
    }
  }
#endif
}

GpioStatusLed::~GpioStatusLed() {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (line_handle_ != nullptr) {
    gpiod_line_release(static_cast<gpiod_line*>(line_handle_));
  }
  if (chip_handle_ != nullptr) {
    gpiod_chip_close(static_cast<gpiod_chip*>(chip_handle_));
  }
#endif
}

bool GpioStatusLed::Write(bool value) {
  on_ = value;
#ifdef COATHEAL_HAS_LIBGPIOD
  if (line_handle_ == nullptr) {
    return false;
  }
  return gpiod_line_set_value(static_cast<gpiod_line*>(line_handle_),
                              value ? 1 : 0) == 0;
#else
  return healthy_;
#endif
}

bool GpioStatusLed::On() { return Write(true); }
bool GpioStatusLed::Off() { return Write(false); }
bool GpioStatusLed::Toggle() { return Write(!on_); }

bool GpioStatusLed::SetBlink(int hz) {
  blink_hz_ = hz < 0 ? 0 : hz;
  // Software blink cadence is driven from the main tick loop; the HAL only
  // records the requested rate. A hardware PWM backend can override this.
  return healthy_;
}

bool GpioStatusLed::Set(Pattern pattern) {
  pattern_ = pattern;
  switch (pattern) {
    case Pattern::kOff:
      blink_hz_ = 0;
      return Write(false);
    case Pattern::kSolid:
      blink_hz_ = 0;
      return Write(true);
    case Pattern::kHeartbeat:
      blink_hz_ = 1;
      return healthy_;
    case Pattern::kFastBlink:
      blink_hz_ = 5;
      return healthy_;
    case Pattern::kSOS:
      blink_hz_ = 3;
      return healthy_;
  }
  return false;
}

}  // namespace coatheal
