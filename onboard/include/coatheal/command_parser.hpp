#pragma once

#include <string>

#include "coatheal/command.hpp"

namespace coatheal {

class CommandParser {
 public:
  CommandParseResult ParseLine(const std::string& line) const;
};

}  // namespace coatheal