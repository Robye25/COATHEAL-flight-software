// RtcAdapter: the "RTC valid" flag is derived from the system clock's
// state (no RTC exists on schematic v4), not hard-wired true.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>

#include "coatheal/hal/rtc_adapter.hpp"

using namespace coatheal;

namespace {

void TestBuildFloorIsThisProjectsEra() {
  // __DATE__ parsed at compile time: at or after 2026-01-01 and never in
  // the future of the host building the tests.
  const std::int64_t floor = RtcAdapter::BuildFloorUnixSeconds();
  const std::int64_t jan_2026 = 1767225600;  // 2026-01-01T00:00:00Z
  assert(floor >= jan_2026);
  const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  assert(floor <= now_s);
  // The floor is a whole UTC day.
  assert(floor % 86400 == 0);
}

void TestClockAfterBuildIsAFloor() {
  const std::int64_t floor = RtcAdapter::BuildFloorUnixSeconds();
  assert(!RtcAdapter::ClockAfterBuild(0));            // the epoch
  assert(!RtcAdapter::ClockAfterBuild(1440000000));   // 2015, a Pi with no fake-hwclock
  assert(!RtcAdapter::ClockAfterBuild(floor - 1));
  assert(RtcAdapter::ClockAfterBuild(floor));
  assert(RtcAdapter::ClockAfterBuild(floor + 86400 * 365));
}

void TestOverrideWinsAndClears() {
  RtcAdapter rtc;
  rtc.set_valid(false);
  assert(!rtc.valid());
  rtc.set_valid(true);
  assert(rtc.valid());
  rtc.clear_override();
  // Derived answer: whatever this host's clock state is, it must agree
  // with the two public criteria it is built from.
  const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const bool derived = rtc.valid();
  if (!RtcAdapter::ClockAfterBuild(now_s)) assert(!derived);
  if (derived) assert(RtcAdapter::ClockAfterBuild(now_s));
}

void TestIsoTimestampShape() {
  RtcAdapter rtc;
  const std::string ts = rtc.NowUtcIso8601();
  assert(ts.size() == 20);
  assert(ts[4] == '-' && ts[7] == '-' && ts[10] == 'T');
  assert(ts[13] == ':' && ts[16] == ':' && ts[19] == 'Z');
}

}  // namespace

int main() {
  TestBuildFloorIsThisProjectsEra();
  TestClockAfterBuildIsAFloor();
  TestOverrideWinsAndClears();
  TestIsoTimestampShape();
  return 0;
}
