#pragma once

#include <string>

namespace coatheal {

// Rev C mission FSM. All flying phases share a single thermal policy
// (floor >= +5 C). PRE_FLOAT runs the mechanical fatigue pull sequence
// near float altitude; FLOAT is monitoring-only after pulls complete.
enum class MissionPhase {
  kBoot,
  kAscent,
  kPreFloat,   // Fatigue pull sequence near float altitude
  kFloat,
  kDescent,
  kLanded,
  kStopped
};

inline std::string ToString(MissionPhase phase) {
  switch (phase) {
    case MissionPhase::kBoot:
      return "BOOT";
    case MissionPhase::kAscent:
      return "ASCENT";
    case MissionPhase::kPreFloat:
      return "PRE_FLOAT";
    case MissionPhase::kFloat:
      return "FLOAT";
    case MissionPhase::kDescent:
      return "DESCENT";
    case MissionPhase::kLanded:
      return "LANDED";
    case MissionPhase::kStopped:
      return "STOPPED";
  }
  return "UNKNOWN";
}

}  // namespace coatheal
