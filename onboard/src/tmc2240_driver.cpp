#include "coatheal/tmc2240_driver.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>

#include "coatheal/hal/gpio_output.hpp"
#include "coatheal/hal/spi_bus_lock.hpp"

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
constexpr std::uint8_t kRegDRVCONF    = 0x0A;
constexpr std::uint8_t kRegGLOBALSCALER = 0x0B;
constexpr std::uint8_t kRegIHOLD_IRUN = 0x10;
constexpr std::uint8_t kRegTPOWERDOWN = 0x11;
constexpr std::uint8_t kRegCHOPCONF   = 0x6C;
constexpr std::uint8_t kRegDRVSTATUS  = 0x6F;
constexpr std::uint8_t kRegPWMCONF    = 0x70;
constexpr std::uint8_t kWriteBit = 0x80;
constexpr std::uint8_t kExpectedVersion = 0x40;
constexpr std::uint32_t kDriverFaultMask =
    (1U << 28U) | (1U << 27U) | (1U << 26U) | (1U << 25U) |
    (1U << 13U) | (1U << 12U);
constexpr std::uint32_t kGlobalFaultMask = 0x0000001FU;

bool IsSupportedMicrostep(int divisor) {
  return divisor == 1 || divisor == 2 || divisor == 4 || divisor == 8 ||
         divisor == 16 || divisor == 32 || divisor == 64 ||
         divisor == 128 || divisor == 256;
}

}  // namespace

Tmc2240Driver::Tmc2240Driver(const Tmc2240Config& cfg)
    : cfg_(cfg), microstep_(cfg.microstep) {
  if (!IsSupportedMicrostep(cfg.microstep)) return;
  const bool gpio_ok = OpenGpio();
  const bool spi_ok = OpenSpi() && Reinitialize();
  healthy_ = gpio_ok && spi_ok;
}

Tmc2240Driver::~Tmc2240Driver() {
  Enable(false);
  CloseGpio();
  CloseSpi();
}

std::uint32_t Tmc2240Driver::EncodeGconf(bool stealth_chop) {
  return stealth_chop ? 0x00000004u : 0x00000000u;
}

bool Tmc2240Driver::CalculateCurrentSettings(
    double run_a_rms, double hold_frac, double requested_range_a_peak,
    Tmc2240CurrentSettings* settings) {
  if (settings == nullptr || !std::isfinite(run_a_rms) ||
      !std::isfinite(hold_frac) || !std::isfinite(requested_range_a_peak) ||
      run_a_rms <= 0.0 || hold_frac < 0.0 || hold_frac > 1.0 ||
      requested_range_a_peak < 0.0) {
    return false;
  }

  const double requested_peak = run_a_rms * std::sqrt(2.0);
  double range_peak = requested_range_a_peak;
  if (range_peak == 0.0) {
    if (requested_peak <= 1.0) {
      range_peak = 1.0;
    } else if (requested_peak <= 2.0) {
      range_peak = 2.0;
    } else if (requested_peak <= 3.0) {
      range_peak = 3.0;
    } else {
      return false;
    }
  }
  if ((range_peak != 1.0 && range_peak != 2.0 && range_peak != 3.0) ||
      requested_peak > range_peak) {
    return false;
  }

  const auto scaler = static_cast<int>(
      std::lround(requested_peak * 256.0 / range_peak));
  if (scaler < 32 || scaler > 256) {
    return false;
  }
  const int ihold = std::clamp(
      static_cast<int>(std::lround(32.0 * hold_frac)) - 1, 0, 31);
  settings->range_a_peak = range_peak;
  settings->range_code =
      range_peak == 1.0 ? 0U : (range_peak == 2.0 ? 1U : 2U);
  // The register encodes 256/full scale as zero.
  settings->global_scaler =
      scaler == 256 ? 0U : static_cast<std::uint8_t>(scaler);
  settings->ihold_irun =
      (static_cast<std::uint32_t>(4) << 24U) |
      (static_cast<std::uint32_t>(6) << 16U) |
      (static_cast<std::uint32_t>(31) << 8U) |
      static_cast<std::uint32_t>(ihold);
  return true;
}

std::uint32_t Tmc2240Driver::EncodeChopconf(int microstep_divisor) {
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
  chopconf |= static_cast<std::uint32_t>(0x4) << 4;
  chopconf |= static_cast<std::uint32_t>(0x3);
  return chopconf;
}

