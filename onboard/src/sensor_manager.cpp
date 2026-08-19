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

// v3 schematic SPI0 chip-select crossover (see the Global Constraints GPIO
// map): CE1/GP07 wires to click index 0 (SAMPLE1), which spidev enumerates
// as /dev/spidev0.1; CE0/GP08 wires to click index 1 (SAMPLE2), enumerated
// as /dev/spidev0.0. Fixed by hardware, not configurable.
constexpr const char* kClick1SpiDevice = "/dev/spidev0.1";  // click 0, SAMPLE1
constexpr const char* kClick2SpiDevice = "/dev/spidev0.0";  // click 1, SAMPLE2

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

// Same rationale as MakeSequentOptions: the HAL does not depend on
// OnboardConfig, so the config-key -> adapter-options translation lives
// here. Options::spi_speed_hz/settle_ms/conversion_ms are deliberately left
// at Max31865Adapter::Options{}'s datasheet-minimum defaults (500 kHz /
// 10 ms / 65 ms) -- the plan adds exactly three sensor.max31865_* keys
// (reference ohm, poll interval, sample indices) and no more, so per-click
// SPI speed or one-shot timing are not configurable.
Max31865Adapter::Options MakeMax31865Options(const OnboardConfig& config,
                                             const std::string& device) {
  Max31865Adapter::Options options;
  options.spi_device = device;
  options.reference_ohm = config.sensors.max31865_reference_ohm;
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
                             Ina3221Adapter* ina,
                             I2cBus* rtd_bus_override,
                             SpiBus* click1_bus_override,
                             SpiBus* click2_bus_override)
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
      rtd_bus_active_(rtd_bus_override != nullptr
                          ? rtd_bus_override
                          : static_cast<I2cBus*>(&rtd_bus_)),
      rtd_(rtd_bus_active_, MakeSequentOptions(config)),
      click1_bus_(),
      click2_bus_(),
      click1_bus_active_(click1_bus_override != nullptr
                             ? click1_bus_override
                             : static_cast<SpiBus*>(&click1_bus_)),
      click2_bus_active_(click2_bus_override != nullptr
                             ? click2_bus_override
                             : static_cast<SpiBus*>(&click2_bus_)),
      click1_(click1_bus_active_, MakeMax31865Options(config, kClick1SpiDevice)),
      click2_(click2_bus_active_, MakeMax31865Options(config, kClick2SpiDevice)) {
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
  rtd_health_.state = rtd_bus_active_->available() ? ComponentState::kDiscovering
                                                   : ComponentState::kDisabled;
  if (!rtd_bus_active_->available()) rtd_health_.error = "I2C_UNAVAILABLE";

  // Same reasoning as the RTD card: no enable key, "configured off" is not
  // a state the clicks can be in (Max31865Loop always polls when both buses
  // are available, independent of resistance_source). DISABLED, not FAILED,
  // on a build host with no Linux SPI support.
  const bool clicks_available =
      click1_bus_active_->available() && click2_bus_active_->available();
  for (ComponentHealth& health : max31865_health_) {
    health.state = clicks_available ? ComponentState::kDiscovering
                                    : ComponentState::kDisabled;
    if (!clicks_available) health.error = "SPI_UNAVAILABLE";
  }
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
  if (rtd_bus_active_->available()) {
    rtd_thread_ = std::thread(&SensorManager::SequentRtdLoop, this);
  }
  if (click1_bus_active_->available() && click2_bus_active_->available()) {
    max31865_thread_ = std::thread(&SensorManager::Max31865Loop, this);
  }
}

