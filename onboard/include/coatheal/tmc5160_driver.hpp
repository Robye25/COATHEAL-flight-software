#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

// FYSETC TMC5160 setup. Motion remains STEP/DIR/EN; SPI is used to program
// current, microstep, and chopper/stealthChop registers at startup.
struct Tmc5160Config {
  std::string gpio_chip = "/dev/gpiochip0";
  std::string spi_device = "/dev/spidev0.0";
  std::size_t cs_line = 22;
  std::size_t step_line = 19;
  std::size_t dir_line = 26;
  std::size_t enable_line = 12;
  bool invert_direction = false;
  bool enable_active_low = true;

  double run_current_a_rms = 2.0;
  double hold_current_frac = 0.30;
  int microstep = 4;
  bool stealth_chop = true;
  std::uint32_t spi_speed_hz = 1000000;
};

class Tmc5160Driver : public StepperDriver {
 public:
  explicit Tmc5160Driver(const Tmc5160Config& cfg);
  ~Tmc5160Driver() override;

  Tmc5160Driver(const Tmc5160Driver&) = delete;
  Tmc5160Driver& operator=(const Tmc5160Driver&) = delete;
  Tmc5160Driver(Tmc5160Driver&&) = delete;
  Tmc5160Driver& operator=(Tmc5160Driver&&) = delete;

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  bool ActiveCheck() override { return Reinitialize(); }
  std::uint64_t pulses_issued() const override { return pulses_; }

  bool Reinitialize();

  const Tmc5160Config& config() const { return cfg_; }

  static std::uint32_t EncodeIholdIrun(double run_a_rms, double hold_frac);
  static std::uint32_t EncodeChopconf(int microstep_divisor);
  static std::uint32_t EncodeGconf(bool stealth_chop);

 private:
  bool OpenSpi();
  bool OpenGpio();
  void CloseSpi();
  void CloseGpio();
  bool WriteRegister(std::uint8_t address, std::uint32_t value);

  Tmc5160Config cfg_;
  int spi_fd_ = -1;
  bool healthy_ = false;
  bool gpio_healthy_ = false;
  bool enabled_ = false;
  bool last_direction_forward_ = true;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
  void* gpio_chip_handle_ = nullptr;
  void* cs_handle_ = nullptr;
  void* step_handle_ = nullptr;
  void* dir_handle_ = nullptr;
  void* enable_handle_ = nullptr;
};

}  // namespace coatheal
