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

// Rev B.1: SensorManager reads the 8 PT100 sample temperatures via the two
// 4-channel Modbus RTD collectors, ambient pressure via MS5803-01BA (no
// humidity — BME280 is gone), and per-sample resistance via the two INA3221
// chips on I2C at 0x40 / 0x41. The INA3221 adapter is stubbed; if it is
// nullptr or unhealthy the `sample_resistance_ohm` vector is zeroed and the
// caller can flip `resistance_ok`.
//
// Sample count (sample_temps_c.size()) is `hardware.heater_count + 2` in
// practice: 6 heated samples plus 2 pulled-but-unheated samples (indices
// 6 and 7). Size is driven entirely by the number of PT100 channels — 8.
class SensorManager {
 public:
  static constexpr std::size_t kSampleCount = 8;
  // Rev B.1: only the first 6 samples have INA3221 channels wired (two chips
  // × 3 channels each). Samples 6 and 7 are pulled but electrically
  // unmeasured — their resistance serializes as the "-" placeholder.
  static constexpr std::size_t kResistanceChannelCount = 6;

  SensorManager(const OnboardConfig& config,
                SpiAdapter* spi,
                I2cAdapter* i2c,
                RtcAdapter* rtc,
                Ina3221Adapter* ina = nullptr);

  SensorSnapshot ReadSnapshot(MissionPhase phase,
                              const std::vector<double>& heater_duty,
                              double dt_seconds);

  // Status flags for ambient temperature/pressure range checks. Updated on
  // every ReadSnapshot call. Raw sensor values are not clamped.
  bool t_ambient_ok() const { return t_ambient_ok_; }
  bool p_ambient_ok() const { return p_ambient_ok_; }
  // True iff the INA3221 instrument is present and healthy. Drives the
  // RESISTANCE_OK status bit.
  bool resistance_ok() const { return resistance_ok_; }

  // Test/bench hook: inform the simulator a pull happened on `motor_id` so
  // the synthetic resistance decay can step. Harmless when called with an
  // unknown id. Real hardware path is a no-op.
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
