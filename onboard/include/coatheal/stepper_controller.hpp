#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/motion_lock.hpp"
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

// Forward-declared so we can hold channels without a circular include. The
// full channel header is pulled in by the .cpp.
class StepperChannel;
struct StepperChannelConfig;

// REV-B stepper controller.
//
// Two-motor capable: holds a vector of StepperChannel objects, one per motor,
// and dispatches commands by motor_id. All single-motor methods preserve the
// REV-A signature and route to channel 0 — the existing system_controller
// integration keeps working unchanged. New id-taking overloads are provided
// for the multi-motor command surface.
//
// Ownership: channels are created in one of three ways
//   1. Legacy ctor (single StepperConfig + driver) — builds a single channel
//      from `cfg` with channel_id=0 and samples=0..3. Backward compatible.
//   2. Multi-channel ctor (vector of StepperChannelConfig + vector of
//      unique_ptr<StepperDriver>) — one channel per pair. The MotionLock is
//      owned by the controller and shared across channels.
//   3. AddChannel() — append a pre-built channel (used by tests / plugins).
class StepperController {
 public:
  // Legacy single-channel constructor. Behaves exactly like REV-A for the one
  // motor it controls; Tick() still applies phase-based bend setpoints to
  // channel 0. Kept so the existing system_controller wiring compiles.
  StepperController(const StepperConfig& cfg,
                    const BendScheduleConfig& schedule,
                    std::unique_ptr<StepperDriver> driver);

  // Multi-channel constructor. `channel_cfgs[i]` pairs with `drivers[i]`.
  // Throws std::invalid_argument if the vectors differ in size.
  StepperController(std::vector<StepperChannelConfig> channel_cfgs,
                    std::vector<std::unique_ptr<StepperDriver>> drivers,
                    const BendScheduleConfig& schedule);

  ~StepperController();

  StepperController(const StepperController&) = delete;
  StepperController& operator=(const StepperController&) = delete;

  // Called once per control-loop tick with the wall-clock dt since the last
  // call. Applies phase-based bend setpoints (to channel 0 only) and ticks
  // every owned channel so their ramp schedulers advance.
  void Tick(MissionPhase phase, double dt_s);

  // ---- Legacy single-motor command surface (routes to channel 0) ----
  bool MoveSteps(std::int64_t delta_steps, std::string* error);
  bool MoveToSteps(std::int64_t absolute_steps, double hold_s, std::string* error);
  bool Rotate(double revolutions, std::string* error);
  bool Home(std::string* error);
  void Stop();
  bool SetSpeed(double step_hz, std::string* error);
  bool SetMicrostep(int divisor, std::string* error);
  bool SetEnabled(bool enable);

  // ---- REV-B multi-motor command surface ----
  bool MoveSteps(int motor_id, std::int64_t delta_steps, std::string* error);
  bool MoveToSteps(int motor_id, std::int64_t absolute_steps, double hold_s,
                   std::string* error);
  bool Rotate(int motor_id, double revolutions, std::string* error);
  bool Home(int motor_id, std::string* error);
  bool Stop(int motor_id, std::string* error);
  bool SetSpeed(int motor_id, double step_hz, std::string* error);
  bool SetMicrostep(int motor_id, int divisor, std::string* error);
  bool SetEnabled(int motor_id, bool enable, std::string* error);

  // Pull-cycle shortcuts. ArmPull queues the motion non-blocking; ExecutePull
  // blocks until the retract leg completes. Both acquire / release the
  // MotionLock atomically.
  bool ArmPull(int motor_id, std::string* error);
  bool ExecutePull(int motor_id, std::string* error);

  // Legacy single-motor snapshot — returns channel 0's status.
  StepperStatus Snapshot() const;
  // Per-channel snapshot.
  StepperStatus Snapshot(int motor_id) const;

  std::size_t channel_count() const;
  // Lookup samples() for a channel (empty if invalid id).
  std::vector<std::size_t> SamplesForMotor(int motor_id) const;

  MotionLock* motion_lock() { return &lock_; }

 private:
  void ApplyPhaseSetpoint(MissionPhase phase);
  bool ResolvePhaseBend(MissionPhase phase, std::int64_t* steps, double* hold_s) const;
  StepperChannel* ChannelById(int motor_id);
  const StepperChannel* ChannelById(int motor_id) const;

  BendScheduleConfig schedule_;
  MotionLock lock_;
  std::vector<std::unique_ptr<StepperChannel>> channels_;

  mutable std::mutex mu_;
  MissionPhase last_phase_ = MissionPhase::kBoot;
  bool last_phase_valid_ = false;
};

}  // namespace coatheal
