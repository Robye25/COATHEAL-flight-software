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
  kCheck,
  kComponents,
  kArm,
  kDisarm,
  kEnterSafe,
  kExitSafe,
  kArmDebug,
  kDisarmDebug,
  kSetHeaterDuty,
  kSetAllDuty,
  kHeaterTest,
  kSetPid,
  // Relay PID auto-tune sequence (bench commissioning):
  // PID_TUNE_START <heater> <setpoint_c> [relay_duty] [cycles],
  // PID_TUNE_ABORT, PID_TUNE_STATUS.
  kPidTuneStart,
  kPidTuneAbort,
  kPidTuneStatus,
  kSetTempTarget,
  kSetAllTempTargets,
  kClearTempTarget,
  kClearTempTargets,
  kGetThermal,
  // GET_LAYOUT: which samples and heaters each motor group has, the click
  // samples, and the wiring behind them (card terminal, heater BCM line).
  kGetLayout,
  kClearOverrides,
  kSetBenchMode,
  kSetTickHz,
  kRadioSilence,
  kRadioResume,
  kSetPhase,
  kStepperMove,
  kStepperMoveTo,
  // Distance surface (ball-screw lead conversion happens onboard):
  // STEPPER_MOVE_MM <id> <mm>, STEPPER_MOVETO_MM <id> <mm> [hold_s].
  kStepperMoveMm,
  kStepperMoveToMm,
  kStepperRotate,
  kStepperHome,
  kStepperStop,
  kStepperSetSpeed,
  kStepperSetAccel,
  kStepperSetCurrent,
  kStepperSetMicrostep,
  kStepperEnable,
  kStepperDisable,
  kStepperBend,
  kSetPositionZero,
  // STEPLOSS_ACK <id>: clear the position-uncertain latch a step-loss event
  // set, keeping the zero (SET_POSITION_ZERO clears it with a new zero).
  kStepLossAck,
  // TIME_SYNC <ground_unix_ms> [<rtt_ms>]: the ground station's clock. The
  // onboard steps its own to it when they differ by clock.step_threshold_ms
  // or more and reports the offset either way (clock_sync.hpp).
  kTimeSync,
  kBendSeqLoad,
  kBendSeqRun,
  kBendSeqPause,
  kBendSeqResume,
  kBendSeqStop,
  kBendSeqStatus,
  kBendSeqClear,
  kPullArm,
  kPullExecute,
  // Link-loss failsafe plan (redesign spec §10).
  kFallbackPlan,
  kFallbackArm,
  kFallbackDisarm,
  kFallbackStatus,
  kMotorDebug,
  kUnknown,
};

struct Command {
  CommandType type = CommandType::kUnknown;
  std::string name;
  std::vector<std::string> args;
  bool is_extended = false;

  // Stepper commands carry an optional motor_id as their first numeric
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
