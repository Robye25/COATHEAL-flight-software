#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace coatheal {

SensorManager::SensorManager(const OnboardConfig& config,
                             SpiAdapter* spi,
                             I2cAdapter* i2c,
                             RtcAdapter* rtc)
    : config_(config),
      spi_(spi),
      i2c_(i2c),
      rtc_(rtc),
      sample_temps_c_(config.hardware.heater_count, config.phase.ascent_target_c) {}

SensorSnapshot SensorManager::ReadSnapshot(MissionPhase phase,
                                           const std::vector<double>& heater_duty,
                                           double dt_seconds) {
  const double dt = dt_seconds <= 1e-6 ? 1.0 : dt_seconds;

  // Coarse pressure profile model for simulation and bench testing.
  // Floor is 5 mbar per BEXUS User Manual §5.6 — the experiment acceptance
  // pressure level — so vacuum-regime tests exercise the real flight envelope.
  if (phase == MissionPhase::kDescentFloor) {
    pressure_descending_ = false;
  }
  pressure_mbar_ += (pressure_descending_ ? -1.5 : 1.8) * dt;
  pressure_mbar_ = std::clamp(pressure_mbar_, 5.0, 1013.25);

  double ambient_temp = -40.0;
  if (phase == MissionPhase::kActivationRamp || phase == MissionPhase::kFloatHold) {
    ambient_temp = -55.0;
  } else if (phase == MissionPhase::kDescentFloor) {
    ambient_temp = -15.0;
  }

  for (std::size_t i = 0; i < sample_temps_c_.size(); ++i) {
    const double duty = (i < heater_duty.size()) ? heater_duty[i] : 0.0;
    const double heating = duty * 5.0 * dt;
    const double cooling = (sample_temps_c_[i] - ambient_temp) * 0.03 * dt;
    sample_temps_c_[i] += heating - cooling;
  }

  const double box_duty = (config_.hardware.electronics_heater_index < heater_duty.size())
                              ? heater_duty[config_.hardware.electronics_heater_index]
                              : 0.0;
  const double box_heating = box_duty * 2.5 * dt;
  const double box_cooling = (box_temp_c_ - ambient_temp) * 0.02 * dt;
  box_temp_c_ += box_heating - box_cooling;

  SensorSnapshot snapshot;
  snapshot.rtc_valid = rtc_ != nullptr ? rtc_->valid() : false;
  snapshot.timestamp_utc = rtc_ != nullptr ? rtc_->NowUtcIso8601() : "1970-01-01T00:00:00Z";
  snapshot.ambient_temp_c = ambient_temp;
  snapshot.ambient_pressure_mbar = pressure_mbar_;
  snapshot.ambient_humidity_pct = 15.0 + (std::sin(pressure_mbar_ / 90.0) * 5.0);
  snapshot.uv = phase == MissionPhase::kFloatHold ? 1.8 : 0.4;
  snapshot.box_temp_c = box_temp_c_;
  snapshot.sample_temps_c = sample_temps_c_;

  // Range checks: do not mutate raw readings; only flip status bits so the
  // ground sees an honest out-of-range sample.
  t_ambient_ok_ = (snapshot.ambient_temp_c >= config_.sensor_range.ambient_temp_min_c) &&
                  (snapshot.ambient_temp_c <= config_.sensor_range.ambient_temp_max_c);
  p_ambient_ok_ = (snapshot.ambient_pressure_mbar >= config_.sensor_range.ambient_pressure_min_mbar) &&
                  (snapshot.ambient_pressure_mbar <= config_.sensor_range.ambient_pressure_max_mbar);

  if (spi_ != nullptr && i2c_ != nullptr) {
    if (!spi_->healthy() || !i2c_->healthy()) {
      // Keep data running but avoid NaNs in case hardware probing fails.
      snapshot.uv = 0.0;
    }
  }

  return snapshot;
}

}  // namespace coatheal