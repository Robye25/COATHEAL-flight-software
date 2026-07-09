#pragma once

#include <string>

namespace coatheal {

// Rev C mission phase label. Phase changes never start motor motion.
enum class MissionPhase {
  kBoot,
  kAscent,
  kPreFloat,
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
