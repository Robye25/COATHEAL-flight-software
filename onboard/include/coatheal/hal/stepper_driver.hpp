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

  // Runtime run-current change (STEPPER_SET_CURRENT). Backends with a real
  // current DAC validate against their sense-resistor limits, apply the new
  // value to the chip, and keep it across reconfigurations; backends with
  // no power stage just record it. False = not applied (with an
  // operator-readable reason in *error).
  virtual bool SetRunCurrent(double a_rms, std::string* error) {
    (void)a_rms;
    (void)error;
    return true;
  }
  // The run current the backend is configured for, in A RMS. 0 when the
  // backend has no notion of current.
  virtual double run_current_a_rms() const { return 0.0; }

  // Driver die thermal state, best-available over the backend's bus. The
  // TMC5160 has no numeric temperature ADC; it reports two DRV_STATUS
  // threshold flags, which map to: 0 = nominal, 1 = over-temperature
  // pre-warning (die >= ~120 °C, otpw), 2 = over-temperature shutdown
  // seen (die >= ~150 °C, ot) — 2 is LATCHED by the backend until the
  // next Enable(true) so the channel safety that acts on it cannot race a
  // self-clearing chip flag. Backends with no thermal telemetry report 0.
  virtual int thermal_state() const { return 0; }

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
  // A fault that does not stop the motor but the operator must know about
  // (today: the enable line has no effect on the chip, so STEPPER_DISABLE
  // cannot de-energise the power stage through EN). Empty when all clear.
  virtual std::string warning() const { return {}; }
  // Live read of the chip's motion-truth registers for MOTOR_DEBUG, as a
  // `key=value;` string (empty when the backend has no chip to ask or the
  // bus failed). The software position counters can advance while a motor
  // never turns (bench, 2026-08-26); these registers cannot lie about it.
  virtual std::string DebugRegisters() { return {}; }
  // Called about once a second while the motor is enabled and idle: a
  // chance to notice that the chip lost its configuration (TMC5160 reset
  // on a supply dip) while nothing was stepping. Default: nothing to do.
  virtual bool Poll() { return true; }
  virtual std::uint64_t pulses_issued() const = 0;

  // --- Step-loss supervision ---------------------------------------------
  // The motors run open loop: the position is a count of the steps that
  // were commanded. A step-loss event is anything that makes that count
  // untrustworthy as a statement about the rotor -- the chip lost its
  // configuration, its power stage was off under it, it reported a stall,
  // the chip is not where it was told to go. Backends count events since
  // boot; the channel latches "position uncertain" on every new one.
  // Backends with nothing to supervise report none.
  virtual std::uint64_t step_loss_events() const { return 0; }
  // The last event in operator-readable terms, empty when there was none.
  virtual std::string step_loss_reason() const { return {}; }
  // True once after an event that asks the channel to stop the move in
  // progress and keep the motor energised (a stall with stall detection
  // set to stop: pushing on loses every further step). Reading clears it.
  virtual bool TakeStepLossStop() { return false; }
  // Called by the channel when a leg has reached its target: the last
  // position command of a move has no later one to supersede it, so this is
  // where a backend proves the chip holds the target it was given. False
  // when it does not and could not be made to.
  virtual bool ConfirmTarget() { return true; }
  // How fast the channel is stepping (full-steps/s) and whether that rate
  // is steady (cruise, not a ramp); 0 while no move is in progress.
  // Load-sensing backends trust their stall measurement only at a steady
  // rate above their minimum.
  virtual void NoteStepRate(double full_step_hz, bool steady) {
    (void)full_step_hz;
    (void)steady;
  }
};

class SimulatedStepperDriver : public StepperDriver {
 public:
  SimulatedStepperDriver();

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return true; }
  std::uint64_t pulses_issued() const override { return pulses_; }
  bool SetRunCurrent(double a_rms, std::string* error) override;
  double run_current_a_rms() const override { return run_current_a_rms_; }

  bool enabled() const { return enabled_; }
  int microstep() const { return microstep_; }
  bool last_direction_forward() const { return last_dir_; }

 private:
  bool enabled_ = false;
  bool last_dir_ = true;
  int microstep_ = 1;
  double run_current_a_rms_ = 0.0;
  std::uint64_t pulses_ = 0;
};

}  // namespace coatheal
