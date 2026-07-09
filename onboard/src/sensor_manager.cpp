#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>

#if defined(__linux__) && __has_include(<linux/i2c-dev.h>)
#define COATHEAL_HAS_LINUX_SENSOR_IO 1
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <termios.h>
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

std::uint16_t ModbusCrc(const std::uint8_t* data, std::size_t size) {
  std::uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const bool lsb = (crc & 1U) != 0U;
      crc >>= 1U;
      if (lsb) crc ^= 0xA001U;
    }
  }
  return crc;
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

speed_t BaudConstant(int baud) {
  switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return 0;
  }
}

std::string DiscoverSerialDevice(const std::string& configured,
                                 bool auto_discover) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!configured.empty() && configured != "auto" &&
      fs::exists(configured, ec)) {
    return configured;
  }
  if (!auto_discover) return {};

  std::set<std::string> stable_candidates;
  const fs::path by_id("/dev/serial/by-id");
  if (fs::is_directory(by_id, ec)) {
    for (const auto& entry : fs::directory_iterator(by_id, ec)) {
      stable_candidates.insert(entry.path().string());
    }
  }
  if (stable_candidates.size() == 1) return *stable_candidates.begin();
  if (stable_candidates.size() > 1) return {};

  std::set<std::string> candidates;
  for (int i = 0; i < 8; ++i) {
    const std::string usb = "/dev/ttyUSB" + std::to_string(i);
    const std::string acm = "/dev/ttyACM" + std::to_string(i);
    if (fs::exists(usb, ec)) candidates.insert(usb);
    if (fs::exists(acm, ec)) candidates.insert(acm);
  }
  return candidates.size() == 1 ? *candidates.begin() : std::string{};
}
#else
std::string DiscoverSerialDevice(const std::string&, bool) {
  return {};
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
      sample_cache_(config.hardware.sample_count) {
  if (config_.sensors.resistance_source != "simulated") {
    std::fill(sample_resistance_ohm_.begin(), sample_resistance_ohm_.end(), 0.0);
  }
  dps_health_.state = config_.sensors.dps310_enabled
                          ? ComponentState::kDiscovering
                          : ComponentState::kDisabled;
  ads_health_.state = config_.sensors.ads1115_enabled
                          ? ComponentState::kDiscovering
                          : ComponentState::kDisabled;
  daq_health_.state = config_.sensors.daq132m_enabled
                          ? ComponentState::kDiscovering
                          : ComponentState::kDisabled;
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
  if (config_.sensors.daq132m_enabled) {
    daq_thread_ = std::thread(&SensorManager::DaqLoop, this);
  }
}

void SensorManager::Stop() {
  if (!running_.exchange(false)) return;
  stop_cv_.notify_all();
  if (dps_thread_.joinable()) dps_thread_.join();
  if (ads_thread_.joinable()) ads_thread_.join();
  if (daq_thread_.joinable()) daq_thread_.join();
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
  const std::size_t end = motor_id == 0 ? 4U : sample_resistance_ohm_.size();
  if (motor_id != 0 && motor_id != 1) return;
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

bool SensorManager::ReadDaq132m(std::vector<double>* temperatures,
                                std::vector<bool>* valid) {
  temperatures->assign(config_.hardware.sample_count, 0.0);
  valid->assign(config_.hardware.sample_count, false);
#if COATHEAL_HAS_LINUX_SENSOR_IO
  const speed_t baud = BaudConstant(config_.sensors.daq132m_baud);
  if (baud == 0) return false;
  const std::string device =
      resolved_daq_device_.empty() ? config_.sensors.daq132m_device
                                   : resolved_daq_device_;
  const int fd = ::open(device.c_str(),
                        O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) return false;

  termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    ::close(fd);
    return false;
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, baud);
  cfsetospeed(&tty, baud);
  tty.c_cflag |= CLOCAL | CREAD;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= config_.sensors.daq132m_data_bits == 7 ? CS7 : CS8;
  tty.c_cflag &= ~(PARENB | PARODD);
  if (config_.sensors.daq132m_parity == "E") {
    tty.c_cflag |= PARENB;
  } else if (config_.sensors.daq132m_parity == "O") {
    tty.c_cflag |= PARENB | PARODD;
  }
  if (config_.sensors.daq132m_stop_bits == 2) {
    tty.c_cflag |= CSTOPB;
  } else {
    tty.c_cflag &= ~CSTOPB;
  }
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 5;
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    ::close(fd);
    return false;
  }
  tcflush(fd, TCIOFLUSH);

  std::array<std::uint8_t, 8> request{};
  request[0] = static_cast<std::uint8_t>(config_.sensors.daq132m_slave_id);
  request[1] = static_cast<std::uint8_t>(config_.sensors.daq132m_function_code);
  request[2] = static_cast<std::uint8_t>(
      (config_.sensors.daq132m_register_base >> 8) & 0xFF);
  request[3] = static_cast<std::uint8_t>(
      config_.sensors.daq132m_register_base & 0xFF);
  request[4] = static_cast<std::uint8_t>(
      (config_.sensors.daq132m_register_count >> 8) & 0xFF);
  request[5] = static_cast<std::uint8_t>(
      config_.sensors.daq132m_register_count & 0xFF);
  const std::uint16_t request_crc = ModbusCrc(request.data(), 6);
  request[6] = static_cast<std::uint8_t>(request_crc & 0xFFU);
  request[7] = static_cast<std::uint8_t>(request_crc >> 8U);
  if (::write(fd, request.data(), request.size()) !=
      static_cast<ssize_t>(request.size())) {
    ::close(fd);
    return false;
  }
  tcdrain(fd);

  const std::size_t expected =
      5U + 2U * static_cast<std::size_t>(config_.sensors.daq132m_register_count);
  std::vector<std::uint8_t> response(expected);
  std::size_t received = 0;
  while (received < expected) {
    const ssize_t n = ::read(fd, response.data() + received, expected - received);
    if (n < 0) {
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    received += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (received != expected || response[0] != request[0] ||
      response[1] != request[1] ||
      response[2] != 2U * config_.sensors.daq132m_register_count) {
    return false;
  }
  const std::uint16_t response_crc =
      static_cast<std::uint16_t>(response[expected - 2]) |
      (static_cast<std::uint16_t>(response[expected - 1]) << 8U);
  if (ModbusCrc(response.data(), expected - 2) != response_crc) return false;

  for (std::size_t i = 0; i < config_.hardware.sample_count; ++i) {
    const std::size_t offset = 3U + i * 2U;
    const std::int16_t counts = static_cast<std::int16_t>(
        (static_cast<std::uint16_t>(response[offset]) << 8U) |
        response[offset + 1]);
    const double value =
        counts * config_.sensors.daq132m_c_per_count +
        config_.sensors.daq132m_c_offset;
    const bool enabled =
        std::find(config_.sensors.daq132m_enabled_channels.begin(),
                  config_.sensors.daq132m_enabled_channels.end(), i) !=
        config_.sensors.daq132m_enabled_channels.end();
    (*temperatures)[i] = value;
    (*valid)[i] = enabled && std::isfinite(value) &&
                  value >= -250.0 && value <= 850.0;
  }
  // A valid Modbus frame means the RS485 path is healthy even when one or
  // more DAQ inputs are intentionally unwired. Per-channel validity is kept
  // in `valid` so thermal control can independently inhibit those heaters.
  return true;
#else
  return false;
#endif
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

void SensorManager::DaqLoop() {
  while (running_.load()) {
    const std::string discovered = DiscoverSerialDevice(
        config_.sensors.daq132m_device,
        config_.sensors.daq132m_auto_discover);
    std::vector<double> temperatures;
    std::vector<bool> valid;
    bool ok = false;
    {
      std::lock_guard<std::mutex> io_lock(daq_io_mu_);
      resolved_daq_device_ = discovered;
      if (!resolved_daq_device_.empty()) {
        ok = ReadDaq132m(&temperatures, &valid);
      }
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      std::size_t valid_count = 0;
      if (ok) {
        for (std::size_t i = 0;
             i < sample_cache_.size() && i < temperatures.size(); ++i) {
          sample_cache_[i].valid = i < valid.size() && valid[i];
          if (sample_cache_[i].valid) {
            sample_cache_[i] = {temperatures[i], true, true, now};
            ++valid_count;
          }
        }
        const std::size_t enabled_count =
            config_.sensors.daq132m_enabled_channels.size();
        daq_health_.state =
            valid_count == enabled_count && enabled_count > 0
                ? ComponentState::kOk
                : ComponentState::kDegraded;
        daq_health_.error =
            valid_count == 0 ? "NO_VALID_CHANNELS"
                             : (valid_count < enabled_count
                                    ? "PARTIAL_CHANNELS"
                                    : "NONE");
        daq_health_.last_success_age_ms = 0;
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
        daq_health_.state = FailedState(any_previous, newest);
        daq_health_.error = discovered.empty()
                                ? "DEVICE_NOT_FOUND_OR_AMBIGUOUS"
                                : "MODBUS_NO_VALID_FRAME";
        daq_health_.last_success_age_ms = AgeMs(newest, any_previous);
      }
      rs485_ok_ = ok;
      sample_temp_ok_ = valid_count > 0;
    }
    if (WaitForPoll(config_.sensors.daq132m_poll_ms)) break;
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
  snapshot.daq132m = {ComponentState::kOk, "SIMULATED", 0};
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
    snapshot.daq132m = daq_health_;
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
    std::size_t enabled_with_value = 0;
    std::size_t fresh_enabled = 0;
    std::int64_t newest_enabled_age = -1;
    for (const std::size_t channel :
         config_.sensors.daq132m_enabled_channels) {
      if (channel >= sample_cache_.size() ||
          !sample_cache_[channel].has_value) {
        continue;
      }
      ++enabled_with_value;
      const std::int64_t age = snapshot.sample_temp_age_ms[channel];
      if (newest_enabled_age < 0 || age < newest_enabled_age) {
        newest_enabled_age = age;
      }
      if (snapshot.sample_temp_valid[channel]) ++fresh_enabled;
    }
    snapshot.daq132m.last_success_age_ms = newest_enabled_age;
    if (config_.sensors.daq132m_enabled) {
      const std::size_t expected =
          config_.sensors.daq132m_enabled_channels.size();
      if (fresh_enabled == expected && expected > 0) {
        snapshot.daq132m.state = ComponentState::kOk;
      } else if (fresh_enabled > 0) {
        snapshot.daq132m.state = ComponentState::kDegraded;
      } else if (enabled_with_value > 0) {
        snapshot.daq132m.state =
            newest_enabled_age >= config_.sensors.stale_after_ms
                ? ComponentState::kStale
                : ComponentState::kDegraded;
      }
    }
    i2c_ok_ =
        (!config_.sensors.dps310_enabled ||
         (snapshot.ambient_temp_valid && snapshot.ambient_pressure_valid)) &&
        (!config_.sensors.ads1115_enabled || snapshot.uv_valid);
    uv_ok_ = snapshot.uv_valid;
    sample_temp_ok_ = fresh_enabled > 0;
  }

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

  auto check_daq = [&]() {
    if (!config_.sensors.daq132m_enabled) return true;
#if COATHEAL_HAS_LINUX_SENSOR_IO
    const std::string discovered = DiscoverSerialDevice(
        config_.sensors.daq132m_device,
        config_.sensors.daq132m_auto_discover);
    std::vector<double> temperatures;
    std::vector<bool> valid;
    bool ok = false;
    std::lock_guard<std::mutex> lock(daq_io_mu_);
    const std::string previous = resolved_daq_device_;
    resolved_daq_device_ = discovered;
    if (!resolved_daq_device_.empty()) {
      ok = ReadDaq132m(&temperatures, &valid);
    }
    if (!ok && resolved_daq_device_.empty()) resolved_daq_device_ = previous;
    return ok;
#else
    return false;
#endif
  };

  const bool dps_requested = component == "ALL" || component == "DPS310";
  const bool ads_requested = component == "ALL" || component == "ADS1115";
  const bool daq_requested = component == "ALL" || component == "DAQ132M";
  const bool dps_ok = !dps_requested || check_dps();
  const bool ads_ok = !ads_requested || check_ads();
  const bool daq_ok = !daq_requested || check_daq();
  if (details != nullptr) {
    std::ostringstream oss;
    oss << "dps310=" << (!dps_requested ? "SKIPPED"
                                        : (dps_ok ? "OK" : "FAIL"))
        << ";ads1115=" << (!ads_requested ? "SKIPPED"
                                          : (ads_ok ? "OK" : "FAIL"))
        << ";daq132m=" << (!daq_requested ? "SKIPPED"
                                          : (daq_ok ? "OK" : "FAIL"));
    *details = oss.str();
  }
  return dps_ok && ads_ok && daq_ok;
}

std::string SensorManager::ComponentSummary() const {
  if (simulated_) {
    return "dps310=OK;ads1115=OK;daq132m=OK;daq_valid_channels=" +
           std::to_string(config_.hardware.sample_count) + ";simulated=1";
  }
  std::string daq_device;
  {
    std::lock_guard<std::mutex> lock(daq_io_mu_);
    daq_device = resolved_daq_device_;
  }
  std::lock_guard<std::mutex> lock(cache_mu_);
  ComponentHealth dps = dps_health_;
  ComponentHealth ads = ads_health_;
  ComponentHealth daq = daq_health_;
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
  const std::size_t valid_channels =
      static_cast<std::size_t>(std::count_if(
          sample_cache_.begin(), sample_cache_.end(),
          [this](const ScalarCache& sample) {
            return sample.valid &&
                   AgeMs(sample.last_success, sample.has_value) >= 0 &&
                   AgeMs(sample.last_success, sample.has_value) <
                       config_.sensors.stale_after_ms;
          }));
  std::int64_t newest_daq_age = -1;
  for (const std::size_t channel :
       config_.sensors.daq132m_enabled_channels) {
    if (channel >= sample_cache_.size() ||
        !sample_cache_[channel].has_value) {
      continue;
    }
    const std::int64_t age =
        AgeMs(sample_cache_[channel].last_success, true);
    if (newest_daq_age < 0 || age < newest_daq_age) newest_daq_age = age;
  }
  daq.last_success_age_ms = newest_daq_age;
  if (config_.sensors.daq132m_enabled && valid_channels == 0 &&
      newest_daq_age >= config_.sensors.stale_after_ms) {
    daq.state = ComponentState::kStale;
  }
  std::ostringstream oss;
  oss << "dps310=" << ToString(dps.state)
      << ";dps310_error=" << dps.error
      << ";dps310_age_ms=" << dps.last_success_age_ms
      << ";ads1115=" << ToString(ads.state)
      << ";ads1115_error=" << ads.error
      << ";ads1115_age_ms=" << ads.last_success_age_ms
      << ";daq132m=" << ToString(daq.state)
      << ";daq132m_error=" << daq.error
      << ";daq132m_age_ms=" << daq.last_success_age_ms
      << ";daq_valid_channels=" << valid_channels
      << ";daq_device="
      << (daq_device.empty() ? "-" : daq_device)
      << ";simulated=0";
  return oss.str();
}

}  // namespace coatheal
