#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace coatheal {

namespace {
constexpr double kInitialResistanceOhm = 100.0;
constexpr double kResistanceDecayPerPull = 0.05;
}  // namespace

SensorManager::SensorManager(const OnboardConfig& config,
                             SpiAdapter* spi,
                             I2cAdapter* i2c,
                             RtcAdapter* rtc,
                             Ina3221Adapter* ina)
    : config_(config),
      spi_(spi),
      i2c_(i2c),
      rtc_(rtc),
      ina_(ina),
      sample_temps_c_(config.hardware.sample_count, config.phase.sample_floor_c),
      sample_resistance_ohm_(config.hardware.sample_count, kInitialResistanceOhm) {
  if (config_.sensors.resistance_source != "simulated") {
    for (double& value : sample_resistance_ohm_) {
      value = 0.0;
    }
  }
}

void SensorManager::NotePullCompleted(int motor_id) {
  if (config_.sensors.resistance_source != "simulated") {
    return;
  }

  std::size_t start = 0;
  std::size_t end = 0;
  if (motor_id == 0) {
    start = 0;
    end = 4;
  } else if (motor_id == 1) {
    start = 4;
    end = sample_resistance_ohm_.size();
  } else {
    return;
  }
  for (std::size_t i = start; i < end && i < sample_resistance_ohm_.size(); ++i) {
    sample_resistance_ohm_[i] *= (1.0 - kResistanceDecayPerPull);
  }
}

SensorSnapshot SensorManager::ReadSnapshot(MissionPhase phase,
                                           const std::vector<double>& heater_duty,
                                           double dt_seconds) {
  const double dt = dt_seconds <= 1e-6 ? 1.0 : dt_seconds;

  // Simulation model used until final DPS310/DAQ132M/ADS1115 drivers are
  // bench-validated. Keep values deterministic and in the expected flight
  // ranges so command, logging, fallback, and GUI behavior can be tested.
  if (phase == MissionPhase::kDescent || phase == MissionPhase::kLanded) {
    pressure_descending_ = false;
  }
  pressure_mbar_ += (pressure_descending_ ? -1.5 : 1.8) * dt;
  pressure_mbar_ = std::clamp(pressure_mbar_, 5.0, 1013.25);

  double ambient_temp = -40.0;
  if (phase == MissionPhase::kFloat || phase == MissionPhase::kPreFloat) {
    ambient_temp = -55.0;
  } else if (phase == MissionPhase::kDescent) {
    ambient_temp = -15.0;
  } else if (phase == MissionPhase::kLanded) {
    ambient_temp = 0.0;
  }

  for (std::size_t i = 0; i < sample_temps_c_.size(); ++i) {
    const double duty = (i < heater_duty.size()) ? heater_duty[i] : 0.0;
    const double heating = duty * 5.0 * dt;
    const double cooling = (sample_temps_c_[i] - ambient_temp) * 0.03 * dt;
    sample_temps_c_[i] += heating - cooling;
  }

  SensorSnapshot snapshot;
  snapshot.rtc_valid = rtc_ != nullptr ? rtc_->valid() : false;
  snapshot.timestamp_utc = rtc_ != nullptr ? rtc_->NowUtcIso8601()
                                           : "1970-01-01T00:00:00Z";
  snapshot.ambient_temp_c = ambient_temp;
  snapshot.ambient_pressure_mbar = pressure_mbar_;
  snapshot.uv = (phase == MissionPhase::kFloat || phase == MissionPhase::kPreFloat)
                    ? 1.8
                    : 0.4;
  snapshot.sample_temps_c = sample_temps_c_;

  if (config_.sensors.resistance_source == "disabled") {
    resistance_ok_ = true;
    snapshot.sample_resistance_ohm.assign(sample_temps_c_.size(), 0.0);
  } else if (config_.sensors.resistance_source == "simulated") {
    resistance_ok_ = true;
    snapshot.sample_resistance_ohm = sample_resistance_ohm_;
  } else if (ina_ != nullptr && ina_->healthy()) {
    resistance_ok_ = true;
    snapshot.sample_resistance_ohm = sample_resistance_ohm_;
  } else {
    resistance_ok_ = false;
    snapshot.sample_resistance_ohm.assign(sample_temps_c_.size(), 0.0);
  }

  t_ambient_ok_ =
      (snapshot.ambient_temp_c >= config_.sensor_range.ambient_temp_min_c) &&
      (snapshot.ambient_temp_c <= config_.sensor_range.ambient_temp_max_c);
  p_ambient_ok_ =
      (snapshot.ambient_pressure_mbar >= config_.sensor_range.ambient_pressure_min_mbar) &&
      (snapshot.ambient_pressure_mbar <= config_.sensor_range.ambient_pressure_max_mbar);

  if (spi_ != nullptr && i2c_ != nullptr) {
    if (!spi_->healthy() || !i2c_->healthy()) {
      snapshot.uv = 0.0;
    }
  }

  return snapshot;
}

}  // namespace coatheal
