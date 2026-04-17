#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

// Configuration for the TMC2240 SPI register programming. Defaults target the
// OMC 17E19S2504BSM5-150RS integrated ball-screw NEMA-17 (2.5 A/phase
// nameplate) driven at up to 100 full-step/s (~30 rpm). The TMC2240 is rated
// up to 2.1 A RMS (~3 A peak) so we derate IRUN to 2.0 A RMS for margin.
struct Tmc2240Config {
  std::string spi_device = "/dev/spidev1.0";  // SPI1 on the Pi 4
  std::size_t cs_line = 0;                    // chip-select line (info only)
  std::size_t step_line = 5;
  std::size_t dir_line = 6;
  std::size_t enable_line = 13;
  bool invert_direction = false;
  bool enable_active_low = true;

  double run_current_a_rms = 2.0;   // OMC 17E19 nameplate 2.5A; derated.
  double hold_current_frac = 0.30;  // IHOLD = 30% of IRUN on IC scale.
  int microstep = 4;                // default per REV-B.1 motion spec.
  bool stealth_chop = true;         // quiet, low-torque-ripple mode.
  std::uint32_t spi_speed_hz = 1000000;  // 1 MHz (TMC2240 §5.3 max).
};

// TMC2240 driver. Programs the IC over SPI at construction time, then operates
// as a standard STEP/DIR/EN stepper driver (the IC's internal motion engine is
// not used — we pulse STEP ourselves so the microstep schedule is identical to
// the A4988 path and testable in simulation).
//
// Register values written on Reinitialize() — see TMC2240 datasheet rev.1.03:
//   GCONF      (0x00)   bit 2 = en_pwm_mode -> stealthChop.
//   IHOLD_IRUN (0x10)   (IHOLDDELAY<<16) | (IRUN<<8) | IHOLD
//                       IRUN scaled 0..31 over ~2.1 A RMS full-scale
//                       (0.075 Ω external sense, QHV2240-style boards).
//   TPOWERDOWN (0x11)   0x0A (~170 ms before hold-current decay).
//   CHOPCONF   (0x6C)   MRES[27:24] native microsteps (power-of-two),
//                       TOFF=3, TBL=2, HSTRT=4, HEND=1.
//   PWMCONF    (0x70)   0xC10D0024 (stealthChop autoscale, PWM_REG=4).
//
// 5-byte SPI datagram:
//   [ addr | 0x80 ] [ data[31:24] ] [ data[23:16] ] [ data[15:8] ] [ data[7:0] ]
// Clocked at up to 1 MHz in SPI mode 3 (CPOL=1, CPHA=1), 8 bits/word.
//
// If the SPI device cannot be opened or any ioctl fails, the driver reports
// healthy()=false and returns false from all hardware ops. It does NOT throw.
// SystemController may fall back to a plain GPIO STEP/DIR/EN driver so the
// tick loop still runs for diagnostics.
class Tmc2240Driver : public StepperDriver {
 public:
  explicit Tmc2240Driver(const Tmc2240Config& cfg);
  ~Tmc2240Driver() override;

  // Non-copyable, non-movable (owns an SPI file descriptor).
  Tmc2240Driver(const Tmc2240Driver&) = delete;
  Tmc2240Driver& operator=(const Tmc2240Driver&) = delete;
  Tmc2240Driver(Tmc2240Driver&&) = delete;
  Tmc2240Driver& operator=(Tmc2240Driver&&) = delete;

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  std::uint64_t pulses_issued() const override { return pulses_; }

  // Re-run the register programming (ctor calls this). Useful after a brown-out.
  bool Reinitialize();

  const Tmc2240Config& config() const { return cfg_; }

  // Unit-test hooks. Pure functions; independent of SPI access so the host
  // bench build can verify the encoded register values.
  static std::uint32_t EncodeIholdIrun(double run_a_rms, double hold_frac);
  static std::uint32_t EncodeChopconf(int microstep_divisor);
  static std::uint32_t EncodeGconf(bool stealth_chop);

 private:
  bool OpenSpi();
  void CloseSpi();
  bool WriteRegister(std::uint8_t address, std::uint32_t value);

  Tmc2240Config cfg_;
  int spi_fd_ = -1;
  bool healthy_ = false;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
};

}  // namespace coatheal
