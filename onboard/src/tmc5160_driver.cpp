#include "coatheal/tmc5160_driver.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

#include "coatheal/hal/spi_bus_lock.hpp"

namespace coatheal {

namespace {

constexpr std::uint8_t kRegGCONF = 0x00;
constexpr std::uint8_t kRegGSTAT = 0x01;
constexpr std::uint8_t kRegIOIN = 0x04;
constexpr std::uint8_t kRegTSTEP = 0x12;
constexpr std::uint8_t kRegVACTUAL = 0x22;
constexpr std::uint8_t kRegRAMPSTAT = 0x35;
constexpr std::uint8_t kRegMSCNT = 0x6A;
constexpr std::uint8_t kRegDRV_STATUS = 0x6F;
constexpr std::uint8_t kRegPWM_SCALE = 0x71;
constexpr std::uint8_t kRegPWM_AUTO = 0x72;
constexpr std::uint8_t kRegGLOBALSCALER = 0x0B;
constexpr std::uint8_t kRegIHOLD_IRUN = 0x10;
constexpr std::uint8_t kRegTPOWERDOWN = 0x11;
constexpr std::uint8_t kRegRAMPMODE = 0x20;
constexpr std::uint8_t kRegXACTUAL = 0x21;
constexpr std::uint8_t kRegVSTART = 0x23;
constexpr std::uint8_t kRegA1 = 0x24;
constexpr std::uint8_t kRegV1 = 0x25;
constexpr std::uint8_t kRegAMAX = 0x26;
constexpr std::uint8_t kRegVMAX = 0x27;
constexpr std::uint8_t kRegDMAX = 0x28;
constexpr std::uint8_t kRegD1 = 0x2A;
constexpr std::uint8_t kRegVSTOP = 0x2B;
constexpr std::uint8_t kRegXTARGET = 0x2D;
constexpr std::uint8_t kRegCHOPCONF = 0x6C;
constexpr std::uint8_t kWriteBit = 0x80;

constexpr std::uint8_t kExpectedVersion = 0x30;

// IOIN (0x04) input-pin readback bits. DRV_ENN (bit 4) is the enable input,
// active LOW: a HIGH readback means the power stage is disabled. SD_MODE
// (bit 6) selects the motion source: LOW = internal ramp generator over
// SPI, HIGH = external STEP/DIR pins.
constexpr std::uint32_t kIoinDrvEnn = 1U << 4;
constexpr std::uint32_t kIoinSdMode = 1U << 6;

// GCONF bit2 (en_pwm_mode / StealthChop): quiet low-speed operation, at the
// cost of torque headroom. Driven by Tmc5160Config::stealth_chop, which the
// factory copies from motorN.stealth_chop -- every other GCONF bit stays at
// its reset value of 0, so GCONF is exactly this bit or nothing.
constexpr std::uint32_t kGconfEnPwmMode = 0x00000004U;

// TMC5160 datasheet fixed full-scale sense voltage.
constexpr double kVfs = 0.325;

// CHOPCONF bits 16:15 (TBL, blank time, 2-bit field) = 2 and bits 6:4
// (HSTRT, hysteresis start, 3-bit field) = 4: datasheet-recommended
// spreadCycle defaults, unrelated to microstep/TOFF. HEND (bits 10:7,
// hysteresis end) is left at its reset value of 0 -- a bench-tunable
// default, not something this task's tests need to pin.
constexpr std::uint32_t kChopconfTbl = 0x2U << 15;
constexpr std::uint32_t kChopconfHstrt = 0x4U << 4;

// IHOLD_IRUN bits 19:16 (IHOLDDELAY): fixed stand-still current ramp-down
// delay; not exposed in Tmc5160Config, not load-bearing for this task.
constexpr std::uint32_t kIholdDelay = 6U;

// Fixed short stand-still power-down delay. TPOWERDOWN's LSB is 2^18/f_clk
// (~21.8 ms at the default 12 MHz internal clock), so 10 is roughly 218 ms
// -- not the ~2.1 ms this comment previously (incorrectly) claimed. Not
// exposed in Tmc5160Config.
constexpr std::uint32_t kTpowerdown = 10U;

// VMAX: the pacing thread above this seam drives Step() at up to 100 Hz
// (MotorConfig.max_step_hz); XTARGET moves in the ramp generator's fixed
// 256 internal-microsteps/fullstep units regardless of MRES, so a 100 Hz
// dribble corresponds to a worst-case demand of 100*256 = 25600 internal-
// microsteps/s. The VMAX register's unit is f_clk/2^24 internal-
// microsteps/s (~0.715 usteps/s per LSB at the default 12 MHz internal
// clock), so VMAX=102400 corresponds to roughly 73000 usteps/s -- a ~2.9x
// margin over the worst-case demand (not the 4x the raw
// 4*100*256 arithmetic below might suggest), comfortably keeping the ramp
// generator from ever becoming the limiting factor at any configured
// microstep divisor.
constexpr std::uint32_t kVmax = 4U * 100U * 256U;

// GSTAT flags (write 1 to clear): bit 0 reset, bit 1 drv_err, bit 2 uv_cp.
constexpr std::uint32_t kGstatReset = 0x1U;
constexpr std::uint32_t kGstatClearAll = 0x7U;
// Step() re-reads GSTAT this often. One 5-byte datagram per 64 steps is
// noise next to the XTARGET write every step, and a brown-out that wiped
// the chip mid-move is caught within 0.64 s at 100 Hz.
constexpr std::uint32_t kResetCheckInterval = 64U;

bool IsSupportedMicrostep(int divisor) {
  return Tmc5160Driver::EncodeMres(divisor) != Tmc5160Driver::kInvalidMres;
}

}  // namespace

Tmc5160Driver::Tmc5160Driver(Tmc5160Config cfg, SpiBus* bus, bool use_gpio)
    : cfg_(std::move(cfg)),
      bus_(bus),
      use_gpio_(use_gpio),
      microstep_(cfg_.microstep),
      verify_enable_line_(use_gpio) {
  if (bus_ == nullptr) return;
  const bool gpio_ok = OpenGpio();
  const bool spi_ok = OpenSpi() && ReinitializeUnlocked();
  healthy_ = gpio_ok && spi_ok;
}

Tmc5160Driver::~Tmc5160Driver() {
  Enable(false);
  CloseGpio();
  if (spi_open_ && bus_ != nullptr) {
    bus_->Close();
    spi_open_ = false;
  }
}

std::uint8_t Tmc5160Driver::EncodeMres(int divisor) {
  switch (divisor) {
    case 256: return 0;
    case 128: return 1;
    case 64: return 2;
    case 32: return 3;
    case 16: return 4;
    case 8: return 5;
    case 4: return 6;
    case 2: return 7;
    case 1: return 8;
    default: return kInvalidMres;
  }
}

bool Tmc5160Driver::IoinStepDirMode(std::uint32_t ioin) {
  return (ioin & kIoinSdMode) != 0U;
}

bool Tmc5160Driver::IoinDriverDisabled(std::uint32_t ioin) {
  return (ioin & kIoinDrvEnn) != 0U;
}

std::uint32_t Tmc5160Driver::DeltaXtarget(int divisor) {
  if (EncodeMres(divisor) == kInvalidMres) return 0;
  return static_cast<std::uint32_t>(256 / divisor);
}

// TMC5160 datasheet, "Selecting Sense Resistors": with the chip's fixed
// full-scale sense voltage Vfs and a board sense resistor R,
//
//   I_peak = (GLOBALSCALER/256) * ((IRUN+1)/32) * (Vfs/R)
//
// The maximum current this sense resistor can ever deliver is reached at
// GLOBALSCALER=256, IRUN=31: I_peak_max = Vfs/R. A request above that is
// rejected outright -- no choice of GLOBALSCALER/IRUN can reach it (both
// factors only ever *reduce* the achievable current below that ceiling).
//
// Below the ceiling, there are two equally-valid ways to hit any target
// current: hold IRUN at 31 and let GLOBALSCALER alone carry the fraction of
// full scale, or hold GLOBALSCALER at 256 and let IRUN alone carry it.
// GLOBALSCALER has 8-bit resolution (32..256 usable) vs. IRUN's 5-bit
// resolution (0..31), so GLOBALSCALER gives finer control -- but only up to
// the point where GLOBALSCALER would need to exceed 256, i.e. once the
// target current passes 50% of I_peak_max. Past that point GLOBALSCALER is
// pinned at its maximum (256) for the best available current-DAC
// resolution and IRUN is *reduced* below 31 to land exactly on the target
// -- the "IRUN reduction" branch below.
bool Tmc5160Driver::CalculateCurrent(double a_rms, double sense_ohm,
                                     double hold_frac,
                                     std::uint32_t* globalscaler,
                                     std::uint8_t* irun, std::uint8_t* ihold) {
  if (globalscaler == nullptr || irun == nullptr || ihold == nullptr) {
    return false;
  }
  if (!std::isfinite(a_rms) || !std::isfinite(sense_ohm) ||
      !std::isfinite(hold_frac)) {
    return false;
  }
  if (a_rms <= 0.0 || sense_ohm <= 0.0 || hold_frac < 0.0 ||
      hold_frac > 1.0) {
    return false;
  }

  const double i_peak = a_rms * std::sqrt(2.0);
  const double i_peak_max = kVfs / sense_ohm;
  if (i_peak > i_peak_max) return false;

  const double fraction = i_peak / i_peak_max;  // 0..1

  int chosen_irun;
  double gs_raw;
  if (fraction <= 0.5) {
    chosen_irun = 31;
    gs_raw = 256.0 * fraction;
    if (gs_raw < 32.0) {
      // GLOBALSCALER floor: 32 is the lowest sane register value (below it
      // the current DAC's resolution degrades badly). Below 12.5% of the
      // max deliverable current, GLOBALSCALER alone can't represent the
      // target without dropping under that floor. Naively clamping
      // GLOBALSCALER up to 32 while leaving IRUN at 31 would silently
      // *overcurrent* the motor (a 0.1 A_rms request would deliver
      // ~0.38 A_rms, +283%) -- so instead pin GLOBALSCALER=32 and let IRUN
      // carry the (very small) remainder, the low-current mirror of the
      // >50% high-regime branch below: at GS=32,
      //   I_peak = ((IRUN+1)/256) * (Vfs/R_sense)
      //   => IRUN = round(256 * I_peak * R_sense / Vfs) - 1
      gs_raw = 32.0;
      chosen_irun = static_cast<int>(
          std::lround(256.0 * i_peak * sense_ohm / kVfs)) - 1;
      chosen_irun = std::clamp(chosen_irun, 0, 31);
      // Even IRUN=0 at the GS floor has a nonzero minimum deliverable
      // current (I_peak_max/256): a request small enough to sit well below
      // that floor can't be honoured without materially overcurrenting the
      // motor. Reject loudly rather than silently deliver >10% more than
      // asked for -- same convention as the unreachable-ceiling rejection
      // above.
      const double delivered = (32.0 / 256.0) *
                               ((chosen_irun + 1) / 32.0) *
                               (kVfs / sense_ohm);
      if (delivered > i_peak * 1.10) return false;
    }
  } else {
    gs_raw = 256.0;
    chosen_irun = static_cast<int>(std::lround(32.0 * fraction)) - 1;
  }
  chosen_irun = std::clamp(chosen_irun, 0, 31);

  std::uint32_t gs = static_cast<std::uint32_t>(std::lround(gs_raw));
  gs = std::clamp(gs, 32U, 256U);

  *irun = static_cast<std::uint8_t>(chosen_irun);
  *globalscaler = gs;
  // IHOLD scales relative to the *chosen* IRUN (not a fixed 0..31 range):
  // hold_frac=0 must give IHOLD=0 and hold_frac=1 must give IHOLD==IRUN
  // (full run current held). round((IRUN+1)*frac)-1 hits both endpoints
  // exactly; clamped to [0, IRUN] as a belt-and-suspenders bound.
  *ihold = static_cast<std::uint8_t>(std::clamp(
      static_cast<int>(std::lround((chosen_irun + 1) * hold_frac)) - 1, 0,
      chosen_irun));
  return true;
}

std::uint32_t Tmc5160Driver::EncodeChopconf(std::uint8_t toff) const {
  const std::uint8_t mres = EncodeMres(microstep_);
  const std::uint8_t safe_mres = (mres == kInvalidMres) ? 0x06U : mres;
  std::uint32_t chopconf = 0;
  chopconf |= static_cast<std::uint32_t>(safe_mres) << 24;
  chopconf |= kChopconfTbl;
  chopconf |= kChopconfHstrt;
  chopconf |= static_cast<std::uint32_t>(toff & 0x0FU);
  return chopconf;
}

bool Tmc5160Driver::OpenGpio() {
  if (!use_gpio_) {
    gpio_healthy_ = true;
    return true;
  }
  cs_handle_ = RequestGpioOutput(cfg_.gpio_chip, cfg_.cs_line,
                                 "coatheal-tmc5160-cs", /*initial_value=*/true);
  const bool en_initial = cfg_.enable_active_low;  // active-low: HIGH=off
  enable_handle_ = RequestGpioOutput(cfg_.gpio_chip, cfg_.enable_line,
                                     "coatheal-tmc5160-en", en_initial);
  if (cs_handle_ == nullptr || enable_handle_ == nullptr) {
    std::cerr << "[tmc5160] GPIO request failed on " << cfg_.gpio_chip
              << " cs=" << cfg_.cs_line << " en=" << cfg_.enable_line << '\n';
    CloseGpio();
    return false;
  }
  gpio_healthy_ = true;
  return true;
}

void Tmc5160Driver::CloseGpio() {
  if (use_gpio_) {
    if (cs_handle_ != nullptr) SetGpioOutput(cs_handle_, true);
    if (enable_handle_ != nullptr) {
      SetGpioOutput(enable_handle_, cfg_.enable_active_low);
    }
    ReleaseGpioOutput(cs_handle_);
    ReleaseGpioOutput(enable_handle_);
  }
  cs_handle_ = nullptr;
  enable_handle_ = nullptr;
  gpio_healthy_ = false;
}

bool Tmc5160Driver::OpenSpi() {
  if (bus_ == nullptr) return false;
  if (spi_open_) return true;
  // no_cs=true: this device uses a soft GPIO chip-select. The kernel must
  // not assert CE0/CE1 -- they are wired to the MAX31865 clicks.
  //
  // Open() programs the shared node's mode/speed, so it is a bus event, not
  // a private one: take the controller lock so it cannot land between
  // another driver's settings re-apply and its data ioctl. Never called
  // from inside Transfer(), so this hold never nests.
  {
    SpiBusLock bus_lock(cfg_.spi_device);
    spi_open_ = bus_->Open(cfg_.spi_device, /*mode=*/3, cfg_.spi_speed_hz,
                           /*no_cs=*/true);
  }
  if (!spi_open_) {
    spi_bus_ok_ = false;
    std::cerr << "[tmc5160] SPI open failed on " << cfg_.spi_device << '\n';
  }
  return spi_open_;
}

bool Tmc5160Driver::Transfer(const std::uint8_t tx[5], std::uint8_t rx[5]) {
  // Every early return below is a datagram that could not be conducted at
  // all -- that, and only that, is what spi_bus_ok_ reports.
  if (bus_ == nullptr || !spi_open_) {
    spi_bus_ok_ = false;
    return false;
  }
  if (use_gpio_ && (!gpio_healthy_ || cs_handle_ == nullptr)) {
    spi_bus_ok_ = false;
    return false;
  }

  // ONE hold spans the whole cs-low -> [settings re-apply + data ioctl] ->
  // cs-high triplet, and the lock is keyed per physical SPI0 controller,
  // not per device node -- so the MAX31865 clicks (including click 1 on
  // /dev/spidev0.1) cannot clock the shared bus while this soft CS is
  // asserted. Full rule and rationale: hal/spi_bus_lock.hpp.
  SpiBusLock bus_lock(cfg_.spi_device);
  if (use_gpio_) {
    if (!SetGpioOutput(cs_handle_, false)) {
      gpio_healthy_ = false;
      spi_bus_ok_ = false;
      return false;
    }
  }
  const bool transferred = bus_->Transfer(tx, rx, 5);
  if (use_gpio_) {
    if (!SetGpioOutput(cs_handle_, true)) {
      // The datagram may have clocked, but CS is now stuck: the
      // conversation did not complete cleanly either way.
      gpio_healthy_ = false;
      spi_bus_ok_ = false;
      return false;
    }
  }
  spi_bus_ok_ = transferred;
  return transferred;
}

bool Tmc5160Driver::WriteRegister(std::uint8_t address, std::uint32_t value) {
  std::uint8_t tx[5];
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  tx[0] = static_cast<std::uint8_t>(address | kWriteBit);
  tx[1] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
  tx[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
  tx[3] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  tx[4] = static_cast<std::uint8_t>(value & 0xFFU);
  return Transfer(tx, rx);
}

bool Tmc5160Driver::ReadRegister(std::uint8_t address, std::uint32_t* value) {
  if (value == nullptr) return false;
  std::uint8_t tx[5] = {static_cast<std::uint8_t>(address & 0x7FU), 0, 0, 0,
                        0};
  std::uint8_t rx[5] = {0, 0, 0, 0, 0};
  // Two-phase read: the first exchange latches the address, the reply data
  // for THIS request only appears on the second exchange.
  if (!Transfer(tx, rx) || !Transfer(tx, rx)) return false;
  *value = (static_cast<std::uint32_t>(rx[1]) << 24) |
           (static_cast<std::uint32_t>(rx[2]) << 16) |
           (static_cast<std::uint32_t>(rx[3]) << 8) |
           static_cast<std::uint32_t>(rx[4]);
  return true;
}

bool Tmc5160Driver::Reinitialize() {
  std::lock_guard<std::mutex> lock(io_mu_);
  return ReinitializeUnlocked();
}

bool Tmc5160Driver::ReinitializeUnlocked() {
  if (bus_ == nullptr) {
    healthy_ = false;
    return false;
  }
  if (use_gpio_ && !gpio_healthy_) {
    CloseGpio();
    if (!OpenGpio()) {
      std::cerr << "[tmc5160] reinitialize failed: GPIO unavailable on "
                << cfg_.gpio_chip << '\n';
      healthy_ = false;
      return false;
    }
  }
  if (!spi_open_ && !OpenSpi()) {
    std::cerr << "[tmc5160] reinitialize failed: SPI unavailable on "
              << cfg_.spi_device << '\n';
    healthy_ = false;
    return false;
  }
  if (!IsSupportedMicrostep(microstep_)) {
    std::cerr << "[tmc5160] invalid microstep divisor " << microstep_
              << '\n';
    healthy_ = false;
    return false;
  }

  std::uint32_t ioin = 0;
  if (!ReadRegister(kRegIOIN, &ioin)) {
    ReportError("IOIN read failed on " + cfg_.spi_device + " cs=" +
                std::to_string(cfg_.cs_line));
    healthy_ = false;
    return false;
  }
  const auto version = static_cast<std::uint8_t>(ioin >> 24);
  if (version != kExpectedVersion) {
    std::ostringstream msg;
    msg << "TMC5160_VERSION mismatch on " << cfg_.spi_device
        << " cs=" << cfg_.cs_line << " got=0x" << std::hex
        << static_cast<int>(version) << " expected=0x"
        << static_cast<int>(kExpectedVersion);
    ReportError(msg.str());
    healthy_ = false;
    return false;
  }

  // SD_MODE strap gate. With SD_MODE tied HIGH the chip takes its motion
  // from the STEP/DIR pins and the internal ramp generator this driver
  // steers is bypassed. That failure is invisible from the SPI side unless
  // it is checked for: XACTUAL still tracks every XTARGET write, so each
  // Step() returns true, pulses_issued() climbs, the position telemetry
  // advances and CHECK MOTORn reports OK -- while the microstep sequencer
  // never advances and the motor only ever holds its last position. The
  // v3 pinout has no STEP/DIR lines, so a module strapped this way can
  // never pull a specimen; refuse it here rather than report a healthy
  // motor that silently does nothing. (Bench, 2026-08-24: motor0's module
  // was strapped SD_MODE=1 while motor1's was SD_MODE=0 -- proven by
  // MSCNT staying frozen on motor0 and advancing on motor1 for the
  // identical commanded move.)
  if (IoinStepDirMode(ioin)) {
    ReportError(cfg_.spi_device + " cs=" + std::to_string(cfg_.cs_line) +
                ": SD_MODE strapped HIGH -- driver is in STEP/DIR mode and"
                " ignores SPI ramp-generator motion. Tie SD_MODE low to use"
                " this firmware; the motor cannot move as wired.");
    healthy_ = false;
    return false;
  }

  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  if (!CalculateCurrent(cfg_.run_current_a_rms, cfg_.sense_resistor_ohm,
                        cfg_.hold_current_frac, &globalscaler, &irun,
                        &ihold)) {
    std::cerr << "[tmc5160] invalid current settings run_a_rms="
              << cfg_.run_current_a_rms
              << " sense_ohm=" << cfg_.sense_resistor_ohm << '\n';
    healthy_ = false;
    return false;
  }

  const std::uint32_t gconf = cfg_.stealth_chop ? kGconfEnPwmMode : 0U;
  const std::uint32_t chopconf = EncodeChopconf(/*toff=*/3);
  // GLOBALSCALER register convention: 0 means "256" (full scale); 256 never
  // appears on the wire as itself.
  const std::uint32_t gs_reg = (globalscaler >= 256U) ? 0U : globalscaler;
  const std::uint32_t ihold_irun =
      (static_cast<std::uint32_t>(ihold) & 0x1FU) |
      ((static_cast<std::uint32_t>(irun) & 0x1FU) << 8) |
      (kIholdDelay << 16);

  if (!WriteRegister(kRegGCONF, gconf)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegCHOPCONF, chopconf)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegGLOBALSCALER, gs_reg)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegIHOLD_IRUN, ihold_irun)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegTPOWERDOWN, kTpowerdown)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegRAMPMODE, 0)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegVSTART, 0)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegVSTOP, 10)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegA1, 0xFFFF)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegAMAX, 0xFFFF)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegV1, 0)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegD1, 0xFFFF)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegDMAX, 0xFFFF)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegVMAX, kVmax)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegXACTUAL, 0)) {
    healthy_ = false;
    return false;
  }
  if (!WriteRegister(kRegXTARGET, 0)) {
    healthy_ = false;
    return false;
  }
  target_ = 0;

  std::uint32_t verify = 0;
  if (!ReadRegister(kRegGCONF, &verify) || verify != gconf) {
    std::cerr << "[tmc5160] GCONF verify failed on " << cfg_.spi_device
              << '\n';
    healthy_ = false;
    return false;
  }
  if (!ReadRegister(kRegCHOPCONF, &verify) || verify != chopconf) {
    healthy_ = false;
    return false;
  }
  // The configuration is on the chip now: clear the reset/drv_err/uv_cp
  // flags so that GSTAT.reset from here on means exactly "the chip lost its
  // configuration since this point".
  if (!WriteRegister(kRegGSTAT, kGstatClearAll)) {
    std::cerr << "[tmc5160] CHOPCONF verify failed on " << cfg_.spi_device
              << '\n';
    healthy_ = false;
    return false;
  }

  healthy_ = true;
  last_error_message_.clear();
  return true;
}