void SensorManager::Stop() {
  if (!running_.exchange(false)) return;
  stop_cv_.notify_all();
  if (dps_thread_.joinable()) dps_thread_.join();
  if (ads_thread_.joinable()) ads_thread_.join();
  if (rtd_thread_.joinable()) rtd_thread_.join();
  if (max31865_thread_.joinable()) max31865_thread_.join();
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
  // The early return above and the owns_resistance guard in SequentRtdLoop
  // together mean only one of the two ever writes this vector, so the two
  // cannot actually race. Take cache_mu_ anyway: it keeps the invariant that
  // every access to sample_resistance_ohm_ is under cache_mu_, which is what
  // makes the ownership split checkable locally instead of by argument.
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

    // Bus-level health: did the I2C conversation itself succeed, independent
    // of whether every channel it returned is plausible. ReadSnapshot ANDs
    // this into i2c_ok_ so I2C_FAIL reflects a dead RTD card even though the
    // card contributes nothing to dps310/ads1115 validity.
    rtd_bus_ok_ = ok;

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      std::vector<bool> channel_valid(sample_cache_.size(), false);
      std::size_t valid_count = 0;
      // sample_resistance_ohm_ has two possible owners and they mean
      // different physical quantities on the same telemetry field. What the
      // card reports is the PT100 *element* resistance; what
      // NotePullCompleted decays is a model of the sample *material*
      // resistance, and that model owns the vector whenever
      // sensor.resistance_source is "simulated". Writing it from here in that
      // mode would clobber the decay every poll and put element ohms on the
      // RESISTANCE= wire field, where ground software expects material ohms.
      const bool owns_resistance =
          config_.sensors.resistance_source != "simulated";

      if (ok) {
        rtd_last_reading_ = reading;
        rtd_has_reading_ = true;
        for (std::size_t i = 0;
             i < sample_cache_.size() &&
             i < SequentRtdAdapter::kChannelCount;
             ++i) {
          if (reading.channel_valid[i]) {
            sample_cache_[i] = {reading.temperature_c[i], true, true, now};
            if (owns_resistance) {
              sample_resistance_ohm_[i] = reading.resistance_ohm[i];
            }
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

      // Only sample_temp_ok_ is published from here. ReadSnapshot recomputes
      // i2c_ok_ from the DPS310/ADS1115 validity and resistance_ok_ from
      // sensor.resistance_source on every control tick, so writing them here
      // as well would be a dead store that reads like a second opinion.
      sample_temp_ok_ = HeatedChannelsValid(config_, channel_valid);
    }
    if (WaitForPoll(config_.sensors.sequent_rtd_poll_ms)) break;
  }
}

// Polls both MAX31865 clicks every cycle, independent of resistance_source
// -- exactly the SequentRtdLoop shape (see its owns_resistance comment):
// this worker always runs and always publishes MAX31865_1/2 health so
// CHECK MAX31865 and ComponentSummary stay live no matter which
// resistance_source is selected, but only WRITES sample_resistance_ohm_
// when max31865_click is actually the configured source.
void SensorManager::Max31865Loop() {
  while (running_.load()) {
    Max31865Adapter::Reading readings[2];
    std::string errors[2];
    bool ok[2] = {false, false};
    {
      std::lock_guard<std::mutex> io_lock(clicks_io_mu_);
      ok[0] = click1_.ReadOneShot(&readings[0], &errors[0]);
      ok[1] = click2_.ReadOneShot(&readings[1], &errors[1]);
    }

    // Bus-level health: did each click's one-shot CONVERSATION itself
    // succeed. Saturation is a valid measurement of an out-of-range
    // specimen (bus healthy, channel invalid) and must never key this --
    // only a call failure (ok[n] == false) does. This is keyed off the
    // ReadOneShot() return value, never off readings[n].valid.
    clicks_bus_ok_ = ok[0] && ok[1];

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      // Mirrors SequentRtdLoop's owns_resistance guard: sample_resistance_ohm_
      // has more than one possible writer, and this worker owns it only when
      // max31865_click is the selected source. It still runs and publishes
      // click health unconditionally (see the function comment above).
      const bool owns_resistance =
          config_.sensors.resistance_source == "max31865_click";
      for (int click = 0; click < 2; ++click) {
        ComponentHealth& health = max31865_health_[click];
        if (ok[click]) {
          click_has_success_[click] = true;
          click_last_success_[click] = now;
          health.last_success_age_ms = 0;
          if (readings[click].out_of_range) {
            health.state = ComponentState::kDegraded;
            health.error = "OUT_OF_RANGE";
          } else {
            health.state = ComponentState::kOk;
            health.error = "NONE";
          }
          if (owns_resistance &&
              static_cast<std::size_t>(click) <
                  config_.sensors.max31865_sample_indices.size()) {
            const std::size_t index =
                config_.sensors.max31865_sample_indices[
                    static_cast<std::size_t>(click)];
            // 0.0 is the existing wire dash convention for "not valid" (see
            // the "disabled" branch in ReadSnapshot below): a saturated
            // reading writes 0.0 here rather than the diagnostic
            // resistance_ohm value, keeping an out-of-range specimen
            // indistinguishable on the wire from an unmonitored index.
            if (index < sample_resistance_ohm_.size()) {
              sample_resistance_ohm_[index] =
                  readings[click].valid ? readings[click].resistance_ohm
                                        : 0.0;
            }
          }
        } else {
          health.state =
              FailedState(click_has_success_[click], click_last_success_[click]);
          health.error = errors[click].empty() ? "NO_RESPONSE" : errors[click];
          health.last_success_age_ms =
              AgeMs(click_last_success_[click], click_has_success_[click]);
        }
      }
    }
    if (WaitForPoll(config_.sensors.max31865_poll_ms)) break;
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
  snapshot.sequent_rtd = {ComponentState::kOk, "SIMULATED", 0};
  snapshot.simulated = true;
  i2c_ok_ = sample_temp_ok_ = uv_ok_ = true;
  rtd_bus_ok_ = true;
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
    snapshot.dps310 = dps_health_;
    snapshot.ads1115 = ads_health_;
    snapshot.sequent_rtd = rtd_health_;
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
    //
    // rtd_bus_ok_ is AND-ed in alongside the DPS310/ADS1115 contribution so
    // I2C_FAIL asserts on a dead RTD card even when DPS310/ADS1115 are
    // disabled and would otherwise report the bus healthy by omission. It is
    // bus-level ("did Probe/ReadAll succeed"), not per-channel plausibility,
    // so a card that answers but has one open sensor still reports I2C_OK;
    // per-channel detail stays on sample_temp_valid and SEQUENT_RTD.
    i2c_ok_ =
        (!config_.sensors.dps310_enabled ||
         (snapshot.ambient_temp_valid && snapshot.ambient_pressure_valid)) &&
        (!config_.sensors.ads1115_enabled || snapshot.uv_valid) &&
        rtd_bus_ok_.load();
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
    } else if (config_.sensors.resistance_source == "max31865_click") {
      // The v3-shipped default. Max31865Loop is the writer of
      // sample_resistance_ohm_ here, so health is bus-level: clicks_bus_ok_
      // tracks whether the last one-shot CONVERSATION with each click
      // succeeded, not whether the specimen's resistance happened to land
      // in range. A saturated/out-of-range specimen is a valid measurement
      // (bus healthy, channel invalid) and must not drag resistance_ok()
      // down -- that is the whole reason this is keyed off ReadOneShot's
      // call result and never off Reading.valid.
      resistance_ok_ = clicks_bus_ok_.load();
      snapshot.sample_resistance_ohm = sample_resistance_ohm_;
    } else if (config_.sensors.resistance_source == "sequent_rtd") {
      // Pre-v3 default, still accepted. SequentRtdLoop is the writer of
      // sample_resistance_ohm_ here, so health is bus-level: the numbers are
      // real element resistances exactly when the card conversation works.
      // Per-channel plausibility (open sensor, out-of-window resistance,
      // CVD cross-check) already flows out on sample_temp_valid and
      // SEQUENT_RTD, so folding it in here would only make one broken
      // channel blank the other seven.
      resistance_ok_ = rtd_bus_ok_.load();
      snapshot.sample_resistance_ohm = sample_resistance_ohm_;
    } else if (ina_ != nullptr && ina_->healthy()) {
      // Unreachable for any value config.cpp accepts today. It survives
      // because `sensor.resistance_source` still takes legacy labels from
      // fielded INIs, and Ina3221Adapter is still a compiled stub: if a
      // future value routes back through it, this is the branch that answers.
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

  Max31865Adapter::Reading click_readings[2];
  std::string click_errors[2] = {"SKIPPED", "SKIPPED"};
  bool click_ok[2] = {false, false};
  auto check_max31865 = [&]() {
    // A CHECK is an on-demand full conversation, same as check_rtd below:
    // a real one-shot conversion on each click, not a cached health read.
    std::lock_guard<std::mutex> lock(clicks_io_mu_);
    if (!click1_bus_active_->available() ||
        !click2_bus_active_->available()) {
      click_errors[0] = click_errors[1] = "SPI_UNAVAILABLE";
      return false;
    }
    click_ok[0] = click1_.ReadOneShot(&click_readings[0], &click_errors[0]);
    click_ok[1] = click2_.ReadOneShot(&click_readings[1], &click_errors[1]);
    return click_ok[0] && click_ok[1];
  };

  std::string rtd_error = "SKIPPED";
  SequentRtdAdapter::Identity identity;
  SequentRtdAdapter::Reading reading;
  int rtd_address = 0;
  bool rtd_burst = false;
  auto check_rtd = [&]() {
    // Nothing below this lambda may touch rtd_ directly. SequentRtdLoop
    // writes the adapter's burst_mode_ from the worker thread, inside
    // ReadAll, under this same mutex; reading it from the reply-formatting
    // code afterwards would be a race on a plain bool. So everything the
    // reply needs is captured into locals here, under the lock, and only the
    // locals are used later. address() reads immutable options_ and would be
    // safe unlocked, but it is captured the same way to keep the rule simple.
    std::lock_guard<std::mutex> lock(rtd_io_mu_);
    rtd_address = rtd_.address();
    rtd_burst = rtd_.burst_mode();
    if (!rtd_bus_active_->available()) {
      rtd_error = "I2C_UNAVAILABLE";
      return false;
    }
    rtd_error.clear();
    // A CHECK is an on-demand full conversation: probe first so the reply
    // carries a freshly-read identity, then read every channel.
    bool ok = rtd_.Probe(&identity, &rtd_error);
    if (ok) {
      ok = rtd_.ReadAll(&reading, &rtd_error);
      if (ok) rtd_identity_ = identity;
    }
    // ReadAll latches burst_mode_ off permanently if the firmware refuses a
    // 32-byte read, so re-capture it after the conversation rather than
    // reporting the value from before it.
    rtd_burst = rtd_.burst_mode();
    // Keep the worker's probe state consistent with what we just observed,
    // so a CHECK never leaves the loop trusting a card that just failed.
    rtd_probed_ = ok;
    if (ok && rtd_error.empty()) rtd_error = "NONE";
    if (!ok && rtd_error.empty()) rtd_error = "UNKNOWN";
    return ok;
  };

  const bool dps_requested = component == "ALL" || component == "DPS310";
  const bool ads_requested = component == "ALL" || component == "ADS1115";
  // SEQUENT_RTD is the current name (it is what COMPONENT_STATE puts on the
  // wire); DAQ132M and RTD_CLICK survive as request aliases only. Both
  // legacy acquisition paths were replaced by the one Sequent card, and
  // system_controller's CHECK whitelist still accepts all three names, so
  // routing them here keeps CHECK DAQ132M from silently succeeding against
  // nothing.
  const bool rtd_requested = component == "ALL" ||
                             component == "SEQUENT_RTD" ||
                             component == "RTD_CLICK" ||
                             component == "DAQ132M";
  const bool max31865_requested =
      component == "ALL" || component == "MAX31865";
  const bool dps_ok = !dps_requested || check_dps();
  const bool ads_ok = !ads_requested || check_ads();
  const bool rtd_ok = !rtd_requested || check_rtd();
  const bool max31865_ok = !max31865_requested || check_max31865();
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
      oss << ";sequent_rtd_addr=0x" << std::hex << rtd_address << std::dec
          << ";sequent_rtd_burst=" << (rtd_burst ? "1" : "0");
      if (rtd_ok) {
        AppendSequentIdentity(&oss, identity);
        AppendSequentDiagnostics(&oss, reading);
      }
    }
    oss << ";max31865_1=" << (!max31865_requested
                                  ? "SKIPPED"
                                  : (click_ok[0] ? "OK" : "FAIL"))
        << ";max31865_1_error="
        << (!max31865_requested ? "SKIPPED" : click_errors[0])
        << ";max31865_2=" << (!max31865_requested
                                  ? "SKIPPED"
                                  : (click_ok[1] ? "OK" : "FAIL"))
        << ";max31865_2_error="
        << (!max31865_requested ? "SKIPPED" : click_errors[1]);
    *details = oss.str();
  }
  return dps_ok && ads_ok && rtd_ok && max31865_ok;
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
  const ComponentHealth click1_health = max31865_health_[0];
  const ComponentHealth click2_health = max31865_health_[1];
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
  // max31865_health_ is not re-derived here either, same reasoning as
  // rtd_health_ just above: Max31865Loop already folded staleness in.
  oss << ";max31865_1=" << ToString(click1_health.state)
      << ";max31865_1_error=" << click1_health.error
      << ";max31865_1_age_ms=" << click1_health.last_success_age_ms
      << ";max31865_2=" << ToString(click2_health.state)
      << ";max31865_2_error=" << click2_health.error
      << ";max31865_2_age_ms=" << click2_health.last_success_age_ms;
  oss << ";sample_valid_channels=" << sample_valid_channels
      << ";heated_channels_ok="
      << (HeatedChannelsValid(config_, channel_valid) ? "1" : "0")
      << ";simulated=0";
  return oss.str();
}

}  // namespace coatheal
