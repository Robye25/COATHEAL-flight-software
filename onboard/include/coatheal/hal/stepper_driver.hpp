#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coatheal {

// Driver-agnostic stepper interface. The final SPI-motion path (schematic
// v3, no STEP/DIR lines) exposes CS/EN plus SPI configuration, but the
// controller only needs this small motion surface. Real pulse timing is
// backend-specific; the simulated driver just counts pulses.
class StepperDriver {
 public:
  virtual ~StepperDriver() = default;

  virtual bool Enable(bool enable) = 0;
  virtual bool Step(bool direction_forward) = 0;
  virtual void SetMicrostep(int divisor) = 0;
  virtual bool healthy() const = 0;
  virtual bool ActiveCheck() { return healthy(); }

  // Transport health, deliberately separate from healthy().
  //
  // healthy() answers "can this motor be driven" -- a module strapped for
  // STEP/DIR, or a wrong chip version, fails that while its SPI
  // conversations succeed perfectly. This answers the narrower "did the
  // last bus conversation get through", so the SPI_OK telemetry flag can
  // describe the bus rather than the motor on it. Backends with no bus of
  // their own have nothing to break and report true.
  virtual bool spi_bus_ok() const { return true; }

  // Why this driver last refused, in operator-readable terms, or empty if
  // it has nothing to say. Bring-up diagnoses (wrong chip version, module
  // strapped for STEP/DIR, enable line not reaching the chip) otherwise
  // reach only the journal, leaving the ground station with a bare
  // "enable failed" for a fault whose cause is already known.
  virtual std::string last_error() const { return {}; }
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

}  // namespace coatheal
