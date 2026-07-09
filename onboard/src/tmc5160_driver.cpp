#include "coatheal/tmc5160_driver.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>

#include "coatheal/hal/gpio_output.hpp"

#if defined(__linux__) && __has_include(<linux/spi/spidev.h>)
#  define COATHEAL_HAS_SPIDEV 1
#  include <fcntl.h>
#  include <linux/spi/spidev.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#else
#  define COATHEAL_HAS_SPIDEV 0
#endif

namespace coatheal {

namespace {

constexpr std::uint8_t kRegGCONF      = 0x00;
constexpr std::uint8_t kRegGSTAT      = 0x01;
constexpr std::uint8_t kRegIOIN       = 0x04;
constexpr std::uint8_t kRegIHOLD_IRUN = 0x10;
constexpr std::uint8_t kRegTPOWERDOWN = 0x11;
constexpr std::uint8_t kRegCHOPCONF   = 0x6C;
constexpr std::uint8_t kRegDRVSTATUS  = 0x6F;
constexpr std::uint8_t kRegPWMCONF    = 0x70;
constexpr std::uint8_t kWriteBit = 0x80;
constexpr std::uint8_t kExpectedVersion = 0x30;

}  // namespace

Tmc5160Driver::Tmc5160Driver(const Tmc5160Config& cfg)
    : cfg_(cfg), microstep_(cfg.microstep) {
  const bool gpio_ok = OpenGpio();
  const bool spi_ok = OpenSpi() && Reinitialize();
  healthy_ = gpio_ok && spi_ok;
}

Tmc5160Driver::~Tmc5160Driver() {
  Enable(false);
  CloseGpio();
  CloseSpi();
}

std::uint32_t Tmc5160Driver::EncodeGconf(bool stealth_chop) {
  return stealth_chop ? 0x00000004u : 0x00000000u;
}

std::uint32_t Tmc5160Driver::EncodeIholdIrun(double run_a_rms,
                                             double hold_frac,
                                             double sense_resistor_ohm) {
  const double rsense = std::max(0.001, sense_resistor_ohm);
  const double irun_f =
      std::round(run_a_rms * 32.0 * std::sqrt(2.0) *
                 (rsense + 0.02) / 0.325 - 1.0);
  const int irun = static_cast<int>(std::clamp(irun_f, 0.0, 31.0));
  int ihold = static_cast<int>(
      std::round(irun * std::clamp(hold_frac, 0.0, 1.0)));
  if (ihold > irun) ihold = irun;
  return (static_cast<std::uint32_t>(6) << 16) |
         (static_cast<std::uint32_t>(irun & 0x1F) << 8) |
         static_cast<std::uint32_t>(ihold & 0x1F);
}

std::uint32_t Tmc5160Driver::EncodeChopconf(int microstep_divisor) {
  int mres = 6;
  switch (microstep_divisor) {
    case 1:   mres = 8; break;
    case 2:   mres = 7; break;
    case 4:   mres = 6; break;
    case 8:   mres = 5; break;
    case 16:  mres = 4; break;
    case 32:  mres = 3; break;
    case 64:  mres = 2; break;
    case 128: mres = 1; break;
    case 256: mres = 0; break;
    default:  mres = 6; break;
  }
  std::uint32_t chopconf = 0;
  chopconf |= static_cast<std::uint32_t>(mres & 0x0F) << 24;
  chopconf |= static_cast<std::uint32_t>(0x2) << 15;
  chopconf |= static_cast<std::uint32_t>(0x1) << 7;
  chopconf |= static_cast<std::uint32_t>(0x4) << 4;
  chopconf |= static_cast<std::uint32_t>(0x3);
  return chopconf;
}

bool Tmc5160Driver::OpenSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) return true;
  const int fd = ::open(cfg_.spi_device.c_str(), O_RDWR);
  if (fd < 0) {
    std::cerr << "[tmc5160] open(" << cfg_.spi_device
              << ") failed: " << std::strerror(errno) << '\n';
    return false;
  }
  std::uint8_t mode = SPI_MODE_3 | SPI_NO_CS;
  std::uint8_t bits = 8;
  std::uint32_t speed = cfg_.spi_speed_hz;
  if (::ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ::ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ::ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
    std::cerr << "[tmc5160] SPI ioctl setup failed on " << cfg_.spi_device
              << ": " << std::strerror(errno) << '\n';
    ::close(fd);
    return false;
  }
  spi_fd_ = fd;
  return true;
#else
  std::cerr << "[tmc5160] spidev not available on this platform\n";
  return false;
#endif
}