void Tmc5160Driver::ReportError(const std::string& message) {
  if (message == last_error_message_) return;
  last_error_message_ = message;
  std::cerr << "[tmc5160] " << message << '\n';
}

bool Tmc5160Driver::Enable(bool enable) {
  std::lock_guard<std::mutex> lock(io_mu_);
  return EnableUnlocked(enable);
}

bool Tmc5160Driver::EnableUnlocked(bool enable) {
  if (bus_ == nullptr) return false;

  if (!enable) {
    // Freeze in place, no coast: retarget XTARGET to the current actual
    // position BEFORE cutting the chopper, so the ramp generator has
    // nothing left to chase once TOFF drops. Doing this in any other order
    // risks a brief uncommanded coast (either the ramp generator still
    // driving toward a stale target, or the motor freewheeling on EN before
    // the target/TOFF settle).
    std::uint32_t xactual = 0;
    bool ok = ReadRegister(kRegXACTUAL, &xactual);
    if (ok) {
      ok = WriteRegister(kRegXTARGET, xactual);
      if (ok) target_ = static_cast<std::int32_t>(xactual);
    }
    if (ok) {
      ok = WriteRegister(kRegCHOPCONF, EncodeChopconf(/*toff=*/0));
    }
    if (!ok) healthy_ = false;

    if (use_gpio_ && enable_handle_ != nullptr) {
      const bool off_value = cfg_.enable_active_low;  // active-low: HIGH=off
      if (!SetGpioOutput(enable_handle_, off_value)) {
        gpio_healthy_ = false;
        healthy_ = false;
        ok = false;
      }
    }
    enabled_ = false;
    // Prove the enable line can also DISABLE the chip. A module whose EN
    // pin is not routed to DRV_ENN (bench, 2026-08-28: motor1's DRV_ENN
    // read 0 whatever GPIO 21 did) passes the Enable(true) check -- the
    // pin already reads enabled -- and moves fine, but STEPPER_DISABLE
    // then only stops the chopper (TOFF=0); the power stage is never
    // de-energised through EN. That is a warning, not a fault: the motor
    // still works, the operator must know.
    if (ok && verify_enable_line_) {
      std::uint32_t ioin = 0;
      if (!ReadRegister(kRegIOIN, &ioin)) {
        ReportError(cfg_.spi_device + " cs=" + std::to_string(cfg_.cs_line) +
                    ": IOIN read failed while verifying disable");
        healthy_ = false;
        return false;
      }
      const bool effective = IoinDriverDisabled(ioin);
      if (!effective && !enable_warning_logged_) {
        std::cerr << "[tmc5160] " << cfg_.spi_device << " cs=" << cfg_.cs_line
                  << ": DRV_ENN still LOW after driving enable line "
                  << cfg_.enable_line << " to disable -- the enable line does"
                  << " not reach the driver (module EN not routed to DRV_ENN?);"
                  << " STEPPER_DISABLE can only stop the chopper, the power"
                  << " stage stays energised through EN.\n";
        enable_warning_logged_ = true;
      }
      if (effective) enable_warning_logged_ = false;
      enable_line_effective_ = effective;
    }
    return ok;
  }

  // Enable(true): energize EN first, (re)probe if unhealthy, then
  // unconditionally restore the chopper (TOFF=3) -- Enable(false) may have
  // left TOFF=0 even while healthy_ stayed true.
  if (use_gpio_ && enable_handle_ != nullptr) {
    const bool on_value = !cfg_.enable_active_low;  // active-low: LOW=on
    if (!SetGpioOutput(enable_handle_, on_value)) {
      gpio_healthy_ = false;
      healthy_ = false;
      return false;
    }
  }
  if (!healthy_ && !ReinitializeUnlocked()) {
    return false;
  }
  if (!RecoverFromChipResetUnlocked("enable")) {
    return false;
  }
  steps_since_reset_check_ = 0;
  // Prove the enable line actually reached the chip. DRV_ENN is mirrored
  // in IOIN, so a broken or unrouted enable trace is detectable over SPI
  // -- and it has to be, because it presents exactly like a healthy motor
  // that never moves. Only checked when this driver owns the GPIO: with
  // use_gpio_=false nothing here drives the pin and its level says nothing
  // about us. (Bench, 2026-08-24: motor1's DRV_ENN never followed its
  // enable GPIO.)
  if (verify_enable_line_) {
    std::uint32_t enable_ioin = 0;
    if (!ReadRegister(kRegIOIN, &enable_ioin)) {
      ReportError(cfg_.spi_device + " cs=" + std::to_string(cfg_.cs_line) +
                  ": IOIN read failed while verifying enable");
      healthy_ = false;
      return false;
    }
    if (IoinDriverDisabled(enable_ioin)) {
      ReportError(cfg_.spi_device + " cs=" + std::to_string(cfg_.cs_line) +
                  ": DRV_ENN still HIGH after driving enable line " +
                  std::to_string(cfg_.enable_line) +
                  " -- enable signal is not reaching the driver; the power"
                  " stage stays off and the motor cannot move.");
      healthy_ = false;
      return false;
    }
  }
  if (!WriteRegister(kRegCHOPCONF, EncodeChopconf(/*toff=*/3))) {
    healthy_ = false;
    return false;
  }
  enabled_ = true;
  return true;
}

