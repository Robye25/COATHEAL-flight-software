#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

// TMC2240 setup. Motion uses STEP/DIR/EN; SPI configures and verifies the
// driver before EN may become active.
struct Tmc2240Config {
  std::string gpio_chip = "/dev/gpiochip0";
  std::string spi_device = "/dev/spidev0.0";
  std::size_t cs_line = 22;
  std::size_t step_line = 19;
  std::size_t dir_line = 26;
  std::size_t enable_line = 12;
  bool invert_direction = false;
  bool enable_active_low = true;

  double run_current_a_rms = 0.8;
  double current_range_a_peak = 0.0;  // 0 selects the lowest fitting range.
  double hold_current_frac = 0.30;
  int microstep = 4;
  bool stealth_chop = true;
  std::uint32_t spi_speed_hz = 1000000;
  int pulse_high_us = 3;
};

struct Tmc2240CurrentSettings {
  std::uint8_t range_code = 0;
  double range_a_peak = 0.0;
  std::uint8_t global_scaler = 0;
  std::uint32_t ihold_irun = 0;
};

class Tmc2240Driver : public StepperDriver {
 public:
  explicit Tmc2240Driver(const Tmc2240Config& cfg);
  ~Tmc2240Driver() override;

  Tmc2240Driver(const Tmc2240Driver&) = delete;
  Tmc2240Driver& operator=(const Tmc2240Driver&) = delete;
  Tmc2240Driver(Tmc2240Driver&&) = delete;
  Tmc2240Driver& operator=(Tmc2240Driver&&) = delete;

  bool Enable(bool enable) override;
  bool Step(bool direction_forward) override;
  void SetMicrostep(int divisor) override;
  bool healthy() const override { return healthy_; }
  bool ActiveCheck() override { return Reinitialize(); }
  std::uint64_t pulses_issued() const override { return pulses_; }

  bool Reinitialize();

  const Tmc2240Config& config() const { return cfg_; }

  static bool CalculateCurrentSettings(
      double run_a_rms, double hold_frac, double requested_range_a_peak,
      Tmc2240CurrentSettings* settings);
  static std::uint32_t EncodeChopconf(int microstep_divisor);
  static std::uint32_t EncodeGconf(bool stealth_chop);

 private:
  bool OpenSpi();
  bool OpenGpio();
  void CloseSpi();
  void CloseGpio();
  bool WriteRegister(std::uint8_t address, std::uint32_t value);
  bool ReadRegister(std::uint8_t address, std::uint32_t* value);
  bool Transfer(const std::uint8_t tx[5], std::uint8_t rx[5]);
  bool EnableUnlocked(bool enable);

  Tmc2240Config cfg_;
  int spi_fd_ = -1;
  bool healthy_ = false;
  bool gpio_healthy_ = false;
  bool enabled_ = false;
  bool last_direction_forward_ = false;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
  void* cs_handle_ = nullptr;
  void* step_handle_ = nullptr;
  void* dir_handle_ = nullptr;
  void* enable_handle_ = nullptr;
  mutable std::mutex io_mu_;
};

}  // namespace coatheal
