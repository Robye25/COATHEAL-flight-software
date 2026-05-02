#pragma once

#include <cstdint>
#include <string>

#include "coatheal/config.hpp"

namespace coatheal {

class StepperController;

// Rev C: orchestrates the PRE_FLOAT mechanical fatigue pull sequence.
//
// The sequencer drives two motors in series. For each motor:
//   1. FATIGUE: pull-release-pull-release (N cycles, configurable)
//   2. SOAK: final pull and hold for configurable duration (default 15 min)
//
// Motors and heaters never run simultaneously (enforced by the MotionLock +
// HeaterScheduler interlock). Only one motor runs at a time (enforced by
// the MotionLock). The pulling mechanism is irreversible so deactivating
// motors maintains position.
//
// Usage: call Tick() every control-loop tick while in PRE_FLOAT. When
// is_complete() returns true, set state_overrides.fatigue_complete = true
// so the StateManager transitions to FLOAT.
class FatigueSequencer {
 public:
  FatigueSequencer(const FatigueConfig& cfg, StepperController* stepper);

  // Called each tick during PRE_FLOAT. Returns true when the entire
  // sequence (both motors) is complete.
  bool Tick(double dt_s);

  // Reset the sequencer for re-entry (e.g., after a ground RESET command).
  void Reset();

  bool is_complete() const { return stage_ == Stage::kComplete; }
  int current_motor() const { return current_motor_; }
  int current_cycle() const { return current_cycle_; }
  bool is_soaking() const { return stage_ == Stage::kMotor0Soak || stage_ == Stage::kMotor1Soak; }

  // Human-readable stage name for telemetry/logging.
  std::string stage_name() const;

 private:
  enum class Stage {
    kMotor0Fatigue,     // Pull-release cycling on motor 0
    kMotor0Soak,        // Final hold on motor 0
    kMotor1Fatigue,     // Pull-release cycling on motor 1
    kMotor1Soak,        // Final hold on motor 1
    kComplete           // Both done
  };

  // Internal sub-state for tracking where we are within a single pull cycle.
  enum class PullPhase {
    kIdle,              // Ready to start next action
    kWaitingForPull,    // Pull armed, waiting for completion (lock release)
  };

  bool StartPull(int motor_id, int travel_full_steps, double hold_s);
  bool IsPullComplete(int motor_id) const;

  FatigueConfig cfg_;
  StepperController* stepper_;

  Stage stage_ = Stage::kMotor0Fatigue;
  PullPhase pull_phase_ = PullPhase::kIdle;
  int current_motor_ = 0;
  int current_cycle_ = 0;
  double soak_timer_s_ = 0.0;
};

}  // namespace coatheal
