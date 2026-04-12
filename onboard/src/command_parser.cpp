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
    case CommandType::kArm:
      return "ARM";
    case CommandType::kDisarm:
      return "DISARM";
    case CommandType::kEnterSafe:
      return "ENTER_SAFE";
    case CommandType::kExitSafe:
      return "EXIT_SAFE";
    case CommandType::kSecondaryCycle:
      return "SECONDARY_CYCLE";
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
    case CommandType::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool IsExtendedCommand(CommandType type) {
  switch (type) {
    case CommandType::kSetHeaterDuty:
    case CommandType::kSetAllDuty:
    case CommandType::kSetPid:
    case CommandType::kClearOverrides:
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
      {"ARM", CommandType::kArm},
      {"DISARM", CommandType::kDisarm},
      {"ENTER_SAFE", CommandType::kEnterSafe},
      {"EXIT_SAFE", CommandType::kExitSafe},
      {"SECONDARY_CYCLE", CommandType::kSecondaryCycle},
      {"ARM_DEBUG", CommandType::kArmDebug},
      {"DISARM_DEBUG", CommandType::kDisarmDebug},
      {"SET_HEATER_DUTY", CommandType::kSetHeaterDuty},
      {"SET_ALL_DUTY", CommandType::kSetAllDuty},
      {"SET_PID", CommandType::kSetPid},
      {"CLEAR_OVERRIDES", CommandType::kClearOverrides},
      {"SET_BENCH_MODE", CommandType::kSetBenchMode},
      {"SET_TICK_HZ", CommandType::kSetTickHz},
      {"RADIO_SILENCE", CommandType::kRadioSilence},
      {"RADIO_RESUME", CommandType::kRadioResume},
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
    case CommandType::kSecondaryCycle:
    case CommandType::kDisarmDebug:
    case CommandType::kClearOverrides:
    case CommandType::kRadioSilence:
    case CommandType::kRadioResume:
      if (!require_args(0)) {
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
      if (!require_args(3)) {
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