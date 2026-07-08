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

inline bool ParseMissionPhase(const std::string& text, MissionPhase* phase) {
  if (phase == nullptr) {
    return false;
  }
  if (text == "BOOT") {
    *phase = MissionPhase::kBoot;
  } else if (text == "ASCENT") {
    *phase = MissionPhase::kAscent;
  } else if (text == "PRE_FLOAT") {
    *phase = MissionPhase::kPreFloat;
  } else if (text == "FLOAT") {
    *phase = MissionPhase::kFloat;
  } else if (text == "DESCENT") {
    *phase = MissionPhase::kDescent;
  } else if (text == "LANDED") {
    *phase = MissionPhase::kLanded;
  } else if (text == "STOPPED") {
    *phase = MissionPhase::kStopped;
  } else {
    return false;
  }
  return true;
}

}  // namespace coatheal
