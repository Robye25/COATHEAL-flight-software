#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "coatheal/hal/gpio_output.hpp"
#include "coatheal/hal/spi_bus.hpp"
#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

// TMC5160 setup (schematic v3). STEP/DIR pins are physically unconnected on
// the QHV5160 v2 board -- motion happens *only* over SPI via the chip's
// internal ramp generator. Every StepperDriver::Step() call nudges XTARGET
// by one configured microstep ("position dribble"); the ramp generator turns
// that into the actual coil waveform. The upper stack (pacing thread,
// position tracking, pull cycles, MotionLock, heater inhibit) is unchanged
// by this driver -- that is the entire point of the design.
struct Tmc5160Config {
  std::string spi_device = "/dev/spidev0.0";
  std::uint32_t spi_speed_hz = 1000000;
  std::string gpio_chip = "/dev/gpiochip0";
  std::size_t cs_line = 22;
  std::size_t enable_line = 20;
  bool invert_direction = false;
  bool enable_active_low = true;
  double run_current_a_rms = 0.8;
  double hold_current_frac = 0.30;
  double sense_resistor_ohm = 0.075;
  // StealthChop (GCONF bit 2, en_pwm_mode): quiet, low-vibration chopper at
  // low speed, at the cost of torque headroom. True mirrors MotorConfig's
  // default. Wired through to the GCONF write and covered by the existing
  // GCONF readback verify.
  bool stealth_chop = true;
  int microstep = 4;
  // Carried through from MotorConfig for parity with the retired TMC2240
  // path; the actual re-probe pacing lives one layer up in
  // StepperChannel::Tick (driver_retry_ms/last_driver_retry_), exactly as it
  // did for the TMC2240 driver -- this field is not consumed internally
  // here.
  int retry_ms = 2000;
};

class Tmc5160Driver : public StepperDriver {
 public:
  // `bus` is not owned: the caller (production: the factory building one
  // LinuxSpiBus per motor; tests: a FakeSpiBus) keeps it alive for the
  // driver's lifetime and is responsible for its storage. The driver does
  // call bus->Open()/Close() as a bookend to its own use of the bus.
  //
  // `use_gpio` selects production vs. test mode. GPIO is a pair of free
  // functions (RequestGpioOutput/SetGpioOutput/ReleaseGpioOutput), not an
  // injectable class, and there is no fake GPIO backend in this codebase, so
  // "nullptr cs/en = test mode" from the plan is realized here as an
  // explicit flag rather than nullable handles: `use_gpio=true` requests
  // real CS/EN lines from `cfg.gpio_chip` at `cfg.cs_line`/`cfg.enable_line`
  // (RequestGpioOutput returning null, e.g. on this Windows dev host, is
  // handled as unhealthy). `use_gpio=false` skips every GPIO call -- CS
  // framing and EN toggling are both no-ops -- so unit tests can exercise
  // the full SPI conversation via FakeSpiBus with no libgpiod dependency.
  Tmc5160Driver(Tmc5160Config cfg, SpiBus* bus, bool use_gpio);
  ~Tmc5160Driver() override;

  Tmc5160Driver(const Tmc5160Driver&) = delete;
  Tmc5160Driver& operator=(const Tmc5160Driver&) = delete;

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  bool ActiveCheck() override { return Reinitialize(); }
  std::uint64_t pulses_issued() const override { return pulses_; }
  // True once a datagram has completed end to end, false the moment
  // one cannot be conducted (device not open, CS line dead, ioctl
  // failed). Independent of the version/strap gates, which reject a
  // module the bus reached just fine.
  bool spi_bus_ok() const override { return spi_bus_ok_; }
  std::string last_error() const override { return last_error_message_; }
  std::string warning() const override;

  // Whether driving the EN GPIO was last seen to move DRV_ENN in IOIN.
  // Enable(true) already refuses a line that leaves DRV_ENN HIGH; the
  // opposite failure -- DRV_ENN stuck LOW, i.e. a module whose enable pin
  // is not routed to the chip -- lets the motor run but makes
  // STEPPER_DISABLE unable to cut the power stage through EN. Enable(false)
  // detects it and reports it here and through warning().
  bool enable_line_effective() const { return enable_line_effective_; }
  // Test hook: the IOIN verification after driving EN normally runs only
  // when this driver owns the GPIO (use_gpio=true); FakeSpiBus tests run
  // with use_gpio=false and turn it on explicitly.
  void set_verify_enable_line(bool verify) { verify_enable_line_ = verify; }