bool Tmc2240Driver::OpenSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) return true;
  const int fd = ::open(cfg_.spi_device.c_str(), O_RDWR);
  if (fd < 0) {
    std::cerr << "[tmc2240] open(" << cfg_.spi_device
              << ") failed: " << std::strerror(errno) << '\n';
    return false;
  }
  std::uint8_t mode = SPI_MODE_3 | SPI_NO_CS;
  std::uint8_t bits = 8;
  std::uint32_t speed = cfg_.spi_speed_hz;
  if (::ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ::ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ::ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
    std::cerr << "[tmc2240] SPI ioctl setup failed on " << cfg_.spi_device
              << ": " << std::strerror(errno) << '\n';
    ::close(fd);
    return false;
  }
  spi_fd_ = fd;
  return true;
#else
  std::cerr << "[tmc2240] spidev not available on this platform\n";
  return false;
#endif
}

bool Tmc2240Driver::OpenGpio() {
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
    std::cerr << "[tmc2240] GPIO request failed on " << cfg_.gpio_chip
              << " cs=" << cfg_.cs_line
              << " step=" << cfg_.step_line
              << " dir=" << cfg_.dir_line
              << " en=" << cfg_.enable_line << '\n';
    CloseGpio();
    return false;
  }
  last_direction_forward_ = false;
  gpio_healthy_ = true;
  return true;
#else
  std::cerr << "[tmc2240] libgpiod support not available in this build\n";
  return false;
#endif
}

void Tmc2240Driver::CloseSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) {
    ::close(spi_fd_);
    spi_fd_ = -1;
  }
#endif
}

