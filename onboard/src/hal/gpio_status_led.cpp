#include "coatheal/hal/status_led.hpp"

#include <utility>

#include "coatheal/hal/gpio_output.hpp"

namespace coatheal {

GpioStatusLed::GpioStatusLed(std::string chip, std::size_t line, std::string label)
    : chip_(std::move(chip)), line_(line), label_(std::move(label)) {
#ifdef COATHEAL_HAS_LIBGPIOD
  line_handle_ = RequestGpioOutput(chip_, line_, label_.c_str(), false);
  healthy_ = line_handle_ != nullptr;
#else
  healthy_ = false;
#endif
}

GpioStatusLed::~GpioStatusLed() {
  if (line_handle_ != nullptr) {
    SetGpioOutput(static_cast<GpioOutput*>(line_handle_), false);
    ReleaseGpioOutput(static_cast<GpioOutput*>(line_handle_));
    line_handle_ = nullptr;
  }
}

bool GpioStatusLed::Write(bool value) {
  if (!healthy_ ||
      !SetGpioOutput(static_cast<GpioOutput*>(line_handle_), value)) {
    healthy_ = false;
    return false;
  }
  on_ = value;
  return true;
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
