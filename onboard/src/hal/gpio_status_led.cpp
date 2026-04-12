#include "coatheal/hal/status_led.hpp"

#include <utility>

namespace coatheal {

GpioStatusLed::GpioStatusLed(std::string chip, std::size_t line, std::string label)
    : chip_(std::move(chip)), line_(line), label_(std::move(label)) {
#ifdef COATHEAL_HAS_LIBGPIOD
  healthy_ = true;
#else
  healthy_ = false;
#endif
}

GpioStatusLed::~GpioStatusLed() = default;

bool GpioStatusLed::Write(bool value) {
  on_ = value;
  return healthy_;
}

bool GpioStatusLed::On() { return Write(true); }
bool GpioStatusLed::Off() { return Write(false); }
bool GpioStatusLed::Toggle() { return Write(!on_); }

bool GpioStatusLed::SetBlink(int hz) {
  blink_hz_ = hz < 0 ? 0 : hz;
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