  // Full probe + register (re)configuration sequence: IOIN version gate,
  // GCONF/CHOPCONF/current/ramp register writes, XACTUAL=XTARGET=0, then a
  // GCONF+CHOPCONF readback verify. Public so ActiveCheck() (and tests) can
  // invoke it directly, matching the retired TMC2240 driver's shape.
  bool Reinitialize();

  const Tmc5160Config& config() const { return cfg_; }
  std::int32_t target() const { return target_; }
  bool enabled() const { return enabled_; }
  int microstep() const { return microstep_; }

  // TMC5160 MRES (CHOPCONF bits 27:24): selects how many of the ramp
  // generator's fixed 256 internal microsteps/fullstep constitute one
  // configured microstep. 256->0, 128->1, 64->2, 32->3, 16->4, 8->5, 4->6,
  // 2->7, 1->8. Any other divisor is invalid and returns kInvalidMres.
  static std::uint8_t EncodeMres(int divisor);

  // The ramp generator's XTARGET always counts in units of its fixed 256
  // microsteps/fullstep internal resolution, regardless of MRES. One
  // Step() call must therefore move XTARGET by 256/microstep_divisor to
  // advance exactly one configured microstep. Returns 0 for an
  // unsupported divisor.
  static std::uint32_t DeltaXtarget(int divisor);

  // IOIN (0x04) pin-state decoders. The TMC5160 mirrors the physical
  // state of its mode/enable pins in the same register the version gate
  // already reads, so validating them costs no extra bus traffic. Shared
  // by the health gates and the unit tests so one definition of these bit
  // positions exists.
  //
  // SD_MODE (bit 6) selects the chip's motion source: 0 = internal ramp
  // generator driven over SPI (what this driver steers via
  // RAMPMODE/XTARGET), 1 = external STEP/DIR pins. DRV_ENN (bit 4) is the
  // enable input, active LOW -- a HIGH readback means the power stage is
  // disabled.
  static bool IoinStepDirMode(std::uint32_t ioin);
  static bool IoinDriverDisabled(std::uint32_t ioin);

  // Derives GLOBALSCALER (32..256), IRUN (0..31) and IHOLD (0..31) from a
  // desired RMS run current, the sense resistor, and the hold-current
  // fraction. See tmc5160_driver.cpp for the derivation and the two-regime
  // rationale (kept in the .cpp so the formula's comment sits next to the
  // constants it explains). Returns false for invalid inputs or a current
  // request the sense resistor cannot deliver even at maximum scale.
  static bool CalculateCurrent(double a_rms, double sense_ohm,
                               double hold_frac, std::uint32_t* globalscaler,
                               std::uint8_t* irun, std::uint8_t* ihold);

  static constexpr std::uint8_t kInvalidMres = 0xFF;

 private:
  bool OpenGpio();
  void CloseGpio();
  bool OpenSpi();
  bool WriteRegister(std::uint8_t address, std::uint32_t value);
  bool ReadRegister(std::uint8_t address, std::uint32_t* value);
  bool Transfer(const std::uint8_t tx[5], std::uint8_t rx[5]);
  bool EnableUnlocked(bool enable);
  bool ReinitializeUnlocked();
  std::uint32_t EncodeChopconf(std::uint8_t toff) const;

  // Bring-up failures are re-probed every driver_retry_ms (2 s by default)
  // for as long as they persist, and a strap/wiring fault persists until
  // someone opens the box. Emitting the diagnosis on every retry buries
  // the rest of the journal, so it is latched: printed when the message
  // changes, and re-armed once the driver comes back healthy.
  void ReportError(const std::string& message);

  Tmc5160Config cfg_;
  SpiBus* bus_;
  bool use_gpio_;
  GpioOutput* cs_handle_ = nullptr;
  GpioOutput* enable_handle_ = nullptr;
  bool gpio_healthy_ = false;
  bool spi_open_ = false;
  bool healthy_ = false;
  bool enabled_ = false;
  int microstep_ = 1;
  std::int32_t target_ = 0;
  std::uint64_t pulses_ = 0;
  std::string last_error_message_;
  bool spi_bus_ok_ = false;
  bool verify_enable_line_ = false;
  bool enable_line_effective_ = true;
  bool enable_warning_logged_ = false;
  mutable std::mutex io_mu_;
};

}  // namespace coatheal
