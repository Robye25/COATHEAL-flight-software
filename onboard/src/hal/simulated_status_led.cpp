#include "coatheal/hal/status_led.hpp"

#include <iostream>
#include <utility>

namespace coatheal {
namespace {

const char* PatternName(StatusLed::Pattern p) {
  switch (p) {
    case StatusLed::Pattern::kOff: return "OFF";
    case StatusLed::Pattern::kHeartbeat: return "HEARTBEAT";
    case StatusLed::Pattern::kSolid: return "SOLID";
    case StatusLed::Pattern::kFastBlink: return "FAST_BLINK";
    case StatusLed::Pattern::kSOS: return "SOS";
  }
  return "?";
}

}  // namespace

SimulatedStatusLed::SimulatedStatusLed(std::string label, std::size_t line)
    : label_(std::move(label)), line_(line) {
  std::cerr << "[led-sim:" << label_ << "] init line=" << line_ << '\n';
}

bool SimulatedStatusLed::On() {
  if (!on_) {
    std::cerr << "[led-sim:" << label_ << "] ON\n";
  }
  on_ = true;
  return true;
}

bool SimulatedStatusLed::Off() {
  if (on_) {
    std::cerr << "[led-sim:" << label_ << "] OFF\n";
  }
  on_ = false;
  return true;
}

bool SimulatedStatusLed::Toggle() {
  on_ = !on_;
  ++toggle_count_;
  return true;
}

bool SimulatedStatusLed::SetBlink(int hz) {
  blink_hz_ = hz < 0 ? 0 : hz;
  std::cerr << "[led-sim:" << label_ << "] blink_hz=" << blink_hz_ << '\n';
  return true;
}

bool SimulatedStatusLed::Set(Pattern pattern) {
  pattern_ = pattern;
  std::cerr << "[led-sim:" << label_ << "] pattern=" << PatternName(pattern) << '\n';
  switch (pattern) {
    case Pattern::kOff: on_ = false; blink_hz_ = 0; break;
    case Pattern::kSolid: on_ = true; blink_hz_ = 0; break;
    case Pattern::kHeartbeat: blink_hz_ = 1; break;
    case Pattern::kFastBlink: blink_hz_ = 5; break;
    case Pattern::kSOS: blink_hz_ = 3; break;
  }
  return true;
}

}  // namespace coatheal
