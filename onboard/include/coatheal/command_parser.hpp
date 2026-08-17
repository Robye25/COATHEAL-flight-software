#pragma once

#include <cstddef>
#include <string>

#include "coatheal/command.hpp"

namespace coatheal {

class CommandParser {
 public:
  // motor_count bounds what counts as a "plausible" leading motor-id token
  // when disambiguating STEPPER_MOVETO/STEPPER_BEND's overlapping legacy
  // ("<target> [hold_s]") and indexed ("<id> <target> [hold_s]") forms (see
  // maybe_extract_id in command_parser.cpp). Defaults to 2, matching
  // config.hpp's std::array<MotorConfig, 2> motors. A default-constructed
  // CommandParser (existing call sites) keeps that default unchanged.
  explicit CommandParser(std::size_t motor_count = 2) : motor_count_(motor_count) {}

  CommandParseResult ParseLine(const std::string& line) const;

 private:
  std::size_t motor_count_;
};

}  // namespace coatheal
