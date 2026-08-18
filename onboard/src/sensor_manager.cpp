#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

#include "coatheal/hal/sequent_rtd_adapter.hpp"

#if defined(__linux__) && __has_include(<linux/i2c-dev.h>)
#define COATHEAL_HAS_LINUX_SENSOR_IO 1
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#define COATHEAL_HAS_LINUX_SENSOR_IO 0
#endif

namespace coatheal {
namespace {

constexpr double kInitialResistanceOhm = 100.0;
constexpr double kResistanceDecayPerPull = 0.05;
constexpr const char* kI2cDevice = "/dev/i2c-1";
constexpr double kNoReading = std::numeric_limits<double>::quiet_NaN();

std::int32_t SignExtend(std::uint32_t value, int bits) {
  const std::uint32_t sign = 1U << (bits - 1);
  if ((value & sign) != 0U) {
    value |= ~((1U << bits) - 1U);
  }
  return static_cast<std::int32_t>(value);
}

// The HAL deliberately does not depend on OnboardConfig, so the translation
// from configuration keys to adapter options lives here, at the one place
// that knows about both.
SequentRtdAdapter::Options MakeSequentOptions(const OnboardConfig& config) {
  SequentRtdAdapter::Options options;
  options.stack = config.sensors.sequent_rtd_stack;
  options.expect_pt1000 =
      config.sensors.sequent_rtd_expect_sensor_type == "pt1000";
  options.resistance_min_ohm = config.sensors.sequent_rtd_resistance_min_ohm;
  options.resistance_max_ohm = config.sensors.sequent_rtd_resistance_max_ohm;
  options.crosscheck_tol_c = config.sensors.sequent_rtd_crosscheck_tol_c;
  for (std::size_t i = 0;
       i < options.channel_map.size() &&
       i < config.sensors.sequent_rtd_channels.size();
       ++i) {
    options.channel_map[i] =
        static_cast<std::uint8_t>(config.sensors.sequent_rtd_channels[i]);
  }
  return options;
}

void AppendSequentIdentity(std::ostringstream* oss,
                           const SequentRtdAdapter::Identity& identity) {
  *oss << ";sequent_rtd_card_type=" << static_cast<int>(identity.card_type)
       << ";sequent_rtd_fw=" << static_cast<int>(identity.fw_major) << '.'
       << static_cast<int>(identity.fw_minor)
       << ";sequent_rtd_hw=" << static_cast<int>(identity.hw_major) << '.'
       << static_cast<int>(identity.hw_minor)
       << ";sequent_rtd_sensor=" << (identity.pt1000 ? "pt1000" : "pt100");
}

// Card housekeeping only. None of this ever reaches control; it is here so a
// bench operator can see the card is powered and the ADC is not resetting.
void AppendSequentDiagnostics(std::ostringstream* oss,
                              const SequentRtdAdapter::Reading& reading) {
  const std::ios_base::fmtflags flags = oss->flags();
  const std::streamsize precision = oss->precision();
  *oss << ";sequent_rtd_card_temp_c=" << std::fixed << std::setprecision(1)
       << reading.card_temp_c << ";sequent_rtd_rail_5v=" << std::setprecision(3)
       << reading.rail_5v
       << ";sequent_rtd_adc_reinit=" << reading.adc_reinit_count;
  oss->flags(flags);
  oss->precision(precision);
}

#if COATHEAL_HAS_LINUX_SENSOR_IO
int OpenI2c(int address) {
  const int fd = ::open(kI2cDevice, O_RDWR);
  if (fd < 0) return -1;
  if (::ioctl(fd, I2C_SLAVE, address) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool WriteI2cRegister(int fd, std::uint8_t reg, std::uint8_t value) {
  const std::uint8_t data[2] = {reg, value};
  return ::write(fd, data, sizeof(data)) == static_cast<ssize_t>(sizeof(data));
}

bool ReadI2cRegisters(int fd, std::uint8_t reg, std::uint8_t* data,
                      std::size_t size) {
  if (::write(fd, &reg, 1) != 1) return false;
  return ::read(fd, data, size) == static_cast<ssize_t>(size);
}
#endif

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
      sample_resistance_ohm_(config.hardware.sample_count, kInitialResistanceOhm),
      simulated_(config.runtime.use_simulated_sensors),
      sample_cache_(config.hardware.sample_count),
      rtd_bus_(),
      rtd_(&rtd_bus_, MakeSequentOptions(config)) {
  if (config_.sensors.resistance_source != "simulated") {
    std::fill(sample_resistance_ohm_.begin(), sample_resistance_ohm_.end(), 0.0);
  }
  dps_health_.state = config_.sensors.dps310_enabled
                          ? ComponentState::kDiscovering
                          : ComponentState::kDisabled;
  ads_health_.state = config_.sensors.ads1115_enabled
                          ? ComponentState::kDiscovering
                          : ComponentState::kDisabled;
  // There is no enable key for the RTD card: it is the only sample
  // temperature source, so "configured off" is not a state it can be in.
  // What it can be is unreachable, on a build host with no Linux I2C at
  // all, and the transport seam reports that as DISABLED rather than
  // FAILED so a desktop build is not mistaken for broken flight hardware.
  rtd_health_.state = rtd_bus_.available() ? ComponentState::kDiscovering
                                           : ComponentState::kDisabled;
  if (!rtd_bus_.available()) rtd_health_.error = "I2C_UNAVAILABLE";
  // Transitional: no RS485 device remains. Pinned false until Task 7
  // removes the flag and its STATUS wire field together.
  rs485_ok_ = false;
}

SensorManager::~SensorManager() { Stop(); }

void SensorManager::Start() {
  if (simulated_ || running_.exchange(true)) return;
  if (config_.sensors.dps310_enabled) {
    dps_thread_ = std::thread(&SensorManager::DpsLoop, this);
  }
  if (config_.sensors.ads1115_enabled) {
    ads_thread_ = std::thread(&SensorManager::AdsLoop, this);
  }
  if (rtd_bus_.available()) {
    rtd_thread_ = std::thread(&SensorManager::SequentRtdLoop, this);
  }
}

void SensorManager::Stop() {
  if (!running_.exchange(false)) return;
  stop_cv_.notify_all();
  if (dps_thread_.joinable()) dps_thread_.join();
  if (ads_thread_.joinable()) ads_thread_.join();
  if (rtd_thread_.joinable()) rtd_thread_.join();
}

bool SensorManager::WaitForPoll(int milliseconds) {
  std::unique_lock<std::mutex> lock(stop_mu_);
  return stop_cv_.wait_for(
      lock, std::chrono::milliseconds(milliseconds),
      [this]() { return !running_.load(); });
}

std::int64_t SensorManager::AgeMs(
    const std::chrono::steady_clock::time_point& value,
    bool has_value) const {
  if (!has_value) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - value)
      .count();
}

ComponentState SensorManager::FailedState(
    bool has_success,
    const std::chrono::steady_clock::time_point& last_success) const {
  if (!has_success) return ComponentState::kFailed;
  return AgeMs(last_success, true) >= config_.sensors.stale_after_ms
             ? ComponentState::kStale
             : ComponentState::kDegraded;
}

void SensorManager::NotePullCompleted(int motor_id) {
  if (config_.sensors.resistance_source != "simulated") return;
  const std::size_t start = motor_id == 0 ? 0U : 4U;
  if (motor_id != 0 && motor_id != 1) return;
  // SequentRtdLoop also writes sample_resistance_ohm_, from the RTD worker
  // thread, so this decay has to run under the same mutex.
  std::lock_guard<std::mutex> lock(cache_mu_);
  const std::size_t end = motor_id == 0 ? 4U : sample_resistance_ohm_.size();
  for (std::size_t i = start; i < end && i < sample_resistance_ohm_.size(); ++i) {
    sample_resistance_ohm_[i] *= (1.0 - kResistanceDecayPerPull);
  }
}

bool SensorManager::ReadDps310At(int address, double* temp_c,
                                 double* pressure_mbar) {
#if COATHEAL_HAS_LINUX_SENSOR_IO
  const int fd = OpenI2c(address);
  if (fd < 0) return false;

  std::uint8_t id = 0;
  if (!ReadI2cRegisters(fd, 0x0D, &id, 1) || (id & 0xF0U) != 0x10U) {
    ::close(fd);
    return false;
  }

  std::array<std::uint8_t, 18> coeff{};
  std::uint8_t coef_source = 0;
  bool ok = ReadI2cRegisters(fd, 0x10, coeff.data(), coeff.size()) &&
            ReadI2cRegisters(fd, 0x28, &coef_source, 1);
  const std::uint8_t osr8 = 0x03;
  ok = WriteI2cRegister(fd, 0x06, osr8) && ok;
  ok = WriteI2cRegister(fd, 0x07,
                        static_cast<std::uint8_t>((coef_source & 0x80U) | osr8)) &&
       ok;
  ok = WriteI2cRegister(fd, 0x09, 0x00) && ok;
  ok = WriteI2cRegister(fd, 0x08, 0x07) && ok;
  if (!ok) {
    ::close(fd);
    return false;
  }

  std::uint8_t ready = 0;
  bool measurement_ready = false;
  for (int attempt = 0; attempt < 25; ++attempt) {
    if (ReadI2cRegisters(fd, 0x08, &ready, 1) &&
        (ready & 0x30U) == 0x30U) {
      measurement_ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  std::array<std::uint8_t, 6> raw{};
  if (!measurement_ready ||
      !ReadI2cRegisters(fd, 0x00, raw.data(), raw.size())) {
    ::close(fd);
    return false;
  }
  ::close(fd);

  const std::int32_t raw_pressure = SignExtend(
      (static_cast<std::uint32_t>(raw[0]) << 16U) |
          (static_cast<std::uint32_t>(raw[1]) << 8U) | raw[2],
      24);
  const std::int32_t raw_temp = SignExtend(
      (static_cast<std::uint32_t>(raw[3]) << 16U) |
          (static_cast<std::uint32_t>(raw[4]) << 8U) | raw[5],
      24);

  const std::int32_t c0 = SignExtend(
      (static_cast<std::uint32_t>(coeff[0]) << 4U) | (coeff[1] >> 4U), 12);
  const std::int32_t c1 = SignExtend(
      ((static_cast<std::uint32_t>(coeff[1]) & 0x0FU) << 8U) | coeff[2], 12);
  const std::int32_t c00 = SignExtend(
      (static_cast<std::uint32_t>(coeff[3]) << 12U) |
          (static_cast<std::uint32_t>(coeff[4]) << 4U) | (coeff[5] >> 4U),
      20);
  const std::int32_t c10 = SignExtend(
      ((static_cast<std::uint32_t>(coeff[5]) & 0x0FU) << 16U) |
          (static_cast<std::uint32_t>(coeff[6]) << 8U) | coeff[7],
      20);
  const auto s16 = [&](int index) {
    return SignExtend((static_cast<std::uint32_t>(coeff[index]) << 8U) |
                          coeff[index + 1],
                      16);
  };
  const std::int32_t c01 = s16(8);
  const std::int32_t c11 = s16(10);
  const std::int32_t c20 = s16(12);
  const std::int32_t c21 = s16(14);
  const std::int32_t c30 = s16(16);

  constexpr double kScaleOsr8 = 7864320.0;
  const double p = raw_pressure / kScaleOsr8;
  const double t = raw_temp / kScaleOsr8;
  const double compensated_temp = c0 * 0.5 + c1 * t;
  const double compensated_pressure_pa =
      c00 + p * (c10 + p * (c20 + p * c30)) +
      t * c01 + t * p * (c11 + p * c21);
  if (!std::isfinite(compensated_temp) ||
      !std::isfinite(compensated_pressure_pa)) {
    return false;
  }
  *temp_c = compensated_temp;
  *pressure_mbar = compensated_pressure_pa / 100.0;
  return true;
#else
  (void)address;
  (void)temp_c;
  (void)pressure_mbar;
  return false;
#endif
}

bool SensorManager::ReadAds1115At(int address, double* voltage) {
#if COATHEAL_HAS_LINUX_SENSOR_IO
  const int fd = OpenI2c(address);
  if (fd < 0) return false;
  const int channel = config_.sensors.uv_ads1115_channel;
  const std::uint16_t cfg = static_cast<std::uint16_t>(
      0x8000U | ((4U + static_cast<unsigned int>(channel)) << 12U) |
      (1U << 9U) | (1U << 8U) | (4U << 5U) | 0x0003U);
  const std::uint8_t write_cfg[3] = {
      0x01, static_cast<std::uint8_t>(cfg >> 8U),
      static_cast<std::uint8_t>(cfg & 0xFFU)};
  if (::write(fd, write_cfg, sizeof(write_cfg)) !=
      static_cast<ssize_t>(sizeof(write_cfg))) {
    ::close(fd);
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::uint8_t raw[2] = {0, 0};
  if (!ReadI2cRegisters(fd, 0x00, raw, sizeof(raw))) {
    ::close(fd);
    return false;
  }
  ::close(fd);
  const std::int16_t counts =
      static_cast<std::int16_t>((static_cast<std::uint16_t>(raw[0]) << 8U) |
                                raw[1]);
  *voltage = static_cast<double>(counts) * config_.sensors.uv_full_scale_v /
             32768.0;
  return std::isfinite(*voltage);
#else
  (void)address;
  (void)voltage;
  return false;
#endif
}

bool SensorManager::Pt100TemperatureFromResistance(double resistance_ohm,
                                                   double* temperature_c) {
  return Pt100TemperatureFromOhms(resistance_ohm, temperature_c);
}

void SensorManager::DpsLoop() {
  while (running_.load()) {
    double temp = 0.0;
    double pressure = 0.0;
    bool ok = false;
    int address = resolved_dps_address_ >= 0
                      ? resolved_dps_address_
                      : config_.sensors.dps310_i2c_addr;
    std::vector<int> candidates = {address};
    if (config_.sensors.dps310_auto_discover) {
      for (const int candidate : {0x76, 0x77}) {
        if (std::find(candidates.begin(), candidates.end(), candidate) ==
            candidates.end()) {
          candidates.push_back(candidate);
        }
      }
    }
    {
      std::lock_guard<std::mutex> io_lock(dps_io_mu_);
      for (const int candidate : candidates) {
        if (ReadDps310At(candidate, &temp, &pressure)) {
          address = candidate;
          ok = true;
          break;
        }
      }
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      if (ok) {
        resolved_dps_address_ = address;
        ambient_temp_cache_ = {temp, true, true, now};
        pressure_cache_ = {pressure, true, true, now};
        dps_health_ = {ComponentState::kOk, "NONE", 0};
      } else {
        ambient_temp_cache_.valid = false;
        pressure_cache_.valid = false;
        dps_health_.state =
            FailedState(ambient_temp_cache_.has_value,
                        ambient_temp_cache_.last_success);
        dps_health_.error = "NO_RESPONSE";
        dps_health_.last_success_age_ms =
            AgeMs(ambient_temp_cache_.last_success,
                  ambient_temp_cache_.has_value);
        resolved_dps_address_ = -1;
      }
    }
    if (WaitForPoll(config_.sensors.dps310_poll_ms)) break;
  }
}

void SensorManager::AdsLoop() {
  while (running_.load()) {
    double voltage = 0.0;
    bool ok = false;
    int address = resolved_ads_address_ >= 0
                      ? resolved_ads_address_
                      : config_.sensors.ads1115_i2c_addr;
    std::vector<int> candidates = {address};
    if (config_.sensors.ads1115_auto_discover) {
      for (const int candidate : {0x48, 0x49, 0x4A, 0x4B}) {
        if (std::find(candidates.begin(), candidates.end(), candidate) ==
            candidates.end()) {
          candidates.push_back(candidate);
        }
      }
    }
    {
      std::lock_guard<std::mutex> io_lock(ads_io_mu_);
      for (const int candidate : candidates) {
        if (ReadAds1115At(candidate, &voltage)) {
          address = candidate;
          ok = true;
          break;
        }
      }
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      if (ok) {
        resolved_ads_address_ = address;
        uv_cache_ = {voltage, true, true, now};
        ads_health_ = {ComponentState::kOk, "NONE", 0};
      } else {
        uv_cache_.valid = false;
        ads_health_.state =
            FailedState(uv_cache_.has_value, uv_cache_.last_success);
        ads_health_.error = "NO_RESPONSE";
        ads_health_.last_success_age_ms =
            AgeMs(uv_cache_.last_success, uv_cache_.has_value);
        resolved_ads_address_ = -1;
      }
    }
    if (WaitForPoll(config_.sensors.ads1115_poll_ms)) break;
  }
}

// sample_temp_ok_ gates the thermal path, so it tracks only the channels a
// heater actually controls. Samples 6 and 7 are pulled but unheated: losing
// one is a data-quality event, not a safety event, and must not disable
// heating. The converse also follows and is the point of the change: a
// heated channel going bad can no longer be masked by a healthy unheated
// one, which the previous any_of() allowed.
//
// Fails closed on an empty mapping and on a mapping that points past the end
// of the sample vector. Both are configuration errors, and on a
// configuration error we do not know which channels gate heating, so the
// only safe answer is "not ok".
bool SensorManager::HeatedChannelsValid(
    const OnboardConfig& config, const std::vector<bool>& channel_valid) {
  if (config.heaters.temperature_channels.empty()) return false;
  for (const std::size_t channel : config.heaters.temperature_channels) {
    if (channel >= channel_valid.size()) return false;
    if (!channel_valid[channel]) return false;
  }
  return true;
}

void SensorManager::SequentRtdLoop() {
  while (running_.load()) {
    SequentRtdAdapter::Reading reading;
    std::string error = "NO_RESPONSE";
    bool ok = false;
    {
      std::lock_guard<std::mutex> io_lock(rtd_io_mu_);
      if (!rtd_probed_) {
        rtd_probed_ = rtd_.Probe(&rtd_identity_, &error);
      }
      if (rtd_probed_) {
        ok = rtd_.ReadAll(&reading, &error);
        // A failed ReadAll means the card stopped answering, so the identity
        // we hold may no longer describe what is on the bus. Re-probe on the
        // next pass. The adapter itself decides whether that re-probe also
        // re-opens the bus: it clears its open_ on I/O failures only, since
        // re-opening cannot change a configuration rejection.
        if (!ok) rtd_probed_ = false;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      std::vector<bool> channel_valid(sample_cache_.size(), false);
      std::size_t valid_count = 0;

      if (ok) {
        rtd_last_reading_ = reading;
        rtd_has_reading_ = true;
        for (std::size_t i = 0;
             i < sample_cache_.size() &&
             i < SequentRtdAdapter::kChannelCount;
             ++i) {
          if (reading.channel_valid[i]) {
            sample_cache_[i] = {reading.temperature_c[i], true, true, now};
            sample_resistance_ohm_[i] = reading.resistance_ohm[i];
            channel_valid[i] = true;
            ++valid_count;
          } else {
            sample_cache_[i].valid = false;
          }
        }
        rtd_health_.state = valid_count == sample_cache_.size()
                                ? ComponentState::kOk
                                : ComponentState::kDegraded;
        rtd_health_.error = valid_count == 0
                                ? "NO_VALID_CHANNELS"
                                : (valid_count < sample_cache_.size()
                                       ? "PARTIAL_CHANNELS"
                                       : "NONE");
        rtd_health_.last_success_age_ms = 0;
      } else {
        bool any_previous = false;
        std::chrono::steady_clock::time_point newest{};
        for (auto& sample : sample_cache_) {
          sample.valid = false;
          if (sample.has_value &&
              (!any_previous || sample.last_success > newest)) {
            newest = sample.last_success;
            any_previous = true;
          }
        }
        rtd_health_.state = FailedState(any_previous, newest);
        rtd_health_.error = error.empty() ? "NO_RESPONSE" : error;
        rtd_health_.last_success_age_ms = AgeMs(newest, any_previous);
      }

      // Staleness applies on top of per-channel validity, so rtd_health_ and
      // channel_valid already carry everything ReadSnapshot needs; it does
      // not recompute per-channel state for this component.
      for (std::size_t i = 0; i < sample_cache_.size(); ++i) {
        const std::int64_t age =
            AgeMs(sample_cache_[i].last_success, sample_cache_[i].has_value);
        if (age < 0 || age >= config_.sensors.stale_after_ms) {
          channel_valid[i] = false;
        }
      }

      // These three are the between-snapshot values. ReadSnapshot recomputes
      // i2c_ok_ and resistance_ok_ from their own inputs on every control
      // tick, and recomputes sample_temp_ok_ through this same helper.
      i2c_ok_ = ok;
      resistance_ok_ = ok;
      sample_temp_ok_ = HeatedChannelsValid(config_, channel_valid);
    }
    if (WaitForPoll(config_.sensors.sequent_rtd_poll_ms)) break;
  }
}

SensorSnapshot SensorManager::ReadSimulatedSnapshot(
    MissionPhase phase, const std::vector<double>& heater_duty,
    double dt_seconds) {
  const double dt = dt_seconds <= 1e-6 ? 1.0 : dt_seconds;
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
    const double duty = i < heater_duty.size() ? heater_duty[i] : 0.0;
    sample_temps_c_[i] +=
        duty * 5.0 * dt - (sample_temps_c_[i] - ambient_temp) * 0.03 * dt;
  }

  SensorSnapshot snapshot;
  snapshot.rtc_valid = rtc_ != nullptr ? rtc_->valid() : false;
  snapshot.timestamp_utc =
      rtc_ != nullptr ? rtc_->NowUtcIso8601() : "1970-01-01T00:00:00Z";
  snapshot.ambient_temp_c = ambient_temp;
  snapshot.ambient_pressure_mbar = pressure_mbar_;
  snapshot.uv =
      (phase == MissionPhase::kFloat || phase == MissionPhase::kPreFloat) ? 1.8
                                                                          : 0.4;
  snapshot.sample_temps_c = sample_temps_c_;
  snapshot.sample_temp_valid.assign(sample_temps_c_.size(), true);
  snapshot.sample_temp_age_ms.assign(sample_temps_c_.size(), 0);
  snapshot.ambient_temp_age_ms = 0;
  snapshot.ambient_pressure_age_ms = 0;
  snapshot.uv_age_ms = 0;
  snapshot.dps310 = {ComponentState::kOk, "SIMULATED", 0};
  snapshot.ads1115 = {ComponentState::kOk, "SIMULATED", 0};
  // Transitional (Task 7 collapses these two into one sequent_rtd field):
  // rtd_click now carries the Sequent card, and daq132m has no acquisition
  // path behind it at all, so it reports DISABLED rather than a plausible
  // OK for a device that is gone.
  snapshot.daq132m = {ComponentState::kDisabled, "REMOVED", -1};
  snapshot.rtd_click = {ComponentState::kOk, "SIMULATED", 0};
  snapshot.simulated = true;
  i2c_ok_ = rs485_ok_ = sample_temp_ok_ = uv_ok_ = true;
  return snapshot;
}

SensorSnapshot SensorManager::ReadSnapshot(
    MissionPhase phase, const std::vector<double>& heater_duty,
    double dt_seconds) {
  SensorSnapshot snapshot =
      simulated_ ? ReadSimulatedSnapshot(phase, heater_duty, dt_seconds)
                 : SensorSnapshot{};
  if (!simulated_) {
    snapshot.rtc_valid = rtc_ != nullptr ? rtc_->valid() : false;
    snapshot.timestamp_utc =
        rtc_ != nullptr ? rtc_->NowUtcIso8601() : "1970-01-01T00:00:00Z";
    std::lock_guard<std::mutex> lock(cache_mu_);
    snapshot.ambient_temp_c =
        ambient_temp_cache_.has_value ? ambient_temp_cache_.value : kNoReading;
    snapshot.ambient_pressure_mbar =
        pressure_cache_.has_value ? pressure_cache_.value : kNoReading;
    snapshot.uv = uv_cache_.has_value ? uv_cache_.value : kNoReading;
    snapshot.ambient_temp_valid = ambient_temp_cache_.valid;
    snapshot.ambient_pressure_valid = pressure_cache_.valid;
    snapshot.uv_valid = uv_cache_.valid;
    snapshot.ambient_temp_age_ms =
        AgeMs(ambient_temp_cache_.last_success,
              ambient_temp_cache_.has_value);
    snapshot.ambient_pressure_age_ms =
        AgeMs(pressure_cache_.last_success, pressure_cache_.has_value);
    snapshot.uv_age_ms =
        AgeMs(uv_cache_.last_success, uv_cache_.has_value);
    snapshot.ambient_temp_valid =
        snapshot.ambient_temp_valid &&
        snapshot.ambient_temp_age_ms >= 0 &&
        snapshot.ambient_temp_age_ms < config_.sensors.stale_after_ms;
    snapshot.ambient_pressure_valid =
        snapshot.ambient_pressure_valid &&
        snapshot.ambient_pressure_age_ms >= 0 &&
        snapshot.ambient_pressure_age_ms < config_.sensors.stale_after_ms;
    snapshot.uv_valid =
        snapshot.uv_valid && snapshot.uv_age_ms >= 0 &&
        snapshot.uv_age_ms < config_.sensors.stale_after_ms;
    snapshot.sample_temps_c.resize(sample_cache_.size(), kNoReading);
    snapshot.sample_temp_valid.resize(sample_cache_.size(), false);
    snapshot.sample_temp_age_ms.resize(sample_cache_.size(), -1);
    for (std::size_t i = 0; i < sample_cache_.size(); ++i) {
      if (sample_cache_[i].has_value) {
        snapshot.sample_temps_c[i] = sample_cache_[i].value;
      }
      snapshot.sample_temp_age_ms[i] =
          AgeMs(sample_cache_[i].last_success, sample_cache_[i].has_value);
      snapshot.sample_temp_valid[i] =
          sample_cache_[i].valid &&
          snapshot.sample_temp_age_ms[i] >= 0 &&
          snapshot.sample_temp_age_ms[i] < config_.sensors.stale_after_ms;
    }
    snapshot.sample_temps_valid =
        std::any_of(snapshot.sample_temp_valid.begin(),
                    snapshot.sample_temp_valid.end(),
                    [](bool valid) { return valid; });
    snapshot.dps310 = dps_health_;
    snapshot.ads1115 = ads_health_;
    // Transitional (Task 7 collapses these two into one sequent_rtd field):
    // rtd_click carries the Sequent card's health verbatim, and daq132m has
    // no acquisition path behind it, so it is pinned DISABLED rather than
    // left at the ComponentHealth default of DISCOVERING/NOT_POLLED, which
    // would put a misleading state on the wire.
    snapshot.daq132m = {ComponentState::kDisabled, "REMOVED", -1};
    snapshot.rtd_click = rtd_health_;
    snapshot.dps310.last_success_age_ms =
        snapshot.ambient_temp_age_ms;
    snapshot.ads1115.last_success_age_ms = snapshot.uv_age_ms;
    if (config_.sensors.dps310_enabled &&
        ambient_temp_cache_.has_value &&
        !snapshot.ambient_temp_valid) {
      snapshot.dps310.state =
          snapshot.ambient_temp_age_ms >= config_.sensors.stale_after_ms
              ? ComponentState::kStale
              : ComponentState::kDegraded;
    }
    if (config_.sensors.ads1115_enabled && uv_cache_.has_value &&
        !snapshot.uv_valid) {
      snapshot.ads1115.state =
          snapshot.uv_age_ms >= config_.sensors.stale_after_ms
              ? ComponentState::kStale
              : ComponentState::kDegraded;
    }
    // No per-channel post-processing for the RTD card. SequentRtdLoop already
    // folds staleness into each channel's validity before it writes
    // rtd_health_, so the health it published is the health we report. The
    // single-channel rtd_click block and the per-channel DAQ block that used
    // to sit here both existed to recompute what the worker did not.
    i2c_ok_ =
        (!config_.sensors.dps310_enabled ||
         (snapshot.ambient_temp_valid && snapshot.ambient_pressure_valid)) &&
        (!config_.sensors.ads1115_enabled || snapshot.uv_valid);
    uv_ok_ = snapshot.uv_valid;
    // The thermal-path flag follows only the channels a heater controls.
    // snapshot.sample_temp_valid is exactly the valid-and-fresh vector the
    // policy wants, and it is recomputed here rather than reused from the
    // worker so the flag ages out between RTD polls instead of latching on
    // the last successful read.
    sample_temp_ok_ = HeatedChannelsValid(config_, snapshot.sample_temp_valid);
  }

  {
    // sample_resistance_ohm_ is written by SequentRtdLoop under cache_mu_,
    // so the read side has to hold it too.
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (config_.sensors.resistance_source == "disabled") {
      resistance_ok_ = true;
      snapshot.sample_resistance_ohm.assign(config_.hardware.sample_count, 0.0);
    } else if (config_.sensors.resistance_source == "simulated") {
      resistance_ok_ = true;
      snapshot.sample_resistance_ohm = sample_resistance_ohm_;
    } else if (ina_ != nullptr && ina_->healthy()) {
      resistance_ok_ = true;
      snapshot.sample_resistance_ohm = sample_resistance_ohm_;
    } else {
      resistance_ok_ = false;
      snapshot.sample_resistance_ohm.assign(config_.hardware.sample_count, 0.0);
    }
  }

  t_ambient_ok_ =
      snapshot.ambient_temp_valid &&
      snapshot.ambient_temp_c >= config_.sensor_range.ambient_temp_min_c &&
      snapshot.ambient_temp_c <= config_.sensor_range.ambient_temp_max_c;
  p_ambient_ok_ =
      snapshot.ambient_pressure_valid &&
      snapshot.ambient_pressure_mbar >=
          config_.sensor_range.ambient_pressure_min_mbar &&
      snapshot.ambient_pressure_mbar <=
          config_.sensor_range.ambient_pressure_max_mbar;
  if (i2c_ != nullptr) i2c_->set_healthy(i2c_ok_.load());
  return snapshot;
}

bool SensorManager::ActiveCheck(const std::string& component,
                                std::string* details) {
  if (simulated_) {
    if (details != nullptr) *details = component + "=OK;simulated=1";
    return true;
  }

  auto check_dps = [&]() {
    if (!config_.sensors.dps310_enabled) return true;
    std::vector<int> candidates = {config_.sensors.dps310_i2c_addr};
    if (config_.sensors.dps310_auto_discover) {
      for (const int address : {0x76, 0x77}) {
        if (std::find(candidates.begin(), candidates.end(), address) ==
            candidates.end()) {
          candidates.push_back(address);
        }
      }
    }
    std::lock_guard<std::mutex> lock(dps_io_mu_);
    for (const int address : candidates) {
      double temp = 0.0;
      double pressure = 0.0;
      if (ReadDps310At(address, &temp, &pressure)) return true;
    }
    return false;
  };

  auto check_ads = [&]() {
    if (!config_.sensors.ads1115_enabled) return true;
    std::vector<int> candidates = {config_.sensors.ads1115_i2c_addr};
    if (config_.sensors.ads1115_auto_discover) {
      for (const int address : {0x48, 0x49, 0x4A, 0x4B}) {
        if (std::find(candidates.begin(), candidates.end(), address) ==
            candidates.end()) {
          candidates.push_back(address);
        }
      }
    }
    std::lock_guard<std::mutex> lock(ads_io_mu_);
    for (const int address : candidates) {
      double voltage = 0.0;
      if (ReadAds1115At(address, &voltage)) return true;
    }
    return false;
  };

  std::string rtd_error = "SKIPPED";
  SequentRtdAdapter::Identity identity;
  SequentRtdAdapter::Reading reading;
  auto check_rtd = [&]() {
    if (!rtd_bus_.available()) {
      rtd_error = "I2C_UNAVAILABLE";
      return false;
    }
    rtd_error.clear();
    std::lock_guard<std::mutex> lock(rtd_io_mu_);
    // A CHECK is an on-demand full conversation: probe first so the reply
    // carries a freshly-read identity, then read every channel.
    bool ok = rtd_.Probe(&identity, &rtd_error);
    if (ok) {
      ok = rtd_.ReadAll(&reading, &rtd_error);
      if (ok) rtd_identity_ = identity;
    }
    // Keep the worker's probe state consistent with what we just observed,
    // so a CHECK never leaves the loop trusting a card that just failed.
    rtd_probed_ = ok;
    if (ok && rtd_error.empty()) rtd_error = "NONE";
    if (!ok && rtd_error.empty()) rtd_error = "UNKNOWN";
    return ok;
  };

  const bool dps_requested = component == "ALL" || component == "DPS310";
  const bool ads_requested = component == "ALL" || component == "ADS1115";
  // DAQ132M and RTD_CLICK survive as request aliases only. Both legacy
  // acquisition paths were replaced by the one Sequent card, and these are
  // still the names system_controller's CHECK whitelist accepts, so routing
  // them here keeps CHECK DAQ132M from silently succeeding against nothing.
  // Task 7 renames the command surface.
  const bool rtd_requested = component == "ALL" || component == "RTD_CLICK" ||
                             component == "DAQ132M";
  const bool dps_ok = !dps_requested || check_dps();
  const bool ads_ok = !ads_requested || check_ads();
  const bool rtd_ok = !rtd_requested || check_rtd();
  if (details != nullptr) {
    std::ostringstream oss;
    oss << "dps310=" << (!dps_requested ? "SKIPPED"
                                        : (dps_ok ? "OK" : "FAIL"))
        << ";ads1115=" << (!ads_requested ? "SKIPPED"
                                          : (ads_ok ? "OK" : "FAIL"))
        << ";sequent_rtd=" << (!rtd_requested ? "SKIPPED"
                                              : (rtd_ok ? "OK" : "FAIL"))
        << ";sequent_rtd_error=" << (!rtd_requested ? "SKIPPED" : rtd_error);
    if (rtd_requested) {
      oss << ";sequent_rtd_addr=0x" << std::hex << rtd_.address() << std::dec
          << ";sequent_rtd_burst=" << (rtd_.burst_mode() ? "1" : "0");
      if (rtd_ok) {
        AppendSequentIdentity(&oss, identity);
        AppendSequentDiagnostics(&oss, reading);
      }
    }
    *details = oss.str();
  }
  return dps_ok && ads_ok && rtd_ok;
}

std::string SensorManager::ComponentSummary() const {
  if (simulated_) {
    return "dps310=OK;ads1115=OK;sequent_rtd=OK;sample_valid_channels=" +
           std::to_string(config_.hardware.sample_count) +
           ";heated_channels_ok=1;simulated=1";
  }
  // rtd_identity_ and the adapter's own state live under rtd_io_mu_, the
  // sample cache under cache_mu_. Snapshot the first and release it before
  // taking the second, exactly as SequentRtdLoop does, so no code path ever
  // holds both at once.
  SequentRtdAdapter::Identity identity;
  bool probed = false;
  int address = 0;
  bool burst = false;
  {
    std::lock_guard<std::mutex> io_lock(rtd_io_mu_);
    identity = rtd_identity_;
    probed = rtd_probed_;
    address = rtd_.address();
    burst = rtd_.burst_mode();
  }
  std::lock_guard<std::mutex> lock(cache_mu_);
  ComponentHealth dps = dps_health_;
  ComponentHealth ads = ads_health_;
  const ComponentHealth rtd = rtd_health_;
  dps.last_success_age_ms =
      AgeMs(ambient_temp_cache_.last_success, ambient_temp_cache_.has_value);
  ads.last_success_age_ms =
      AgeMs(uv_cache_.last_success, uv_cache_.has_value);
  if (config_.sensors.dps310_enabled && ambient_temp_cache_.has_value &&
      dps.last_success_age_ms >= config_.sensors.stale_after_ms) {
    dps.state = ComponentState::kStale;
  }
  if (config_.sensors.ads1115_enabled && uv_cache_.has_value &&
      ads.last_success_age_ms >= config_.sensors.stale_after_ms) {
    ads.state = ComponentState::kStale;
  }
  // rtd_health_ is not re-derived here: SequentRtdLoop already folded
  // per-channel staleness into it before publishing.
  std::vector<bool> channel_valid(sample_cache_.size(), false);
  for (std::size_t i = 0; i < sample_cache_.size(); ++i) {
    const std::int64_t age =
        AgeMs(sample_cache_[i].last_success, sample_cache_[i].has_value);
    channel_valid[i] = sample_cache_[i].valid && age >= 0 &&
                       age < config_.sensors.stale_after_ms;
  }
  const std::size_t sample_valid_channels = static_cast<std::size_t>(
      std::count(channel_valid.begin(), channel_valid.end(), true));
  std::ostringstream oss;
  oss << "dps310=" << ToString(dps.state)
      << ";dps310_error=" << dps.error
      << ";dps310_age_ms=" << dps.last_success_age_ms
      << ";ads1115=" << ToString(ads.state)
      << ";ads1115_error=" << ads.error
      << ";ads1115_age_ms=" << ads.last_success_age_ms
      << ";sequent_rtd=" << ToString(rtd.state)
      << ";sequent_rtd_error=" << rtd.error
      << ";sequent_rtd_age_ms=" << rtd.last_success_age_ms
      << ";sequent_rtd_addr=0x" << std::hex << address << std::dec
      << ";sequent_rtd_burst=" << (burst ? "1" : "0");
  if (probed) AppendSequentIdentity(&oss, identity);
  if (rtd_has_reading_) AppendSequentDiagnostics(&oss, rtd_last_reading_);
  oss << ";sample_valid_channels=" << sample_valid_channels
      << ";heated_channels_ok="
      << (HeatedChannelsValid(config_, channel_valid) ? "1" : "0")
      << ";simulated=0";
  return oss.str();
}

}  // namespace coatheal
