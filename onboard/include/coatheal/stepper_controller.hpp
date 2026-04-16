#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "coatheal/config.hpp"
#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/phase.hpp"

namespace coatheal {

// Snapshot of stepper state used by telemetry and the CSV log. Every field is
// meaningful to the ground operator during flight.
struct StepperStatus {
  std::int64_t position_steps = 0;      // current absolute position, signed
  std::int64_t target_steps = 0;        // target for the active motion
  double step_hz = 0.0;                 // configured step rate
  int microstep = 1;                    // current microstep divisor
  bool enabled = false;                 // driver power stage state
  bool moving = false;                  // pulses currently being issued
  bool holding = false;                 // at target, bend-hold countdown active
  double hold_remaining_s = 0.0;        // 0 when not holding
  std::uint64_t pulses_total = 0;       // driver.pulses_issued() mirror
  std::string last_source;              // "phase:FLOAT_HOLD", "cmd:MOVE", ...
};

class StepperController {
 public:
  StepperController(const StepperConfig& cfg,
                    const BendScheduleConfig& schedule,
                    std::unique_ptr<StepperDriver> driver);

  // Called once per control-loop tick with the wall-clock dt since the last
  // call. Emits pulses up to the configured step rate, advances the hold
  // timer, and triggers phase-based bend setpoints on phase entry.
  void Tick(MissionPhase phase, double dt_s);

  // Command surface — all return false + set *error on rejection.
  bool MoveSteps(std::int64_t delta_steps, std::string* error);
  bool MoveToSteps(std::int64_t absolute_steps, double hold_s, std::string* error);
  bool Rotate(double revolutions, std::string* error);
  bool Home(std::string* error);             // return to 0
  void Stop();                               // abort current motion
  bool SetSpeed(double step_hz, std::string* error);
  bool SetMicrostep(int divisor, std::string* error);
  bool SetEnabled(bool enable);

  StepperStatus Snapshot() const;

 private:
  void ApplyPhaseSetpoint(MissionPhase phase);
  bool ResolvePhaseBend(MissionPhase phase, std::int64_t* steps, double* hold_s) const;

  StepperConfig cfg_;
  BendScheduleConfig schedule_;
  std::unique_ptr<StepperDriver> driver_;

  mutable std::mutex mu_;
  std::int64_t position_ = 0;
  std::int64_t target_ = 0;
  double step_hz_ = 0.0;
  int microstep_ = 1;
  bool enabled_ = false;
  bool moving_ = false;
  double hold_remaining_s_ = 0.0;
  double fractional_steps_ = 0.0;  // accumulator for sub-integer pulses per tick
  std::string last_source_ = "init";
  MissionPhase last_phase_ = MissionPhase::kBoot;
  bool last_phase_valid_ = false;
};

}  // namespace coatheal
