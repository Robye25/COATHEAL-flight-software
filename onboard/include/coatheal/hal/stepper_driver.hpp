#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coatheal {

// Driver-agnostic stepper interface. The final TMC5160 path exposes
// STEP/DIR/EN plus SPI configuration, but the controller only needs this small
// motion surface. Real pulse timing is backend-specific; the simulated driver
// just counts pulses.
class StepperDriver {
 public:
  virtual ~StepperDriver() = default;

  virtual bool Enable(bool enable) = 0;
  virtual bool Step(bool direction_forward) = 0;
  virtual void SetMicrostep(int divisor) = 0;
  virtual bool healthy() const = 0;
  virtual bool ActiveCheck() { return healthy(); }
  virtual std::uint64_t pulses_issued() const = 0;
};

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

// STEP/DIR/EN backend used by the TMC5160 fallback path. Pulses are emitted
// through libgpiod and still require waveform validation on the target Pi.
class GpioStepDirStepperDriver : public StepperDriver {
 public:
  GpioStepDirStepperDriver(std::string chip,
                           std::size_t step_line,
                           std::size_t dir_line,
                           std::size_t enable_line,
                           bool invert_direction,
                           bool enable_active_low);
  ~GpioStepDirStepperDriver() override;

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
  bool enabled_ = false;
  bool last_direction_forward_ = true;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
  void* step_handle_ = nullptr;
  void* dir_handle_ = nullptr;
  void* enable_handle_ = nullptr;
};

}  // namespace coatheal
