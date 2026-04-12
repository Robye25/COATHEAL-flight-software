#pragma once

#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"

namespace coatheal {

class SensorManager {
 public:
  SensorManager(const OnboardConfig& config,
                SpiAdapter* spi,
                I2cAdapter* i2c,
                RtcAdapter* rtc);

  SensorSnapshot ReadSnapshot(MissionPhase phase,
                              const std::vector<double>& heater_duty,
                              double dt_seconds);

  // Status flags for ambient temperature/pressure range checks. Updated on
  // every ReadSnapshot call. Raw sensor values are not clamped.
  bool t_ambient_ok() const { return t_ambient_ok_; }
  bool p_ambient_ok() const { return p_ambient_ok_; }

 private:
  OnboardConfig config_;
  SpiAdapter* spi_ = nullptr;
  I2cAdapter* i2c_ = nullptr;
  RtcAdapter* rtc_ = nullptr;
  std::vector<double> sample_temps_c_;
  double box_temp_c_ = 10.0;
  double pressure_mbar_ = 1013.25;
  bool pressure_descending_ = true;
  bool t_ambient_ok_ = true;
  bool p_ambient_ok_ = true;
};

}  // namespace coatheal