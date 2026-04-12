#pragma once

#include <string>
#include <vector>

namespace coatheal {

enum class CommandType {
  kPing,
  kStatus,
  kForceStart,
  kForceStop,
  kHeatersOff,
  kResetCtrl,
  kShutdownSafe,
  kArm,
  kDisarm,
  kEnterSafe,
  kExitSafe,
  kSecondaryCycle,
  kArmDebug,
  kDisarmDebug,
  kSetHeaterDuty,
  kSetAllDuty,
  kSetPid,
  kClearOverrides,
  kSetBenchMode,
  kSetTickHz,
  kRadioSilence,
  kRadioResume,
  kUnknown,
};

struct Command {
  CommandType type = CommandType::kUnknown;
  std::string name;
  std::vector<std::string> args;
  bool is_extended = false;
};

struct CommandParseResult {
  bool ok = false;
  Command command;
  std::string error;
};

std::string CommandTypeToString(CommandType type);
bool IsExtendedCommand(CommandType type);

}  // namespace coatheal