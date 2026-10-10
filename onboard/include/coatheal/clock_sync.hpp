#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coatheal {

// TIME_SYNC: the onboard's clock set from the ground station.
//
// The BEXUS E-Link carries no NTP and the Pi has no RTC (schematic v4), so
// after a reboot the system clock is fake-hwclock's last save and stays
// wrong until something disciplines it. The ground station is the only time
// reference on the link: it sends `TIME_SYNC <ground_unix_ms> [<rtt_ms>]`
// when its link budget has room (about every ten minutes, never waiting
// for it), the onboard compares `ground_unix_ms + rtt_ms / 2` with its own
// clock and steps the clock to it when they differ by clock.step_threshold_ms
// or more. The reply carries the offset either way, so the ground station
// also learns the sync state ("check periodically").
//
// The clock step itself (SetSystemClockMs) needs CAP_SYS_TIME: the service
// units grant it with AmbientCapabilities=. These functions are pure except
// SetSystemClockMs, and unit-tested through SystemController with a fake
// clock setter.

struct ClockSyncRequest {
  std::int64_t ground_unix_ms = 0;   // the ground station's clock when it sent the command
  std::int64_t rtt_ms = 0;           // its last measured round trip, 0 when unknown
};

struct ClockSyncPlan {
  std::int64_t offset_ms = 0;        // ground time minus the onboard clock (positive: onboard behind)
  std::int64_t target_unix_ms = 0;   // what the onboard clock should read now
  bool step = false;                 // |offset| reached the threshold: set the clock
};

// Parses the command arguments. A ground time before this firmware's build
// day (build_floor_unix_s) or after 2100 is refused as implausible, so a
// ground station with its own clock at the epoch cannot drag the onboard
// there. rtt_ms is 0..30000.
bool ParseClockSyncArgs(const std::vector<std::string>& args,
                        std::int64_t build_floor_unix_s,
                        ClockSyncRequest* out,
                        std::string* error);

// The decision for a request received when the onboard clock read
// local_now_unix_ms: the target is the ground time plus half the round
// trip (the command's one-way delay); the clock is stepped when the offset
// is at least step_threshold_ms.
ClockSyncPlan PlanClockSync(const ClockSyncRequest& request,
                            std::int64_t local_now_unix_ms,
                            std::int64_t step_threshold_ms);

// Sets CLOCK_REALTIME (Linux only; needs CAP_SYS_TIME). On failure `error`
// names the cause and, for EPERM, the capability the service unit must grant.
bool SetSystemClockMs(std::int64_t unix_ms, std::string* error);

}  // namespace coatheal
