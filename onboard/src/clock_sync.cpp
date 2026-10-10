#include "coatheal/clock_sync.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace coatheal {

namespace {

constexpr std::int64_t kYear2100UnixMs = 4102444800000LL;
constexpr std::int64_t kMaxRttMs = 30000;

bool ParseInt64Token(const std::string& text, std::int64_t* out) {
  try {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size()) return false;
    *out = static_cast<std::int64_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

void Fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
}

}  // namespace

bool ParseClockSyncArgs(const std::vector<std::string>& args,
                        std::int64_t build_floor_unix_s,
                        ClockSyncRequest* out,
                        std::string* error) {
  if (out == nullptr) return false;
  if (args.empty() || args.size() > 2) {
    Fail(error, "usage: TIME_SYNC <ground_unix_ms> [<rtt_ms>]");
    return false;
  }
  ClockSyncRequest request;
  if (!ParseInt64Token(args[0], &request.ground_unix_ms)) {
    Fail(error, "invalid ground time (unix milliseconds expected)");
    return false;
  }
  if (request.ground_unix_ms < build_floor_unix_s * 1000 ||
      request.ground_unix_ms >= kYear2100UnixMs) {
    Fail(error, "implausible ground time (before this firmware was built, or after 2100)");
    return false;
  }
  if (args.size() == 2) {
    if (!ParseInt64Token(args[1], &request.rtt_ms) || request.rtt_ms < 0 ||
        request.rtt_ms > kMaxRttMs) {
      Fail(error, "invalid rtt_ms (0..30000)");
      return false;
    }
  }
  *out = request;
  return true;
}

ClockSyncPlan PlanClockSync(const ClockSyncRequest& request,
                            std::int64_t local_now_unix_ms,
                            std::int64_t step_threshold_ms) {
  ClockSyncPlan plan;
  plan.target_unix_ms = request.ground_unix_ms + request.rtt_ms / 2;
  plan.offset_ms = plan.target_unix_ms - local_now_unix_ms;
  plan.step = std::llabs(plan.offset_ms) >= (step_threshold_ms < 0 ? 0 : step_threshold_ms);
  return plan;
}

bool SetSystemClockMs(std::int64_t unix_ms, std::string* error) {
#if defined(__linux__)
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(unix_ms / 1000);
  ts.tv_nsec = static_cast<long>((unix_ms % 1000) * 1000000LL);
  if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
    const int err = errno;
    Fail(error, std::string("cannot set the clock: ") + std::strerror(err) +
                    (err == EPERM ? " (the service needs AmbientCapabilities=CAP_SYS_TIME)" : ""));
    return false;
  }
  return true;
#else
  (void)unix_ms;
  Fail(error, "setting the clock is supported on Linux only");
  return false;
#endif
}

}  // namespace coatheal