bool Tmc5160Driver::Poll() {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (bus_ == nullptr || !spi_open_ || !healthy_ || !enabled_) return healthy_;
  return RecoverFromChipResetUnlocked("idle");
}

std::string Tmc5160Driver::DebugRegisters() {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (bus_ == nullptr || !spi_open_) return {};
  struct Reg { const char* name; std::uint8_t addr; std::uint32_t value; };
  Reg regs[] = {
      {"xactual", kRegXACTUAL, 0}, {"xtarget", kRegXTARGET, 0},
      {"vactual", kRegVACTUAL, 0}, {"mscnt", kRegMSCNT, 0},
      {"drv_status", kRegDRV_STATUS, 0}, {"rampstat", kRegRAMPSTAT, 0},
      {"tstep", kRegTSTEP, 0}, {"ioin", kRegIOIN, 0},
      {"gstat", kRegGSTAT, 0}, {"chopconf", kRegCHOPCONF, 0},
      // stealthChop's own view of the coils: PWM_SCALE_SUM is the PWM
      // amplitude the current regulator needs to reach the target. Pinned
      // at 255 = it cannot get there (VM too low, coil open/too resistive,
      // wrong sense resistor); a moderate value = current really flows.
      {"pwm_scale", kRegPWM_SCALE, 0}, {"pwm_auto", kRegPWM_AUTO, 0},
  };
  for (Reg& reg : regs) {
    if (!ReadRegister(reg.addr, &reg.value)) return {};
  }
  const std::uint32_t xactual = regs[0].value, xtarget = regs[1].value,
                      vactual = regs[2].value, mscnt = regs[3].value,
                      drv = regs[4].value, ramp = regs[5].value,
                      tstep = regs[6].value, ioin = regs[7].value,
                      gstat = regs[8].value, chop = regs[9].value,
                      pwm_scale = regs[10].value, pwm_auto = regs[11].value;
  std::int32_t pwm_scale_auto = static_cast<std::int32_t>((pwm_scale >> 16) & 0x1FFU);
  if (pwm_scale_auto & 0x100) pwm_scale_auto -= 0x200;  // 9-bit signed
  // VACTUAL is a 24-bit two's-complement value in 1/256-step units per
  // 2^24/fCLK seconds; XACTUAL/XTARGET are 32-bit signed 1/256-step counts.
  std::int32_t v24 = static_cast<std::int32_t>(vactual & 0xFFFFFFU);
  if (v24 & 0x800000) v24 -= 0x1000000;
  const unsigned mres = (chop >> 24) & 0xFU;
  std::ostringstream out;
  out << "xactual=" << static_cast<std::int32_t>(xactual)
      << ";xtarget=" << static_cast<std::int32_t>(xtarget)
      << ";vactual=" << v24
      << ";mscnt=" << (mscnt & 0x3FFU)
      << ";tstep=" << (tstep & 0xFFFFFU)
      << ";drv_status=0x" << std::hex << drv << std::dec
      << ";stst=" << ((drv >> 31) & 1U)
      << ";cs_actual=" << ((drv >> 16) & 0x1FU)
      << ";sg_result=" << (drv & 0x3FFU)
      << ";stallguard=" << ((drv >> 24) & 1U)
      << ";ot=" << ((drv >> 25) & 1U) << ";otpw=" << ((drv >> 26) & 1U)
      << ";s2ga=" << ((drv >> 27) & 1U) << ";s2gb=" << ((drv >> 28) & 1U)
      << ";ola=" << ((drv >> 29) & 1U) << ";olb=" << ((drv >> 30) & 1U)
      << ";s2vsa=" << ((drv >> 12) & 1U) << ";s2vsb=" << ((drv >> 13) & 1U)
      << ";stealth=" << ((drv >> 14) & 1U) << ";fsactive=" << ((drv >> 15) & 1U)
      << ";rampstat=0x" << std::hex << (ramp & 0x3FFFU) << std::dec
      << ";vzero=" << ((ramp >> 10) & 1U)
      << ";pos_reached=" << ((ramp >> 9) & 1U)
      << ";vel_reached=" << ((ramp >> 8) & 1U)
      << ";status_sg=" << ((ramp >> 13) & 1U)
      << ";ioin=0x" << std::hex << ioin << std::dec
      << ";drv_enn=" << ((ioin & kIoinDrvEnn) ? 1 : 0)
      << ";sd_mode=" << ((ioin & kIoinSdMode) ? 1 : 0)
      << ";version=0x" << std::hex << (ioin >> 24) << std::dec
      << ";gstat=0x" << std::hex << (gstat & 0x7U) << std::dec
      << ";chopconf=0x" << std::hex << chop << std::dec
      << ";toff=" << (chop & 0xFU)
      << ";mres=" << mres << ";usteps=" << (256U >> mres)
      << ";resets=" << reset_count_
      << ";pwm_scale_sum=" << (pwm_scale & 0xFFU)
      << ";pwm_scale_auto=" << pwm_scale_auto
      << ";pwm_ofs_auto=" << (pwm_auto & 0xFFU)
      << ";pwm_grad_auto=" << ((pwm_auto >> 16) & 0xFFU);
  return out.str();
}

