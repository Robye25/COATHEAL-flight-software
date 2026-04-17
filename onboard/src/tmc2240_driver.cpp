#include "coatheal/tmc2240_driver.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>

// Linux SPI userspace interface. On non-Linux hosts (e.g. CI build matrix)
// this header is absent, so we guard the whole SPI access path.
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

// TMC2240 register addresses used by this driver.
constexpr std::uint8_t kRegGCONF      = 0x00;
constexpr std::uint8_t kRegIHOLD_IRUN = 0x10;
constexpr std::uint8_t kRegTPOWERDOWN = 0x11;
constexpr std::uint8_t kRegCHOPCONF   = 0x6C;
constexpr std::uint8_t kRegPWMCONF    = 0x70;

// Write-bit OR'd into the address byte per TMC2240 datasheet §5.3.
constexpr std::uint8_t kWriteBit = 0x80;

// TMC2240 IRUN scale is 0..31 referenced to ~2.1 A RMS full-scale with the
// Trinamic-recommended 0.075 Ω external sense resistor (TMC2240 datasheet
// rev.1.03 §5.1). The SilentStepStick-2240 / QHV2240-style boards ship with
// that sense value; we use it as the denominator. The OMC 17E19 motor is
// 2.5 A/phase nameplate; we derate IRUN to 2.0 A RMS for TMC2240 margin.
constexpr double kIrunFullScaleARms = 2.1;

}  // namespace

Tmc2240Driver::Tmc2240Driver(const Tmc2240Config& cfg)
    : cfg_(cfg), microstep_(cfg.microstep) {
  // OpenSpi() + Reinitialize() are both lazy-safe: on failure healthy_ stays
  // false and SystemController::Initialize swaps in the GPIO fallback driver.
  if (OpenSpi()) {
    Reinitialize();
  }
}

Tmc2240Driver::~Tmc2240Driver() {
  CloseSpi();
}

std::uint32_t Tmc2240Driver::EncodeGconf(bool stealth_chop) {
  // bit 2 = en_pwm_mode (stealthChop). All other bits left at reset default.
  return stealth_chop ? 0x00000004u : 0x00000000u;
}

std::uint32_t Tmc2240Driver::EncodeIholdIrun(double run_a_rms,
                                             double hold_frac) {
  const double scale = std::clamp(run_a_rms, 0.0, kIrunFullScaleARms);
  const double irun_f = std::round(scale / kIrunFullScaleARms * 31.0);
  const int irun = static_cast<int>(std::clamp(irun_f, 0.0, 31.0));
  int ihold = static_cast<int>(
      std::round(irun * std::clamp(hold_frac, 0.0, 1.0)));
  if (ihold > irun) ihold = irun;
  // IHOLDDELAY=6 — gradual current decay when the motor parks into standstill.
  return (static_cast<std::uint32_t>(6) << 16) |
         (static_cast<std::uint32_t>(irun & 0x1F) << 8) |
         static_cast<std::uint32_t>(ihold & 0x1F);
}

std::uint32_t Tmc2240Driver::EncodeChopconf(int microstep_divisor) {
  // MRES field (bits 24..27) selects native microstep count on TMC2240:
  //   0=256, 1=128, 2=64, 3=32, 4=16, 5=8, 6=4, 7=2, 8=full step.
  // TMC2240 only supports power-of-two native tiers; the Rev-B 5× channel is
  // emulated by the pulse scheduler (MRES stays at 4× and the controller
  // issues the 5× step rate).
  int mres = 6;  // default: MRES=6 -> 4 microsteps (REV-B default).
  switch (microstep_divisor) {
    case 1:   mres = 8; break;
    case 2:   mres = 7; break;
    case 4:   mres = 6; break;
    case 5:   mres = 6; break;  // hw tier; controller schedules the 5× rate.
    case 8:   mres = 5; break;
    case 16:  mres = 4; break;
    case 32:  mres = 3; break;
    case 64:  mres = 2; break;
    case 128: mres = 1; break;
    case 256: mres = 0; break;
    default:  mres = 6; break;  // safe fallback
  }
  // Field layout (TMC2240 datasheet §5.5.1):
  //   TOFF   [3:0]   = 3  (~1.67 µs, standard slow-decay setting)
  //   HSTRT  [6:4]   = 4  (hysteresis start)
  //   HEND   [10:7]  = 1  (hysteresis end, signed offset)
  //   CHM    [14]    = 0  (standard SpreadCycle)
  //   TBL    [16:15] = 2  (blank time 36 clocks)
  //   MRES   [27:24]
  std::uint32_t chopconf = 0;
  chopconf |= static_cast<std::uint32_t>(mres & 0x0F) << 24;  // MRES
  chopconf |= static_cast<std::uint32_t>(0x2) << 15;          // TBL=2
  chopconf |= static_cast<std::uint32_t>(0x1) << 7;           // HEND=1
  chopconf |= static_cast<std::uint32_t>(0x4) << 4;           // HSTRT=4
  chopconf |= static_cast<std::uint32_t>(0x3);                // TOFF=3
  return chopconf;
}

