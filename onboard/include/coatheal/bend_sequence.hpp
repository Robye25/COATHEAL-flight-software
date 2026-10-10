#pragma once

// Runtime bend sequences (BENDSEQ_LOAD / BENDSEQ_RUN).
//
// A sequence is a list of absolute moves, each with a hold (the soak at that
// position). The wire form is
//
//   BENDSEQ_LOAD <id> <name> <target>:<hold> ... [repeat=<n> <target>:<hold> ...]
//
// The steps before `repeat=` are the body and run `repeat` times in a row --
// a bend cycle is the body `+limit:<upper soak> -limit:<lower soak>` -- and
// the steps after it run once at the end (the return to zero). Without
// `repeat=` every step is in the body and it runs once. Targets are absolute
// microsteps at the motor's live divisor; the ground station converts from
// millimetres. Speed and acceleration are the motor's own (STEPPER_SET_SPEED,
// STEPPER_SET_ACCEL): a step never carries them, and a `:<hz>` third field is
// refused so that an operator who typed one learns where speed is set.
//
// Parsing lives here, outside SystemController, so the expansion rules can be
// unit-tested without a controller.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coatheal {

// Most repeats of a body. A two-step cycle with two 60 s soaks and 2 mm of
// travel each way takes about two and a half minutes, so 1000 cycles is
// already two days of bending.
constexpr std::int64_t kMaxSequenceRepeat = 1000;

struct BendSequenceStep {
  std::int64_t target_usteps = 0;
  double hold_s = 0.0;
};

struct BendSequenceDefinition {
  std::string name;
  std::vector<BendSequenceStep> steps;  // the body, run `repeat` times
  int repeat = 1;
  std::vector<BendSequenceStep> tail;   // once, after the last repeat

  // The expanded sequence the runtime walks: body x repeat, then the tail.
  std::size_t total_steps() const;
  const BendSequenceStep& at(std::size_t index) const;
  // 1-based cycle the expanded index belongs to; the tail counts as the
  // last cycle.
  int cycle_of(std::size_t index) const;
};

// Parses the tokens after `<id> <name>`. `max_position_usteps` bounds every
// target (|target| <= it) and `max_hold_s` every hold. On failure *error
// holds the NACK reason (no commas: reply bodies are comma-framed).
bool ParseBendSequenceSteps(const std::vector<std::string>& tokens,
                            std::int64_t max_position_usteps,
                            double max_hold_s,
                            BendSequenceDefinition* out,
                            std::string* error);

}  // namespace coatheal