std::string Tmc5160Driver::warning() const {
  std::string text;
  if (!enable_line_effective_) {
    text = "enable line " + std::to_string(cfg_.enable_line) +
           " has no effect on DRV_ENN (cs=" + std::to_string(cfg_.cs_line) +
           "): the power stage cannot be de-energised through EN; STEPPER_DISABLE"
           " stops the chopper only";
  }
  if (reset_count_ > 0) {
    if (!text.empty()) text += "; ";
    text += "chip reset " + std::to_string(reset_count_) +
            "x since boot (cs=" + std::to_string(cfg_.cs_line) +
            "): VM or VCC_IO dropped and the configuration was lost -- restored"
            " each time, but the motor stalls until then; check the 12 V motor"
            " supply and its current limit";
  }
  return text;
}

bool Tmc5160Driver::RecoverFromChipResetUnlocked(const char* where) {
  std::uint32_t gstat = 0;
  if (!ReadRegister(kRegGSTAT, &gstat)) {
    ReportError(cfg_.spi_device + " cs=" + std::to_string(cfg_.cs_line) +
                ": GSTAT read failed (" + where + ")");
    healthy_ = false;
    return false;
  }
  if ((gstat & kGstatReset) == 0U) return true;
  ++reset_count_;
  std::cerr << "[tmc5160] " << cfg_.spi_device << " cs=" << cfg_.cs_line
            << ": chip reset detected on " << where
            << " (GSTAT=0x" << std::hex << gstat << std::dec
            << ") -- VM or VCC_IO dropped since the chip was configured;"
            << " VMAX/TOFF/currents were back at reset defaults, so the motor"
            << " could not move. Re-initialising (reset #" << reset_count_
            << " since boot). Check the 12 V motor supply.\n";
  const bool was_enabled = enabled_;
  if (!ReinitializeUnlocked()) {
    healthy_ = false;
    return false;
  }
  target_ = 0;  // Reinitialize zeroed XACTUAL/XTARGET; the stall lost the position anyway.
  if (was_enabled && !WriteRegister(kRegCHOPCONF, EncodeChopconf(/*toff=*/3))) {
    healthy_ = false;
    return false;
  }
  return true;
}