bool Tmc5160Driver::OpenGpio() {
#ifdef COATHEAL_HAS_LIBGPIOD
  const int disabled = cfg_.enable_active_low ? 1 : 0;
  cs_handle_ =
      RequestGpioOutput(cfg_.gpio_chip, cfg_.cs_line, "coatheal-tmc-cs", true);
  step_handle_ = RequestGpioOutput(
      cfg_.gpio_chip, cfg_.step_line, "coatheal-tmc-step", false);
  dir_handle_ = RequestGpioOutput(
      cfg_.gpio_chip, cfg_.dir_line, "coatheal-tmc-dir", false);
  enable_handle_ = RequestGpioOutput(
      cfg_.gpio_chip, cfg_.enable_line, "coatheal-tmc-enable", disabled != 0);
  if (cs_handle_ == nullptr || step_handle_ == nullptr ||
      dir_handle_ == nullptr || enable_handle_ == nullptr) {
    CloseGpio();
    return false;
  }
  gpio_healthy_ = true;
  return true;
#else
  return false;
#endif
}

void Tmc5160Driver::CloseSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) {
    ::close(spi_fd_);
    spi_fd_ = -1;
  }
#endif
}

void Tmc5160Driver::CloseGpio() {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (cs_handle_ != nullptr) {
    SetGpioOutput(static_cast<GpioOutput*>(cs_handle_), true);
  }
  if (step_handle_ != nullptr) {
    SetGpioOutput(static_cast<GpioOutput*>(step_handle_), false);
  }
  ReleaseGpioOutput(static_cast<GpioOutput*>(cs_handle_));
  ReleaseGpioOutput(static_cast<GpioOutput*>(step_handle_));
  ReleaseGpioOutput(static_cast<GpioOutput*>(dir_handle_));
  ReleaseGpioOutput(static_cast<GpioOutput*>(enable_handle_));
  cs_handle_ = nullptr;
  step_handle_ = nullptr;
  dir_handle_ = nullptr;
  enable_handle_ = nullptr;
#endif
  gpio_healthy_ = false;
}

bool Tmc5160Driver::WriteRegister(std::uint8_t address, std::uint32_t value) {
#if COATHEAL_HAS_SPIDEV
  std::uint8_t tx[5];
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  tx[0] = static_cast<std::uint8_t>(address | kWriteBit);
  tx[1] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  tx[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  tx[3] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  tx[4] = static_cast<std::uint8_t>(value & 0xFF);

  return Transfer(tx, rx);
#else
  (void)address;
  (void)value;
  return false;
#endif
}

bool Tmc5160Driver::Transfer(const std::uint8_t tx[5],
                             std::uint8_t rx[5]) {
#if COATHEAL_HAS_SPIDEV && defined(COATHEAL_HAS_LIBGPIOD)
  if (spi_fd_ < 0 || !gpio_healthy_ || cs_handle_ == nullptr) return false;
  struct spi_ioc_transfer xfer {};
  xfer.tx_buf = reinterpret_cast<__u64>(tx);
  xfer.rx_buf = reinterpret_cast<__u64>(rx);
  xfer.len = 5;
  xfer.speed_hz = cfg_.spi_speed_hz;
  xfer.bits_per_word = 8;
  if (!SetGpioOutput(static_cast<GpioOutput*>(cs_handle_), false)) {
    gpio_healthy_ = false;
    return false;
  }
  const int rc = ::ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &xfer);
  const bool released =
      SetGpioOutput(static_cast<GpioOutput*>(cs_handle_), true);
  if (rc < 0 || !released) {
    gpio_healthy_ = false;
    return false;
  }
  return true;
#else
  (void)tx;
  (void)rx;
  return false;
#endif
}

bool Tmc5160Driver::ReadRegister(std::uint8_t address,
                                 std::uint32_t* value) {
  if (value == nullptr) return false;
  std::uint8_t tx[5] = {
      static_cast<std::uint8_t>(address & 0x7FU), 0, 0, 0, 0};
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  if (!Transfer(tx, rx) || !Transfer(tx, rx)) return false;
  *value = (static_cast<std::uint32_t>(rx[1]) << 24U) |
           (static_cast<std::uint32_t>(rx[2]) << 16U) |
           (static_cast<std::uint32_t>(rx[3]) << 8U) |
           static_cast<std::uint32_t>(rx[4]);
  return true;
}

