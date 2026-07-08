#include "coatheal/sensor_manager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
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
      simulated_(config.runtime.use_simulated_sensors) {
  if (config_.sensors.resistance_source != "simulated") {
    std::fill(sample_resistance_ohm_.begin(), sample_resistance_ohm_.end(), 0.0);
  }
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

bool SensorManager::ReadDps310(double* temp_c, double* pressure_mbar) {
#if COATHEAL_HAS_LINUX_SENSOR_IO
  const int fd = OpenI2c(config_.sensors.dps310_i2c_addr);
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
  (void)temp_c;
  (void)pressure_mbar;
  return false;
#endif
}

bool SensorManager::ReadAds1115(double* voltage) {
#if COATHEAL_HAS_LINUX_SENSOR_IO
  const int fd = OpenI2c(config_.sensors.ads1115_i2c_addr);
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
  const int fd = ::open(config_.sensors.daq132m_device.c_str(),
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
    (*temperatures)[i] = value;
    (*valid)[i] = std::isfinite(value) && value >= -250.0 && value <= 850.0;
  }
  return std::all_of(valid->begin(), valid->end(), [](bool v) { return v; });
#else
  return false;
#endif
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
    snapshot.ambient_temp_valid =
        snapshot.ambient_pressure_valid =
            ReadDps310(&snapshot.ambient_temp_c,
                       &snapshot.ambient_pressure_mbar);
    snapshot.uv_valid = ReadAds1115(&snapshot.uv);
    snapshot.sample_temps_valid =
        ReadDaq132m(&snapshot.sample_temps_c, &snapshot.sample_temp_valid);
    i2c_ok_ = snapshot.ambient_temp_valid &&
              snapshot.ambient_pressure_valid && snapshot.uv_valid;
    rs485_ok_ = snapshot.sample_temps_valid;
    sample_temp_ok_ = snapshot.sample_temps_valid;
    uv_ok_ = snapshot.uv_valid;
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
  if (i2c_ != nullptr) i2c_->set_healthy(i2c_ok_);
  return snapshot;
}

std::string SensorManager::ActiveCheck() {
  double temp = 0.0;
  double pressure = 0.0;
  double uv = 0.0;
  std::vector<double> samples;
  std::vector<bool> valid;
  const bool dps = simulated_ || ReadDps310(&temp, &pressure);
  const bool ads = simulated_ || ReadAds1115(&uv);
  const bool daq = simulated_ || ReadDaq132m(&samples, &valid);
  i2c_ok_ = dps && ads;
  uv_ok_ = ads;
  rs485_ok_ = daq;
  sample_temp_ok_ = daq;
  if (i2c_ != nullptr) i2c_->set_healthy(i2c_ok_);
  std::ostringstream oss;
  oss << "dps310=" << (dps ? "OK" : "FAIL")
      << ";ads1115=" << (ads ? "OK" : "FAIL")
      << ";daq132m=" << (daq ? "OK" : "FAIL")
      << ";simulated=" << (simulated_ ? "1" : "0");
  return oss.str();
}

}  // namespace coatheal
