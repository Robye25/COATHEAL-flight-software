#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coatheal {

// Driver-agnostic stepper interface. Concrete drivers (A4988, DRV8825,
// TMC2209, …) all expose a STEP/DIR/EN pin triplet plus an optional microstep
// selector, so the controller only needs: pulse a step, flip direction, gate
// enable, and be told the microstep ratio. Real pulse timing is backend-
// specific; the simulated driver just counts pulses.
class StepperDriver {
 public:
  virtual ~StepperDriver() = default;

  // Enable/disable the driver's power stage (holding torque off when false).
  virtual bool Enable(bool enable) = 0;

  // Issue one step pulse in the given direction. Returns false on hardware
  // fault.
  virtual bool Step(bool direction_forward) = 0;

  // Microstep divisor (1 = full step, 2 = half, … 32). Ignored by drivers
  // without software-selectable microstepping.
  virtual void SetMicrostep(int divisor) = 0;

  virtual bool healthy() const = 0;

  // Monotonically-counted pulses issued since construction (useful for tests
  // and for sanity cross-checks against the controller's position).
  virtual std::uint64_t pulses_issued() const = 0;
};

// No-op implementation used on bench/CI. Tracks pulses and state in memory.
class SimulatedStepperDriver : public StepperDriver {
 public:
  SimulatedStepperDriver();

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return true; }
  std::uint64_t pulses_issued() const override { return pulses_; }

  bool enabled() const { return enabled_; }
  int microstep() const { return microstep_; }
  bool last_direction_forward() const { return last_dir_; }

 private:
  bool enabled_ = false;
  bool last_dir_ = true;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
};

// Step/DIR/EN backend. Common to almost every hobby/industrial stepper driver
// (A4988, DRV8825, TMC2208/2209 in legacy mode, …). The actual GPIO writes
// are left as a build-time stub until the driver choice is finalized; see
// LibgpiodPwmController for the same pattern. Real pulse generation will
// need a dedicated timer/thread once the driver is known.
class GpioStepDirStepperDriver : public StepperDriver {
 public:
  GpioStepDirStepperDriver(std::string chip,
                           std::size_t step_line,
                           std::size_t dir_line,
                           std::size_t enable_line,
                           bool invert_direction,
                           bool enable_active_low);

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  std::uint64_t pulses_issued() const override { return pulses_; }

 private:
  std::string chip_;
  std::size_t step_line_;
  std::size_t dir_line_;
  std::size_t enable_line_;
  bool invert_direction_;
  bool enable_active_low_;
  bool healthy_ = false;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
};

}  // namespace coatheal