bool Tmc5160Driver::Step(bool direction_forward) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!healthy_ || !enabled_) return false;

  if (++steps_since_reset_check_ >= kResetCheckInterval) {
    steps_since_reset_check_ = 0;
    if (!RecoverFromChipResetUnlocked("step")) return false;
  }

  const bool physical_forward = direction_forward != cfg_.invert_direction;
  const std::int64_t delta =
      static_cast<std::int64_t>(DeltaXtarget(microstep_));
  const std::int64_t next =
      static_cast<std::int64_t>(target_) + (physical_forward ? delta : -delta);
  const auto next32 = static_cast<std::int32_t>(next);

  if (!WriteRegister(kRegXTARGET, static_cast<std::uint32_t>(next32))) {
    healthy_ = false;
    return false;
  }
  target_ = next32;
  ++pulses_;
  return true;
}

void Tmc5160Driver::SetMicrostep(int divisor) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!IsSupportedMicrostep(divisor)) {
    healthy_ = false;
    return;
  }
  microstep_ = divisor;
  if (!healthy_) return;  // picked up by the next Reinitialize()

  const std::uint32_t chopconf = EncodeChopconf(enabled_ ? 3 : 0);
  std::uint32_t verify = 0;
  if (!WriteRegister(kRegCHOPCONF, chopconf) ||
      !ReadRegister(kRegCHOPCONF, &verify) || verify != chopconf) {
    healthy_ = false;
  }
}

}  // namespace coatheal
