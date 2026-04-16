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
  kStepperMove,
  kStepperMoveTo,
  kStepperRotate,
  kStepperHome,
  kStepperStop,
  kStepperSetSpeed,
  kStepperSetMicrostep,
  kStepperEnable,
  kStepperDisable,
  kStepperBend,
  kPullArm,
  kPullExecute,
  kUnknown,
};

struct Command {
  CommandType type = CommandType::kUnknown;
  std::string name;
  std::vector<std::string> args;
  bool is_extended = false;

  // REV-B: stepper commands carry an optional motor_id as their first numeric
  // argument. When present in the wire form (e.g. "STEPPER_MOVE 1 400"), the
  // parser strips it from `args` and stores it here. When absent (legacy
  // form, e.g. "STEPPER_MOVE 400"), motor_id defaults to 0 and `args` keeps
  // the legacy layout so the existing dispatch logic in system_controller
  // continues to work. For non-stepper commands motor_id is always 0.
  int motor_id = 0;
};

struct CommandParseResult {
  bool ok = false;
  Command command;
  std::string error;
};

std::string CommandTypeToString(CommandType type);
bool IsExtendedCommand(CommandType type);

}  // namespace coatheal