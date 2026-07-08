#pragma once

#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"

namespace coatheal {

// Final BOM sensor facade. The configuration names DAQ132M/PT100,
// DPS310/pressure, and ADS1115/GUVA sources. The current implementation still
// supplies simulated values until the device-specific I2C/Modbus reads are
// bench-validated.
class SensorManager {
 public:
  static constexpr std::size_t kSampleCount = 8;

  SensorManager(const OnboardConfig& config,
                SpiAdapter* spi,
                I2cAdapter* i2c,
                RtcAdapter* rtc,
                Ina3221Adapter* ina = nullptr);

  SensorSnapshot ReadSnapshot(MissionPhase phase,
                              const std::vector<double>& heater_duty,
                              double dt_seconds);

  bool t_ambient_ok() const { return t_ambient_ok_; }
  bool p_ambient_ok() const { return p_ambient_ok_; }
  bool resistance_ok() const { return resistance_ok_; }

  void NotePullCompleted(int motor_id);

 private:
  OnboardConfig config_;
  SpiAdapter* spi_ = nullptr;
  I2cAdapter* i2c_ = nullptr;
  RtcAdapter* rtc_ = nullptr;
  Ina3221Adapter* ina_ = nullptr;
  std::vector<double> sample_temps_c_;
  std::vector<double> sample_resistance_ohm_;
  double pressure_mbar_ = 1013.25;
  bool pressure_descending_ = true;
  bool t_ambient_ok_ = true;
  bool p_ambient_ok_ = true;
  bool resistance_ok_ = true;
};

}  // namespace coatheal
