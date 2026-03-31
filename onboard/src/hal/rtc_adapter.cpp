#include "coatheal/hal/rtc_adapter.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace coatheal {

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