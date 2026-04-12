#include <cassert>
#include <iostream>

#include "coatheal/hal/status_led.hpp"

using coatheal::SimulatedStatusLed;
using coatheal::StatusLed;

namespace {

void TestOnOffToggle() {
  SimulatedStatusLed led("t", 17);
  assert(!led.is_on());
  assert(led.On());
  assert(led.is_on());
  assert(led.Off());
  assert(!led.is_on());
  assert(led.Toggle());
  assert(led.is_on());
  assert(led.Toggle());
  assert(!led.is_on());
  assert(led.toggle_count() == 2);
}

void TestSetBlink() {
  SimulatedStatusLed led("t", 27);
  assert(led.SetBlink(5));
  assert(led.blink_hz() == 5);
  assert(led.SetBlink(-3));
  assert(led.blink_hz() == 0);
}

void TestPatternTransitions() {
  SimulatedStatusLed led("mode", 27);

  assert(led.Set(StatusLed::Pattern::kSolid));
  assert(led.pattern() == StatusLed::Pattern::kSolid);
  assert(led.is_on());
  assert(led.blink_hz() == 0);

  assert(led.Set(StatusLed::Pattern::kHeartbeat));
  assert(led.pattern() == StatusLed::Pattern::kHeartbeat);
  assert(led.blink_hz() == 1);

  assert(led.Set(StatusLed::Pattern::kFastBlink));
  assert(led.blink_hz() == 5);

  assert(led.Set(StatusLed::Pattern::kSOS));
  assert(led.pattern() == StatusLed::Pattern::kSOS);
  assert(led.blink_hz() == 3);

  assert(led.Set(StatusLed::Pattern::kOff));
  assert(!led.is_on());
  assert(led.blink_hz() == 0);
}

void TestHealthyAndLine() {
  SimulatedStatusLed led("mode", 27);
  assert(led.healthy());
  assert(led.line() == 27);
}

}  // namespace

int main() {
  TestOnOffToggle();
  TestSetBlink();
  TestPatternTransitions();
  TestHealthyAndLine();
  std::cout << "status_led tests passed.\n";
  return 0;
}
