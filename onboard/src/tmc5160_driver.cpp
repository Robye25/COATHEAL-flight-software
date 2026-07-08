#include "coatheal/tmc5160_driver.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>

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
constexpr std::uint8_t kRegIHOLD_IRUN = 0x10;
constexpr std::uint8_t kRegTPOWERDOWN = 0x11;
constexpr std::uint8_t kRegCHOPCONF   = 0x6C;
constexpr std::uint8_t kRegPWMCONF    = 0x70;
constexpr std::uint8_t kWriteBit = 0x80;

// Board-level current limit is set by the FYSETC TMC5160 module's sense
// resistor and cooling. Keep the software scale conservative until bench
// current is measured on the exact modules.
constexpr double kIrunFullScaleARms = 2.5;

}  // namespace

Tmc5160Driver::Tmc5160Driver(const Tmc5160Config& cfg)
    : cfg_(cfg), microstep_(cfg.microstep) {
  if (OpenSpi()) {
    Reinitialize();
  }
}

Tmc5160Driver::~Tmc5160Driver() {
  CloseSpi();
}

std::uint32_t Tmc5160Driver::EncodeGconf(bool stealth_chop) {
  return stealth_chop ? 0x00000004u : 0x00000000u;
}

std::uint32_t Tmc5160Driver::EncodeIholdIrun(double run_a_rms,
                                             double hold_frac) {
  const double scale = std::clamp(run_a_rms, 0.0, kIrunFullScaleARms);
  const double irun_f = std::round(scale / kIrunFullScaleARms * 31.0);
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
  std::uint8_t mode = SPI_MODE_3;
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

void Tmc5160Driver::CloseSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) {
    ::close(spi_fd_);
    spi_fd_ = -1;
  }
#endif
}

bool Tmc5160Driver::WriteRegister(std::uint8_t address, std::uint32_t value) {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ < 0) return false;

  std::uint8_t tx[5];
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  tx[0] = static_cast<std::uint8_t>(address | kWriteBit);
  tx[1] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  tx[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  tx[3] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  tx[4] = static_cast<std::uint8_t>(value & 0xFF);

  struct spi_ioc_transfer xfer {};
  xfer.tx_buf = reinterpret_cast<__u64>(tx);
  xfer.rx_buf = reinterpret_cast<__u64>(rx);
  xfer.len = sizeof(tx);
  xfer.speed_hz = cfg_.spi_speed_hz;
  xfer.bits_per_word = 8;
  xfer.cs_change = 0;

  const int rc = ::ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &xfer);
  if (rc < 0) {
    std::cerr << "[tmc5160] SPI_IOC_MESSAGE failed for reg 0x"
              << std::hex << static_cast<int>(address) << std::dec
              << ": " << std::strerror(errno) << '\n';
    return false;
  }
  return true;
#else
  (void)address;
  (void)value;
  return false;
#endif
}

bool Tmc5160Driver::Reinitialize() {
  const std::uint32_t gconf = EncodeGconf(cfg_.stealth_chop);
  const std::uint32_t ihold_irun =
      EncodeIholdIrun(cfg_.run_current_a_rms, cfg_.hold_current_frac);
  const std::uint32_t chopconf = EncodeChopconf(microstep_);
  const std::uint32_t pwmconf = 0xC10D0024u;
  const std::uint32_t tpowerdown = 0x0000000Au;

  bool ok = true;
  ok = WriteRegister(kRegGCONF, gconf) && ok;
  ok = WriteRegister(kRegIHOLD_IRUN, ihold_irun) && ok;
  ok = WriteRegister(kRegTPOWERDOWN, tpowerdown) && ok;
  ok = WriteRegister(kRegCHOPCONF, chopconf) && ok;
  ok = WriteRegister(kRegPWMCONF, pwmconf) && ok;
  healthy_ = ok;
  return ok;
}

bool Tmc5160Driver::Enable(bool /*enable*/) {
  return healthy_;
}

bool Tmc5160Driver::Step(bool /*direction_forward*/) {
  if (!healthy_) return false;
  ++pulses_;
  return true;
}

void Tmc5160Driver::SetMicrostep(int divisor) {
  if (divisor <= 0) return;
  microstep_ = divisor;
  WriteRegister(kRegCHOPCONF, EncodeChopconf(divisor));
}

}  // namespace coatheal
