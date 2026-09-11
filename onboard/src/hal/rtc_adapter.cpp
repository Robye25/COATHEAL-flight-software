#include "coatheal/hal/rtc_adapter.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/timex.h>
#endif

namespace coatheal {

namespace {

// Howard Hinnant's days-from-civil, constexpr so the build floor is a
// compile-time constant derived from __DATE__ ("Mmm dd yyyy").
constexpr std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr unsigned MonthFromDate(const char* date) {
  // __DATE__ month names are the C-locale English abbreviations.
  const char a = date[0], b = date[1], c = date[2];
  if (a == 'J' && b == 'a') return 1;
  if (a == 'F') return 2;
  if (a == 'M' && c == 'r') return 3;
  if (a == 'A' && b == 'p') return 4;
  if (a == 'M' && c == 'y') return 5;
  if (a == 'J' && c == 'n') return 6;
  if (a == 'J' && c == 'l') return 7;
  if (a == 'A' && b == 'u') return 8;
  if (a == 'S') return 9;
  if (a == 'O') return 10;
  if (a == 'N') return 11;
  return 12;
}

constexpr unsigned Digit(char c) { return c == ' ' ? 0U : static_cast<unsigned>(c - '0'); }

constexpr std::int64_t BuildFloorFromDate(const char* date) {
  const unsigned month = MonthFromDate(date);
  const unsigned day = Digit(date[4]) * 10 + Digit(date[5]);
  const std::int64_t year = static_cast<std::int64_t>(
      Digit(date[7]) * 1000 + Digit(date[8]) * 100 + Digit(date[9]) * 10 +
      Digit(date[10]));
  return DaysFromCivil(year, month, day) * 86400;
}

constexpr std::int64_t kBuildFloorUnixSeconds = BuildFloorFromDate(__DATE__);
static_assert(kBuildFloorUnixSeconds > DaysFromCivil(2026, 1, 1) * 86400,
              "__DATE__ parsed to a date before this project existed");

// systemd-timesyncd creates this on its first successful synchronisation
// after boot and leaves it there (it lives on /run) -- a per-boot latch
// that also survives a restart of this service.
constexpr const char* kTimesyncdMarker = "/run/systemd/timesync/synchronized";

}  // namespace

std::int64_t RtcAdapter::BuildFloorUnixSeconds() {
  return kBuildFloorUnixSeconds;
}

bool RtcAdapter::ClockAfterBuild(std::int64_t now_unix_seconds) {
  return now_unix_seconds >= kBuildFloorUnixSeconds;
}

bool RtcAdapter::ClockSynchronisedNow() {
#if defined(__linux__)
  struct timex tx {};
  tx.modes = 0;  // query only, no adjustment (needs no privilege)
  if (adjtimex(&tx) >= 0 && (tx.status & STA_UNSYNC) == 0) return true;
  struct stat st {};
  return stat(kTimesyncdMarker, &st) == 0;
#else
  // No portable way to ask; development hosts keep their own clocks right.
  return true;
#endif
}

bool RtcAdapter::valid() const {
  if (override_.has_value()) return *override_;
  if (!synced_since_start_.load() && ClockSynchronisedNow()) {
    synced_since_start_.store(true);
  }
  const auto now = std::chrono::system_clock::now();
  const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         now.time_since_epoch())
                         .count();
  return synced_since_start_.load() && ClockAfterBuild(now_s);
}

std::string RtcAdapter::NowUtcIso8601() const {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &t);
#else
  gmtime_r(&t, &utc_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

}  // namespace coatheal
