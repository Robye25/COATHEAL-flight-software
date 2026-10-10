#include "coatheal/bend_sequence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace coatheal {

namespace {

bool ParseInt64Token(const std::string& text, std::int64_t* out) {
  try {
    std::size_t consumed = 0;
    const std::int64_t value = std::stoll(text, &consumed);
    if (consumed != text.size()) return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDoubleToken(const std::string& text, double* out) {
  try {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value)) return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

void Fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
}

}  // namespace

std::size_t BendSequenceDefinition::total_steps() const {
  return steps.size() * static_cast<std::size_t>(std::max(repeat, 0)) + tail.size();
}

const BendSequenceStep& BendSequenceDefinition::at(std::size_t index) const {
  const std::size_t body = steps.size() * static_cast<std::size_t>(std::max(repeat, 0));
  if (index < body) return steps[index % steps.size()];
  return tail[std::min(index - body, tail.size() - 1)];
}

int BendSequenceDefinition::cycle_of(std::size_t index) const {
  if (steps.empty() || repeat < 1) return std::max(repeat, 1);
  const std::size_t cycle = std::min(index / steps.size(),
                                     static_cast<std::size_t>(repeat) - 1);
  return static_cast<int>(cycle) + 1;
}

bool ParseBendSequenceSteps(const std::vector<std::string>& tokens,
                            std::int64_t max_position_usteps,
                            double max_hold_s,
                            BendSequenceDefinition* out,
                            std::string* error) {
  if (out == nullptr) return false;
  BendSequenceDefinition definition;
  definition.name = out->name;
  bool repeat_seen = false;
  for (const std::string& token : tokens) {
    if (token.rfind("repeat=", 0) == 0) {
      if (repeat_seen) {
        Fail(error, "repeat= given twice");
        return false;
      }
      if (definition.steps.empty()) {
        Fail(error, "repeat= needs at least one step before it");
        return false;
      }
      std::int64_t repeat = 0;
      if (!ParseInt64Token(token.substr(7), &repeat) || repeat < 1 ||
          repeat > kMaxSequenceRepeat) {
        Fail(error, "invalid repeat (1.." + std::to_string(kMaxSequenceRepeat) + ")");
        return false;
      }
      definition.repeat = static_cast<int>(repeat);
      repeat_seen = true;
      continue;
    }
    std::vector<std::string> fields;
    {
      std::istringstream spec(token);
      std::string field;
      while (std::getline(spec, field, ':')) fields.push_back(field);
    }
    if (fields.size() == 3) {
      Fail(error, "sequence steps are <target>:<hold>; speed is set per motor"
                  " with STEPPER_SET_SPEED");
      return false;
    }
    if (fields.size() != 2) {
      Fail(error, "invalid sequence step");
      return false;
    }
    BendSequenceStep step;
    if (!ParseInt64Token(fields[0], &step.target_usteps) ||
        !ParseDoubleToken(fields[1], &step.hold_s) ||
        std::llabs(step.target_usteps) > max_position_usteps ||
        step.hold_s < 0.0 || step.hold_s > max_hold_s) {
      Fail(error, "invalid sequence target/hold");
      return false;
    }
    (repeat_seen ? definition.tail : definition.steps).push_back(step);
  }
  if (definition.total_steps() == 0) {
    Fail(error, "sequence has no steps");
    return false;
  }
  *out = std::move(definition);
  return true;
}

}  // namespace coatheal
