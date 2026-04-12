#pragma once

#include <string>

namespace coatheal {

enum class SystemMode {
  kStandby,
  kRun,
  kSafe,
};

inline std::string ToString(SystemMode mode) {
  switch (mode) {
    case SystemMode::kStandby:
      return "STANDBY";
    case SystemMode::kRun:
      return "RUN";
    case SystemMode::kSafe:
      return "SAFE";
  }
  return "UNKNOWN";
}

}  // namespace coatheal
