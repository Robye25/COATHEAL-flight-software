#include "coatheal/command_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace coatheal {
namespace {

std::string Trim(const std::string& input) {
  const auto begin = std::find_if_not(input.begin(), input.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

std::string ToUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

}  // namespace

std::string CommandTypeToString(CommandType type) {
  switch (type) {
    case CommandType::kPing:
      return "PING";
    case CommandType::kStatus:
      return "STATUS";
    case CommandType::kForceStart:
      return "FORCE_START";
    case CommandType::kForceStop:
      return "FORCE_STOP";
    case CommandType::kHeatersOff:
      return "HEATERS_OFF";
    case CommandType::kResetCtrl:
      return "RESET_CTRL";
    case CommandType::kShutdownSafe:
      return "SHUTDOWN_SAFE";
    case CommandType::kCheck:
      return "CHECK";
    case CommandType::kComponents:
      return "COMPONENTS";
    case CommandType::kArm:
      return "ARM";
    case CommandType::kDisarm:
      return "DISARM";
    case CommandType::kEnterSafe:
      return "ENTER_SAFE";
    case CommandType::kExitSafe:
      return "EXIT_SAFE";
    case CommandType::kArmDebug:
      return "ARM_DEBUG";
    case CommandType::kDisarmDebug:
      return "DISARM_DEBUG";
    case CommandType::kSetHeaterDuty:
      return "SET_HEATER_DUTY";
    case CommandType::kSetAllDuty:
      return "SET_ALL_DUTY";
    case CommandType::kSetPid:
      return "SET_PID";
    case CommandType::kSetTempTarget:
      return "SET_TEMP_TARGET";
    case CommandType::kSetAllTempTargets:
      return "SET_ALL_TEMP_TARGETS";
    case CommandType::kClearTempTarget:
      return "CLEAR_TEMP_TARGET";
    case CommandType::kClearTempTargets:
      return "CLEAR_TEMP_TARGETS";
    case CommandType::kGetThermal:
      return "GET_THERMAL";
    case CommandType::kClearOverrides:
      return "CLEAR_OVERRIDES";
    case CommandType::kSetBenchMode:
      return "SET_BENCH_MODE";
    case CommandType::kSetTickHz:
      return "SET_TICK_HZ";
    case CommandType::kRadioSilence:
      return "RADIO_SILENCE";
    case CommandType::kRadioResume:
      return "RADIO_RESUME";
    case CommandType::kSetPhase:
      return "SET_PHASE";
    case CommandType::kStepperMove:
      return "STEPPER_MOVE";
    case CommandType::kStepperMoveTo:
      return "STEPPER_MOVETO";
    case CommandType::kStepperRotate:
      return "STEPPER_ROTATE";
    case CommandType::kStepperHome:
      return "STEPPER_HOME";
    case CommandType::kStepperStop:
      return "STEPPER_STOP";
    case CommandType::kStepperSetSpeed:
      return "STEPPER_SET_SPEED";
    case CommandType::kStepperSetMicrostep:
      return "STEPPER_SET_MICROSTEP";
    case CommandType::kStepperEnable:
      return "STEPPER_ENABLE";
    case CommandType::kStepperDisable:
      return "STEPPER_DISABLE";
    case CommandType::kStepperBend:
      return "STEPPER_BEND";
    case CommandType::kSetPositionZero:
      return "SET_POSITION_ZERO";
    case CommandType::kBendSeqLoad:
      return "BENDSEQ_LOAD";
    case CommandType::kBendSeqRun:
      return "BENDSEQ_RUN";
    case CommandType::kBendSeqPause:
      return "BENDSEQ_PAUSE";
    case CommandType::kBendSeqResume:
      return "BENDSEQ_RESUME";
    case CommandType::kBendSeqStop:
      return "BENDSEQ_STOP";
    case CommandType::kBendSeqStatus:
      return "BENDSEQ_STATUS";
    case CommandType::kBendSeqClear:
      return "BENDSEQ_CLEAR";
    case CommandType::kPullArm:
      return "PULL_ARM";
    case CommandType::kPullExecute:
      return "PULL_EXECUTE";
    case CommandType::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool IsExtendedCommand(CommandType type) {
  switch (type) {
    case CommandType::kSetBenchMode:
    case CommandType::kDisarmDebug:
      return true;
    default:
      return false;
  }
}

CommandParseResult CommandParser::ParseLine(const std::string& line) const {
  CommandParseResult result;

  std::string normalized = Trim(line);
  if (normalized.empty()) {
    result.error = "empty command";
    return result;
  }

  std::replace(normalized.begin(), normalized.end(), ',', ' ');

  std::istringstream iss(normalized);
  std::vector<std::string> tokens;
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }

  if (tokens.empty()) {
    result.error = "empty command";
    return result;
  }

  const std::string cmd = ToUpper(tokens.front());
  tokens.erase(tokens.begin());

  const std::unordered_map<std::string, CommandType> command_map = {
      {"PING", CommandType::kPing},
      {"STATUS", CommandType::kStatus},
      {"FORCE_START", CommandType::kForceStart},
      {"ON", CommandType::kForceStart},
      {"FORCE_STOP", CommandType::kForceStop},
      {"OFF", CommandType::kForceStop},
      {"HEATERS_OFF", CommandType::kHeatersOff},
      {"RESET_CTRL", CommandType::kResetCtrl},
      {"RESET", CommandType::kResetCtrl},
      {"SHUTDOWN_SAFE", CommandType::kShutdownSafe},
      {"CHECK", CommandType::kCheck},
      {"COMPONENTS", CommandType::kComponents},
      {"ARM", CommandType::kArm},
      {"DISARM", CommandType::kDisarm},
      {"ENTER_SAFE", CommandType::kEnterSafe},
      {"EXIT_SAFE", CommandType::kExitSafe},
      {"ARM_DEBUG", CommandType::kArmDebug},
      {"DISARM_DEBUG", CommandType::kDisarmDebug},
      {"SET_HEATER_DUTY", CommandType::kSetHeaterDuty},
      {"SET_ALL_DUTY", CommandType::kSetAllDuty},
      {"SET_PID", CommandType::kSetPid},
      {"SET_TEMP_TARGET", CommandType::kSetTempTarget},
      {"SET_ALL_TEMP_TARGETS", CommandType::kSetAllTempTargets},
      {"CLEAR_TEMP_TARGET", CommandType::kClearTempTarget},
      {"CLEAR_TEMP_TARGETS", CommandType::kClearTempTargets},
      {"GET_THERMAL", CommandType::kGetThermal},
      {"CLEAR_OVERRIDES", CommandType::kClearOverrides},
      {"SET_BENCH_MODE", CommandType::kSetBenchMode},
      {"SET_TICK_HZ", CommandType::kSetTickHz},
      {"RADIO_SILENCE", CommandType::kRadioSilence},
      {"RADIO_RESUME", CommandType::kRadioResume},
      {"SET_PHASE", CommandType::kSetPhase},
      {"STEPPER_MOVE", CommandType::kStepperMove},
      {"STEPPER_MOVETO", CommandType::kStepperMoveTo},
      {"STEPPER_ROTATE", CommandType::kStepperRotate},
      {"STEPPER_HOME", CommandType::kStepperHome},
      {"STEPPER_STOP", CommandType::kStepperStop},
      {"STEPPER_SET_SPEED", CommandType::kStepperSetSpeed},
      {"STEPPER_SET_MICROSTEP", CommandType::kStepperSetMicrostep},
      {"STEPPER_ENABLE", CommandType::kStepperEnable},
      {"STEPPER_DISABLE", CommandType::kStepperDisable},
      {"STEPPER_BEND", CommandType::kStepperBend},
      {"SET_POSITION_ZERO", CommandType::kSetPositionZero},
      {"BENDSEQ_LOAD", CommandType::kBendSeqLoad},
      {"BENDSEQ_RUN", CommandType::kBendSeqRun},
      {"BENDSEQ_PAUSE", CommandType::kBendSeqPause},
      {"BENDSEQ_RESUME", CommandType::kBendSeqResume},
      {"BENDSEQ_STOP", CommandType::kBendSeqStop},
      {"BENDSEQ_STATUS", CommandType::kBendSeqStatus},
      {"BENDSEQ_CLEAR", CommandType::kBendSeqClear},
      {"PULL_ARM", CommandType::kPullArm},
      {"PULL_EXECUTE", CommandType::kPullExecute},
  };

  auto it = command_map.find(cmd);
  if (it == command_map.end()) {
    result.error = "unknown command: " + cmd;
    return result;
  }

  Command command;
  command.type = it->second;
  command.name = CommandTypeToString(command.type);
  command.args = tokens;
  command.is_extended = IsExtendedCommand(command.type);

  auto require_args = [&](std::size_t expected) -> bool {
    if (command.args.size() != expected) {
      result.error = "invalid argument count for " + command.name;
      return false;
    }
    return true;
  };

  // Stepper / PULL_* commands accept an optional leading motor_id
  // argument. The parser peels it into `command.motor_id` and leaves the
  // remaining tokens in `args` so the legacy dispatch surface ("args[0] is
  // the numeric payload") continues to work unchanged.
  //
  // Detection is arity-based: if the arg count is one greater than the
  // legacy count AND the first token is a small unsigned integer, we treat
  // it as the motor id. Otherwise id defaults to 0.
  auto is_small_int_token = [](const std::string& tok) {
    if (tok.empty() || tok.size() > 3) return false;  // motor ids are 0..99
    for (char c : tok) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
  };

  auto maybe_extract_id = [&](std::size_t legacy_min, std::size_t legacy_max) {
    const std::size_t n = command.args.size();
    const std::size_t new_min = legacy_min + 1;
    const std::size_t new_max = legacy_max + 1;
    const bool matches_new = (n >= new_min && n <= new_max);
    if (matches_new && !command.args.empty() &&
        is_small_int_token(command.args[0])) {
      try {
        command.motor_id = std::stoi(command.args[0]);
      } catch (...) {
        command.motor_id = 0;
      }
      command.args.erase(command.args.begin());
    }
  };

  switch (command.type) {
    case CommandType::kPing:
    case CommandType::kStatus:
    case CommandType::kForceStart:
    case CommandType::kForceStop:
    case CommandType::kHeatersOff:
    case CommandType::kResetCtrl:
    case CommandType::kShutdownSafe:
    case CommandType::kArm:
    case CommandType::kDisarm:
    case CommandType::kEnterSafe:
    case CommandType::kExitSafe:
    case CommandType::kDisarmDebug:
    case CommandType::kClearOverrides:
    case CommandType::kClearTempTargets:
    case CommandType::kGetThermal:
    case CommandType::kRadioSilence:
    case CommandType::kRadioResume:
      if (!require_args(0)) {
        return result;
      }
      break;
    case CommandType::kSetPositionZero:
    case CommandType::kBendSeqPause:
    case CommandType::kBendSeqResume:
    case CommandType::kBendSeqStop:
    case CommandType::kBendSeqStatus:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kComponents:
      if (!require_args(0)) return result;
      break;
    case CommandType::kCheck:
      if (command.args.size() > 1) {
        result.error = "invalid argument count for CHECK";
        return result;
      }
      if (!command.args.empty()) {
        command.args[0] = ToUpper(command.args[0]);
      }
      break;
    case CommandType::kBendSeqRun:
      if (!require_args(2)) {
        return result;
      }
      break;
    case CommandType::kBendSeqLoad:
      if (command.args.size() < 3) {
        result.error = "invalid argument count for " + command.name;
        return result;
      }
      break;
    case CommandType::kBendSeqClear:
      if (command.args.size() < 1 || command.args.size() > 2) {
        result.error = "invalid argument count for " + command.name;
        return result;
      }
      break;
    case CommandType::kSetPhase:
      if (!require_args(1)) {
        return result;
      }
      command.args[0] = ToUpper(command.args[0]);
      break;
    case CommandType::kStepperHome:
    case CommandType::kStepperStop:
    case CommandType::kStepperEnable:
    case CommandType::kStepperDisable:
    case CommandType::kPullArm:
    case CommandType::kPullExecute:
      // Accept an optional motor id. Legacy arity: 0. New: 1.
      maybe_extract_id(0, 0);
      if (!require_args(0)) {
        return result;
      }
      break;
    case CommandType::kStepperMove:
    case CommandType::kStepperRotate:
    case CommandType::kStepperSetSpeed:
    case CommandType::kStepperSetMicrostep:
      // Arity after id-peel: 1 (the value).
      maybe_extract_id(1, 1);
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kStepperMoveTo:
    case CommandType::kStepperBend:
      // Arity after id-peel: 1 or 2 (steps, [hold_s]).
      maybe_extract_id(1, 2);
      if (command.args.size() < 1 || command.args.size() > 2) {
        result.error = "invalid argument count for " + command.name;
        return result;
      }
      break;
    case CommandType::kArmDebug:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kSetHeaterDuty:
      if (!require_args(2)) {
        return result;
      }
      break;
    case CommandType::kSetAllDuty:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kSetPid:
      if (!require_args(4)) {
        return result;
      }
      if (ToUpper(command.args[0]) == "ALL") {
        command.args[0] = "ALL";
      }
      break;
    case CommandType::kSetTempTarget:
      if (!require_args(2)) {
        return result;
      }
      break;
    case CommandType::kSetAllTempTargets:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kClearTempTarget:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kSetBenchMode:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kSetTickHz:
      if (!require_args(1)) {
        return result;
      }
      break;
    case CommandType::kUnknown:
      result.error = "unknown command";
      return result;
  }

  result.ok = true;
  result.command = std::move(command);
  return result;
}

}  // namespace coatheal
