#pragma once

#include <string>

namespace coatheal {

enum class MissionPhase {
  kAscentHold,
  kActivationRamp,
  kFloatHold,
  kDescentFloor,
  kStopped
};

inline std::string ToString(MissionPhase phase) {
  switch (phase) {
    case MissionPhase::kAscentHold:
      return "ASCENT_HOLD_-30C";
    case MissionPhase::kActivationRamp:
      return "ACTIVATION_RAMP_TO_+70C";
    case MissionPhase::kFloatHold:
      return "FLOAT_HOLD_+70C";
    case MissionPhase::kDescentFloor:
      return "DESCENT_FLOOR_-20C";
    case MissionPhase::kStopped:
      return "STOPPED";
  }
  return "UNKNOWN";
}

}  // namespace coatheal