bool Tmc5160Driver::Reinitialize() {
  std::lock_guard<std::mutex> lock(io_mu_);
  const bool restore_enabled = enabled_;
  if (enable_handle_ != nullptr) {
    EnableUnlocked(false);
  }
  if (!gpio_healthy_) {
    CloseGpio();
    if (!OpenGpio()) {
      healthy_ = false;
      return false;
    }
  }
  if (spi_fd_ < 0 && !OpenSpi()) {
    healthy_ = false;
    return false;
  }

  std::uint32_t ioin = 0;
  if (!ReadRegister(kRegIOIN, &ioin) ||
      static_cast<std::uint8_t>(ioin >> 24U) != kExpectedVersion) {
    healthy_ = false;
    return false;
  }
  const std::uint32_t gconf = EncodeGconf(cfg_.stealth_chop);
  const std::uint32_t ihold_irun =
      EncodeIholdIrun(cfg_.run_current_a_rms, cfg_.hold_current_frac,
                      cfg_.sense_resistor_ohm);
  const std::uint32_t chopconf = EncodeChopconf(microstep_);
  const std::uint32_t pwmconf = 0xC10D0024u;
  const std::uint32_t tpowerdown = 0x0000000Au;

  bool ok = true;
  ok = WriteRegister(kRegGCONF, gconf) && ok;
  ok = WriteRegister(kRegIHOLD_IRUN, ihold_irun) && ok;
  ok = WriteRegister(kRegTPOWERDOWN, tpowerdown) && ok;
  ok = WriteRegister(kRegCHOPCONF, chopconf) && ok;
  ok = WriteRegister(kRegPWMCONF, pwmconf) && ok;
  std::uint32_t verify = 0;
  ok = ReadRegister(kRegGCONF, &verify) && verify == gconf && ok;
  ok = ReadRegister(kRegIHOLD_IRUN, &verify) &&
       (verify & 0x000F1F1FU) == (ihold_irun & 0x000F1F1FU) && ok;
  ok = ReadRegister(kRegCHOPCONF, &verify) && verify == chopconf && ok;
  std::uint32_t gstat = 0;
  std::uint32_t drv_status = 0;
  ok = ReadRegister(kRegGSTAT, &gstat) && ok;
  ok = ReadRegister(kRegDRVSTATUS, &drv_status) && ok;
  (void)drv_status;
  ok = (gstat & 0x00000002U) == 0U && ok;
  healthy_ = ok && gpio_healthy_;
  if (healthy_ && restore_enabled) {
    healthy_ = EnableUnlocked(true);
  }
  return healthy_;
}

bool Tmc5160Driver::Enable(bool enable) {
  std::lock_guard<std::mutex> lock(io_mu_);
  return EnableUnlocked(enable);
}

bool Tmc5160Driver::EnableUnlocked(bool enable) {
  if (!gpio_healthy_ || (enable && !healthy_)) return false;
#ifdef COATHEAL_HAS_LIBGPIOD
  const int value = enable ? (cfg_.enable_active_low ? 0 : 1)
                           : (cfg_.enable_active_low ? 1 : 0);
  if (!SetGpioOutput(static_cast<GpioOutput*>(enable_handle_), value != 0)) {
    gpio_healthy_ = false;
    healthy_ = false;
    return false;
  }
#endif
  enabled_ = enable;
  return true;
}

bool Tmc5160Driver::Step(bool direction_forward) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!healthy_ || !enabled_) return false;
  if (pulses_ > 0 && pulses_ % 256U == 0U) {
    std::uint32_t gstat = 0;
    std::uint32_t drv_status = 0;
    constexpr std::uint32_t kDriverFaultMask = 0x7F000000U;
    if (!ReadRegister(kRegGSTAT, &gstat) ||
        !ReadRegister(kRegDRVSTATUS, &drv_status) ||
        (gstat & 0x00000002U) != 0U ||
        (drv_status & kDriverFaultMask) != 0U) {
      healthy_ = false;
      EnableUnlocked(false);
      return false;
    }
  }
#ifdef COATHEAL_HAS_LIBGPIOD
  const bool physical_direction = direction_forward != cfg_.invert_direction;
  if (physical_direction != last_direction_forward_) {
    if (!SetGpioOutput(static_cast<GpioOutput*>(dir_handle_),
                       physical_direction)) {
      gpio_healthy_ = false;
      healthy_ = false;
      enabled_ = false;
      return false;
    }
    last_direction_forward_ = physical_direction;
    std::this_thread::sleep_for(std::chrono::microseconds(2));
  }
  if (!SetGpioOutput(static_cast<GpioOutput*>(step_handle_), true)) {
    gpio_healthy_ = false;
    healthy_ = false;
    SetGpioOutput(static_cast<GpioOutput*>(enable_handle_),
                  cfg_.enable_active_low);
    enabled_ = false;
    return false;
  }
  std::this_thread::sleep_for(
      std::chrono::microseconds(std::max(1, cfg_.pulse_high_us)));
  if (!SetGpioOutput(static_cast<GpioOutput*>(step_handle_), false)) {
    gpio_healthy_ = false;
    healthy_ = false;
    SetGpioOutput(static_cast<GpioOutput*>(enable_handle_),
                  cfg_.enable_active_low);
    enabled_ = false;
    return false;
  }
#endif
  ++pulses_;
  return true;
}

void Tmc5160Driver::SetMicrostep(int divisor) {
  if (divisor <= 0) return;
  std::lock_guard<std::mutex> lock(io_mu_);
  microstep_ = divisor;
  const std::uint32_t requested = EncodeChopconf(divisor);
  std::uint32_t verify = 0;
  if (!WriteRegister(kRegCHOPCONF, requested) ||
      !ReadRegister(kRegCHOPCONF, &verify) || verify != requested) {
    healthy_ = false;
  }
}

}  // namespace coatheal
