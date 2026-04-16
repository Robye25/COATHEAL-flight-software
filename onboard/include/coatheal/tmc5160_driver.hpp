#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

// Configuration for the TMC5160 SPI register programming. Defaults target a
// Pololu 2851 NEMA-17 high-torque motor pulled at up to 100 full-step/s
// (≈30 rpm), which is REV-B motor 0.
struct Tmc5160Config {
  std::string spi_device = "/dev/spidev1.0";  // SPI1 on the Pi 4
  std::size_t cs_line = 0;                    // chip-select line (info only)
  std::size_t step_line = 5;
  std::size_t dir_line = 6;
  std::size_t enable_line = 13;
  bool invert_direction = false;
  bool enable_active_low = true;

  double run_current_a_rms = 1.5;   // Pololu 2851 nominal (IRUN scale).
  double hold_current_frac = 0.30;  // IHOLD = 30% of IRUN on IC scale.
  int microstep = 4;                // default per REV-B motion spec.
  bool stealth_chop = true;         // quiet, low-torque-ripple mode.
};

// TMC5160 driver. Programs the IC over SPI at construction time, then operates
// as a standard STEP/DIR/EN stepper driver (the IC's internal motion engine is
// not used — we pulse STEP ourselves so the microstep schedule is identical to
// the A4988 path and testable in simulation).
//
// Register values written on Init() — see TMC5160 datasheet rev. 1.17 table
// 5.1 (general config) and §7 (current control):
//   GCONF      (0x00) = 0x00000004
//       bit 2 = en_pwm_mode  -> stealthChop (set when stealth_chop=true)
//   CHOPCONF   (0x6C) = 0x000100C3  (TOFF=3, TBL=2, CHM=0, HSTRT/HEND safe)
//                                    MRES field (bits 24-27) per microstep:
//                                      4×  -> 0x7 (8 microsteps per step unused,
//                                                  IC internal)
//                                      5×  -> 0x6 (10 mdegr — see note below)
//                                    Note: TMC5160 MRES is 2^n; we use the
//                                    closest power-of-2 tier and document the
//                                    effective divisor in the runtime log.
//   IHOLD_IRUN (0x10) = (IHOLDDELAY=6 << 16) | (IRUN=<scaled 0..31> << 8)
//                                   | IHOLD=<scaled 0..31>
//                      where IRUN = round(run_current_a_rms / 2.2 A * 31)
//                            IHOLD = round(IRUN * hold_current_frac)
//   TPOWERDOWN (0x11) = 0x0000000A   (~170 ms before entering hold current)
//   PWMCONF    (0x70) = 0xC10D0024   (stealthChop autoscale, PWM_REG=4)
//
// Writes are 40-bit (1 address byte + 4 data bytes). The register programmer
// is implemented in the .cpp; if the SPI bus is unavailable (bench builds) the
// driver still constructs but reports healthy()=false, matching the pattern
// used by GpioStepDirStepperDriver.
class Tmc5160Driver : public StepperDriver {
 public:
  explicit Tmc5160Driver(const Tmc5160Config& cfg);

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  std::uint64_t pulses_issued() const override { return pulses_; }

  // Low-level: re-program the IC registers (also called by the ctor). Exposed
  // so operators can re-assert safe register state after a brown-out.
  bool Reinitialize();

  const Tmc5160Config& config() const { return cfg_; }

 private:
  bool WriteRegister(std::uint8_t address, std::uint32_t value);
  static std::uint32_t EncodeIholdIrun(double run_a_rms, double hold_frac);
  static std::uint32_t EncodeChopconf(int microstep_divisor);
  static std::uint32_t EncodeGconf(bool stealth_chop);

  Tmc5160Config cfg_;
  bool healthy_ = false;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
};

}  // namespace coatheal
