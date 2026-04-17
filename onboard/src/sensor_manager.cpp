#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace coatheal {

namespace {
// Starting resistance of each crystalline sample before any pulls (ohms).
constexpr double kInitialResistanceOhm = 100.0;
// Fraction of resistance lost per simulated pull event. A real microcrack
// population decays resistance over many pulls; 5 % per pull is a plausible
// placeholder that gives the ground plotter something to watch.
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
      // Init samples at the floor so the simulation cooldown drives PID
      // activation as soon as ambient pulls them below the hysteresis band.
      sample_temps_c_(kSampleCount, config.phase.sample_floor_c),
      sample_resistance_ohm_(kSampleCount, kInitialResistanceOhm) {}

void SensorManager::NotePullCompleted(int motor_id) {
  // Rev B.1 simulation: a pull on motor 0 stresses samples 0..3, motor 1
  // stresses 4..7. Each pull knocks the resistance of all its samples
  // down by kResistanceDecayPerPull (multiplicative). The real instrument
  // will measure this directly on the INA3221; today it's synthetic.
  std::size_t start = 0;
  std::size_t end = 0;
  if (motor_id == 0) {
    start = 0;
    end = 4;
  } else if (motor_id == 1) {
    start = 4;
    end = kSampleCount;
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

  // Coarse pressure profile model for simulation and bench testing.
  // Floor is 5 mbar per BEXUS User Manual §5.6 — the experiment acceptance
  // pressure level — so vacuum-regime tests exercise the real flight envelope.
  if (phase == MissionPhase::kDescent || phase == MissionPhase::kLanded) {
    pressure_descending_ = false;
  }
  pressure_mbar_ += (pressure_descending_ ? -1.5 : 1.8) * dt;
  pressure_mbar_ = std::clamp(pressure_mbar_, 5.0, 1013.25);

  double ambient_temp = -40.0;
  if (phase == MissionPhase::kFloat) {
    ambient_temp = -55.0;
  } else if (phase == MissionPhase::kDescent) {
    ambient_temp = -15.0;
  } else if (phase == MissionPhase::kLanded) {
    ambient_temp = 0.0;
  }

  // Rev B.1: heaters 0..5 drive samples 0..5 one-to-one. Samples 6 and 7 are
  // pulled but unheated — they just cool toward ambient.
  for (std::size_t i = 0; i < sample_temps_c_.size(); ++i) {
    const double duty = (i < heater_duty.size()) ? heater_duty[i] : 0.0;
    const double heating = duty * 5.0 * dt;  // 5 W nominal heater
    const double cooling = (sample_temps_c_[i] - ambient_temp) * 0.03 * dt;
    sample_temps_c_[i] += heating - cooling;
  }

  SensorSnapshot snapshot;
  snapshot.rtc_valid = rtc_ != nullptr ? rtc_->valid() : false;
  snapshot.timestamp_utc = rtc_ != nullptr ? rtc_->NowUtcIso8601() : "1970-01-01T00:00:00Z";
  snapshot.ambient_temp_c = ambient_temp;
  snapshot.ambient_pressure_mbar = pressure_mbar_;
  snapshot.uv = phase == MissionPhase::kFloat ? 1.8 : 0.4;
  snapshot.sample_temps_c = sample_temps_c_;

  // Rev B.1 INA3221 sample resistance. When the adapter is absent or has
  // faulted, emit zeros and flip the status bit so the ground sees the
  // truth instead of stale values.
  resistance_ok_ = (ina_ != nullptr) && ina_->healthy();
  if (resistance_ok_) {
    snapshot.sample_resistance_ohm = sample_resistance_ohm_;
  } else {
    snapshot.sample_resistance_ohm.assign(sample_temps_c_.size(), 0.0);
  }

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