bool Tmc2240Driver::OpenSpi() {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ >= 0) return true;  // already open
  const int fd = ::open(cfg_.spi_device.c_str(), O_RDWR);
  if (fd < 0) {
    std::cerr << "[tmc2240] open(" << cfg_.spi_device
              << ") failed: " << std::strerror(errno) << '\n';
    return false;
  }
  // TMC2240 SPI mode 3 (CPOL=1, CPHA=1), 8 bits/word, up to 1 MHz.
  std::uint8_t mode = SPI_MODE_3;
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
  // Non-Linux host build: no SPI available. Caller treats healthy_==false as
  // a signal to fall back to the simulated/GPIO driver.
  std::cerr << "[tmc2240] spidev not available on this platform; "
               "driver stays unhealthy\n";
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

bool Tmc2240Driver::WriteRegister(std::uint8_t address, std::uint32_t value) {
#if COATHEAL_HAS_SPIDEV
  if (spi_fd_ < 0) return false;

  // 5-byte datagram, MSB-first: [addr|0x80][b3][b2][b1][b0].
  std::uint8_t tx[5];
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  tx[0] = static_cast<std::uint8_t>(address | kWriteBit);
  tx[1] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
  tx[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  tx[3] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  tx[4] = static_cast<std::uint8_t>(value & 0xFF);

  spi_ioc_transfer xfer{};
  xfer.tx_buf = reinterpret_cast<__u64>(tx);
  xfer.rx_buf = reinterpret_cast<__u64>(rx);
  xfer.len = sizeof(tx);
  xfer.speed_hz = cfg_.spi_speed_hz;
  xfer.bits_per_word = 8;
  xfer.cs_change = 0;

  const int rc = ::ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &xfer);
  if (rc < 0) {
    std::cerr << "[tmc2240] SPI_IOC_MESSAGE failed for reg 0x"
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

bool Tmc2240Driver::Reinitialize() {
  const std::uint32_t gconf = EncodeGconf(cfg_.stealth_chop);
  const std::uint32_t ihold_irun =
      EncodeIholdIrun(cfg_.run_current_a_rms, cfg_.hold_current_frac);
  const std::uint32_t chopconf = EncodeChopconf(microstep_);
  // PWMCONF: TMC2240 stealthChop autoscale profile. Bits: pwm_autoscale (18),
  // pwm_autograd (19), PWM_FREQ=1, PWM_REG=4, PWM_OFS=36. See datasheet §5.6.
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

bool Tmc2240Driver::Enable(bool /*enable*/) {
  // The hardware /EN pin is authoritative and is driven by the channel-side
  // GPIO wrapper (GpioStepDirStepperDriver-style). GCONF bit 0 (drv_enn) is
  // left cleared so the IC is always listening to /EN.
  return healthy_;
}

bool Tmc2240Driver::Step(bool /*direction_forward*/) {
  if (!healthy_) return false;
  // The channel-side code issues the real STEP GPIO pulse; we just bookkeep
  // pulses for diagnostics parity with SimulatedStepperDriver.
  ++pulses_;
  return true;
}

void Tmc2240Driver::SetMicrostep(int divisor) {
  if (divisor <= 0) return;
  microstep_ = divisor;
  // Re-write CHOPCONF only, so run/hold currents stay programmed.
  WriteRegister(kRegCHOPCONF, EncodeChopconf(divisor));
}

}  // namespace coatheal