void Tmc2240Driver::CloseGpio() {
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

bool Tmc2240Driver::WriteRegister(std::uint8_t address, std::uint32_t value) {
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

bool Tmc2240Driver::Transfer(const std::uint8_t tx[5],
                             std::uint8_t rx[5]) {
#if COATHEAL_HAS_SPIDEV && defined(COATHEAL_HAS_LIBGPIOD)
  if (spi_fd_ < 0 || !gpio_healthy_ || cs_handle_ == nullptr) return false;
  std::lock_guard<std::mutex> bus_lock(SpiBusMutex(cfg_.spi_device));
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

bool Tmc2240Driver::ReadRegister(std::uint8_t address,
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

bool Tmc2240Driver::Reinitialize() {
  std::lock_guard<std::mutex> lock(io_mu_);
  const bool restore_enabled = enabled_;
  if (enable_handle_ != nullptr) {
    EnableUnlocked(false);
  }
  if (!gpio_healthy_) {
    CloseGpio();
    if (!OpenGpio()) {
      std::cerr << "[tmc2240] reinitialize failed: GPIO unavailable\n";
      healthy_ = false;
      return false;
    }
  }
  if (spi_fd_ < 0 && !OpenSpi()) {
    std::cerr << "[tmc2240] reinitialize failed: SPI unavailable\n";
    healthy_ = false;
    return false;
  }

  std::uint32_t ioin = 0;
  if (!ReadRegister(kRegIOIN, &ioin)) {
    std::cerr << "[tmc2240] IOIN read failed on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line << '\n';
    healthy_ = false;
    return false;
  }
  const std::uint8_t version = static_cast<std::uint8_t>(ioin >> 24U);
  if (version != kExpectedVersion) {
    std::cerr << "[tmc2240] IOIN version mismatch on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line
              << " got=0x" << std::hex << static_cast<int>(version)
              << " expected=0x" << static_cast<int>(kExpectedVersion)
              << std::dec << " raw=0x" << std::hex << ioin
              << std::dec << '\n';
    healthy_ = false;
    return false;
  }
  std::uint32_t initial_gstat = 0;
  if (!ReadRegister(kRegGSTAT, &initial_gstat)) {
    std::cerr << "[tmc2240] GSTAT read failed on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line << '\n';
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegGSTAT, initial_gstat & kGlobalFaultMask)) {
    std::cerr << "[tmc2240] GSTAT clear failed on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line
              << " gstat=0x" << std::hex << initial_gstat
              << std::dec << '\n';
    healthy_ = false;
    return false;
  }
  const std::uint32_t gconf = EncodeGconf(cfg_.stealth_chop);
  Tmc2240CurrentSettings current;
  if (!CalculateCurrentSettings(
          cfg_.run_current_a_rms, cfg_.hold_current_frac,
          cfg_.current_range_a_peak, &current)) {
    std::cerr << "[tmc2240] invalid current settings run_a_rms="
              << cfg_.run_current_a_rms
              << " hold_frac=" << cfg_.hold_current_frac
              << " range_a_peak=" << cfg_.current_range_a_peak << '\n';
    healthy_ = false;
    return false;
  }
  const std::uint32_t drvconf = current.range_code;
  const std::uint32_t globalscaler = current.global_scaler;
  const std::uint32_t ihold_irun = current.ihold_irun;
  const std::uint32_t chopconf = EncodeChopconf(microstep_);
  const std::uint32_t pwmconf = 0xC40C001Du;
  const std::uint32_t tpowerdown = 0x0000000Au;

  bool ok = true;
  ok = WriteRegister(kRegGCONF, gconf) && ok;
  ok = WriteRegister(kRegDRVCONF, drvconf) && ok;
  ok = WriteRegister(kRegGLOBALSCALER, globalscaler) && ok;
  ok = WriteRegister(kRegIHOLD_IRUN, ihold_irun) && ok;
  ok = WriteRegister(kRegTPOWERDOWN, tpowerdown) && ok;
  ok = WriteRegister(kRegCHOPCONF, chopconf) && ok;
  ok = WriteRegister(kRegPWMCONF, pwmconf) && ok;
  if (!ok) {
    std::cerr << "[tmc2240] configuration write failed on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line << '\n';
  }
  std::uint32_t verify = 0;
  ok = ReadRegister(kRegGCONF, &verify) && verify == gconf && ok;
  if (!ok) {
    std::cerr << "[tmc2240] GCONF verify failed on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line
              << " got=0x" << std::hex << verify
              << " expected=0x" << gconf << std::dec << '\n';
  }
  ok = ReadRegister(kRegDRVCONF, &verify) &&
       (verify & 0x00000033U) == drvconf && ok;
  ok = ReadRegister(kRegGLOBALSCALER, &verify) &&
       (verify & 0x000000FFU) == globalscaler && ok;
  ok = ReadRegister(kRegIHOLD_IRUN, &verify) &&
       (verify & 0x0F0F1F1FU) == (ihold_irun & 0x0F0F1F1FU) && ok;
  ok = ReadRegister(kRegCHOPCONF, &verify) && verify == chopconf && ok;
  ok = ReadRegister(kRegPWMCONF, &verify) && verify == pwmconf && ok;
  std::uint32_t gstat = 0;
  std::uint32_t drv_status = 0;
  ok = ReadRegister(kRegGSTAT, &gstat) && ok;
  ok = ReadRegister(kRegDRVSTATUS, &drv_status) && ok;
  if ((gstat & kGlobalFaultMask) != 0U) {
    std::cerr << "[tmc2240] GSTAT fault on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line
              << " gstat=0x" << std::hex << gstat << std::dec << '\n';
    ok = false;
  }
  if ((drv_status & kDriverFaultMask) != 0U) {
    std::cerr << "[tmc2240] DRV_STATUS fault on " << cfg_.spi_device
              << " cs=" << cfg_.cs_line
              << " drv_status=0x" << std::hex << drv_status
              << std::dec << '\n';
    ok = false;
  }
  healthy_ = ok && gpio_healthy_;
  if (healthy_ && restore_enabled) {
    healthy_ = EnableUnlocked(true);
  }
  return healthy_;
}

bool Tmc2240Driver::Enable(bool enable) {
  std::lock_guard<std::mutex> lock(io_mu_);
  return EnableUnlocked(enable);
}

bool Tmc2240Driver::EnableUnlocked(bool enable) {
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

bool Tmc2240Driver::Step(bool direction_forward) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!healthy_ || !enabled_) return false;
  if (pulses_ > 0 && pulses_ % 256U == 0U) {
    std::uint32_t gstat = 0;
    std::uint32_t drv_status = 0;
    if (!ReadRegister(kRegGSTAT, &gstat) ||
        !ReadRegister(kRegDRVSTATUS, &drv_status) ||
        (gstat & kGlobalFaultMask) != 0U ||
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
      SetGpioOutput(static_cast<GpioOutput*>(enable_handle_),
                    cfg_.enable_active_low);
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

void Tmc2240Driver::SetMicrostep(int divisor) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!IsSupportedMicrostep(divisor)) {
    healthy_ = false;
    EnableUnlocked(false);
    return;
  }
  microstep_ = divisor;
  const std::uint32_t requested = EncodeChopconf(divisor);
  std::uint32_t verify = 0;
  if (!WriteRegister(kRegCHOPCONF, requested) ||
      !ReadRegister(kRegCHOPCONF, &verify) || verify != requested) {
    healthy_ = false;
    EnableUnlocked(false);
  }
}

}  // namespace coatheal
