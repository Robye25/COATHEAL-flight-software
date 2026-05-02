#include "coatheal/fatigue_sequencer.hpp"

#include <iostream>

#include "coatheal/stepper_controller.hpp"

namespace coatheal {

FatigueSequencer::FatigueSequencer(const FatigueConfig& cfg,
                                   StepperController* stepper)
    : cfg_(cfg), stepper_(stepper) {}

void FatigueSequencer::Reset() {
  stage_ = Stage::kMotor0Fatigue;
  pull_phase_ = PullPhase::kIdle;
  current_motor_ = 0;
  current_cycle_ = 0;
  soak_timer_s_ = 0.0;
}

bool FatigueSequencer::StartPull(int motor_id, int travel_full_steps,
                                 double hold_s) {
  if (!stepper_) return false;

  // Configure the channel's pull travel and hold time before arming.
  // ArmPull uses the channel's existing pull_travel_full_steps and
  // pull_hold_s, so we need to move to the target position directly.
  // Use MoveToSteps for precise control: move to travel_full_steps *
  // microstep, hold for hold_s, then retract to 0.
  std::string err;
  // Get the microstep divisor from the channel snapshot.
  StepperStatus snap = stepper_->Snapshot(motor_id);
  int microstep = std::max(1, snap.microstep);
  std::int64_t target_usteps =
      static_cast<std::int64_t>(travel_full_steps) * microstep;
  if (!stepper_->MoveToSteps(motor_id, target_usteps, hold_s, &err)) {
    std::cerr << "[fatigue] failed to start pull on motor " << motor_id
              << ": " << err << '\n';
    return false;
  }
  return true;
}

bool FatigueSequencer::IsPullComplete(int motor_id) const {
  if (!stepper_) return true;
  StepperStatus snap = stepper_->Snapshot(motor_id);
  // Pull is complete when the channel is idle (not moving, not holding).
  return !snap.moving && !snap.holding;
}

bool FatigueSequencer::Tick(double dt_s) {
  if (stage_ == Stage::kComplete) return true;
  if (!stepper_) {
    stage_ = Stage::kComplete;
    return true;
  }

  switch (stage_) {
    case Stage::kMotor0Fatigue:
    case Stage::kMotor1Fatigue: {
      current_motor_ = (stage_ == Stage::kMotor0Fatigue) ? 0 : 1;

      if (pull_phase_ == PullPhase::kIdle) {
        if (current_cycle_ >= cfg_.fatigue_cycles) {
          // All fatigue cycles done for this motor. Transition to soak.
          current_cycle_ = 0;
          pull_phase_ = PullPhase::kIdle;
          stage_ = (stage_ == Stage::kMotor0Fatigue)
                       ? Stage::kMotor0Soak
                       : Stage::kMotor1Soak;
          soak_timer_s_ = 0.0;
          std::cerr << "[fatigue] motor " << current_motor_
                    << " fatigue cycles complete, entering soak\n";
          break;
        }

        // Start a fatigue pull-release cycle. The hold_s is set so the
        // motor holds briefly at the extended position before retracting.
        if (StartPull(current_motor_, cfg_.fatigue_travel_full_steps,
                      cfg_.fatigue_pull_hold_s)) {
          pull_phase_ = PullPhase::kWaitingForPull;
          std::cerr << "[fatigue] motor " << current_motor_ << " cycle "
                    << current_cycle_ << "/" << cfg_.fatigue_cycles
                    << " started\n";
        }
      } else if (pull_phase_ == PullPhase::kWaitingForPull) {
        if (IsPullComplete(current_motor_)) {
          ++current_cycle_;
          pull_phase_ = PullPhase::kIdle;
        }
      }
      break;
    }

    case Stage::kMotor0Soak:
    case Stage::kMotor1Soak: {
      current_motor_ = (stage_ == Stage::kMotor0Soak) ? 0 : 1;

      if (pull_phase_ == PullPhase::kIdle && soak_timer_s_ == 0.0) {
        // Start the soak: pull to position and hold for soak_hold_s.
        if (StartPull(current_motor_, cfg_.soak_travel_full_steps,
                      cfg_.soak_hold_s)) {
          pull_phase_ = PullPhase::kWaitingForPull;
          std::cerr << "[fatigue] motor " << current_motor_
                    << " soak started (hold " << cfg_.soak_hold_s << " s)\n";
        }
      } else if (pull_phase_ == PullPhase::kWaitingForPull) {
        soak_timer_s_ += dt_s;
        if (IsPullComplete(current_motor_)) {
          pull_phase_ = PullPhase::kIdle;
          soak_timer_s_ = 0.0;
          current_cycle_ = 0;

          if (stage_ == Stage::kMotor0Soak) {
            // Motor 0 soak complete. Proceed to motor 1 fatigue.
            stage_ = Stage::kMotor1Fatigue;
            std::cerr << "[fatigue] motor 0 soak complete, starting motor 1\n";
          } else {
            // Motor 1 soak complete. Entire sequence done.
            stage_ = Stage::kComplete;
            std::cerr << "[fatigue] motor 1 soak complete, sequence finished\n";
          }
        }
      }
      break;
    }

    case Stage::kComplete:
      break;
  }

  return stage_ == Stage::kComplete;
}

std::string FatigueSequencer::stage_name() const {
  switch (stage_) {
    case Stage::kMotor0Fatigue:
      return "M0_FATIGUE";
    case Stage::kMotor0Soak:
      return "M0_SOAK";
    case Stage::kMotor1Fatigue:
      return "M1_FATIGUE";
    case Stage::kMotor1Soak:
      return "M1_SOAK";
    case Stage::kComplete:
      return "COMPLETE";
  }
  return "UNKNOWN";
}

}  // namespace coatheal
