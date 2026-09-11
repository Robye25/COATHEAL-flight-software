#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace coatheal {

// There is no RTC on schematic v4. "RTC" here is the Pi's system clock,
// which boots from fake-hwclock's last hourly save (so it is wrong by the
// whole powered-off interval) and only becomes right once an NTP source
// disciplines it. valid() is therefore DERIVED, never assumed:
//
//   1. the clock is not before this firmware's build date -- a clock that
//      is, is the epoch or a stale fake-hwclock restore; and
//   2. the kernel has reported the clock NTP-synchronised since boot:
//      adjtimex() with STA_UNSYNC clear, or systemd-timesyncd's per-boot
//      marker file. Latched once seen: the kernel re-raises STA_UNSYNC
//      about nine hours after the last correction, and a clock disciplined
//      nine hours ago is still fine for telemetry timestamps.
//
// Until 2026-09 this class returned a constant true, so the ground
// station's "RTC invalid" alarm and checkout row could never fire.
// set_valid() forces the answer (tests, or a bench with no NTP source).
class RtcAdapter {
 public:
  bool valid() const;
  void set_valid(bool value) { override_ = value; }
  void clear_override() { override_.reset(); }

  std::string NowUtcIso8601() const;

  // Unix seconds at 00:00 UTC of the day this translation unit was built.
  static std::int64_t BuildFloorUnixSeconds();
  // Criterion 1 for an arbitrary clock reading (unit-testable).
  static bool ClockAfterBuild(std::int64_t now_unix_seconds);
  // Criterion 2, one un-latched look at the kernel/timesyncd state.
  static bool ClockSynchronisedNow();

 private:
  std::optional<bool> override_;
  mutable std::atomic<bool> synced_since_start_{false};
};

}  // namespace coatheal
