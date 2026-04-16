#pragma once

#include <string>

namespace coatheal {

// Rev B mission FSM. All flying phases (ASCENT/FLOAT/DESCENT) share a single
// thermal policy (floor >= +5 C) — the enum is retained per-stage so motor
// pull sequencing at FLOAT can be wired by other subsystems.
enum class MissionPhase {
  kBoot,
  kAscent,
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
