// Contract tests for Tmc5160Driver: the SPI-only "position dribble" stepper
// backend for schematic v3. STEP/DIR pins are physically unconnected on the
// QHV5160 v2 boards, so every behaviour here is verified against the exact
// bytes the driver puts on the wire, scripted through the strict FakeSpiBus
// (Task 3). All tests run with use_gpio=false: GPIO is a pair of free
// functions with no fake backend, so "nullptr cs/en = test mode" from the
// plan is realized as this explicit flag (see tmc5160_driver.hpp).

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "coatheal/hal/spi_bus_lock.hpp"
#include "coatheal/tmc5160_driver.hpp"
#include "fake_spi_bus.hpp"

using namespace coatheal;

namespace {

// ---------------------------------------------------------------------
// Shared SPI-script helpers
// ---------------------------------------------------------------------

constexpr std::uint8_t kRegGCONF = 0x00;
constexpr std::uint8_t kRegGSTAT = 0x01;
constexpr std::uint8_t kRegIOIN = 0x04;
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
constexpr std::uint8_t kRegCOOLCONF = 0x6D;
constexpr std::uint8_t kRegDRV_STATUS = 0x6F;

// SPI status byte (first byte of every reply) and the register bits the
// step-loss supervision acts on.
constexpr std::uint8_t kStatusReset = 0x01;
constexpr std::uint8_t kStatusDriverError = 0x02;
constexpr std::uint32_t kGstatReset = 0x1U;
constexpr std::uint32_t kGstatDrvErr = 0x2U;
constexpr std::uint32_t kGstatUvCp = 0x4U;
constexpr std::uint32_t kDrvS2vsa = 1U << 12;
constexpr std::uint32_t kDrvOt = 1U << 25;
constexpr std::uint32_t kDrvS2ga = 1U << 27;
constexpr std::uint32_t kDrvOla = 1U << 29;

void ExpectWrite(FakeSpiBus* bus, std::uint8_t addr, std::uint32_t value) {
  std::vector<std::uint8_t> tx = {
      static_cast<std::uint8_t>(addr | 0x80U),
      static_cast<std::uint8_t>((value >> 24) & 0xFFU),
      static_cast<std::uint8_t>((value >> 16) & 0xFFU),
      static_cast<std::uint8_t>((value >> 8) & 0xFFU),
      static_cast<std::uint8_t>(value & 0xFFU)};
  bus->Expect(tx, {0, 0, 0, 0, 0});
}

// A write whose reply carries `status` in the SPI status byte.
void ExpectWriteWithStatus(FakeSpiBus* bus, std::uint8_t addr,
                           std::uint32_t value, std::uint8_t status) {
  std::vector<std::uint8_t> tx = {
      static_cast<std::uint8_t>(addr | 0x80U),
      static_cast<std::uint8_t>((value >> 24) & 0xFFU),
      static_cast<std::uint8_t>((value >> 16) & 0xFFU),
      static_cast<std::uint8_t>((value >> 8) & 0xFFU),
      static_cast<std::uint8_t>(value & 0xFFU)};
  bus->Expect(tx, {status, 0, 0, 0, 0});
}

// Two-phase read: phase 1 latches the address (reply content is whatever the
// chip was clocking out from the *previous* conversation -- ignored by the
// driver, scripted as all-zero here), phase 2 clocks out the data for THIS
// request.
void ExpectRead(FakeSpiBus* bus, std::uint8_t addr, std::uint32_t value) {
  std::vector<std::uint8_t> tx = {static_cast<std::uint8_t>(addr & 0x7FU), 0,
                                  0, 0, 0};
  bus->Expect(tx, {0, 0, 0, 0, 0});
  std::vector<std::uint8_t> rx = {
      0,  // SPI status byte, unused by the driver
      static_cast<std::uint8_t>((value >> 24) & 0xFFU),
      static_cast<std::uint8_t>((value >> 16) & 0xFFU),
      static_cast<std::uint8_t>((value >> 8) & 0xFFU),
      static_cast<std::uint8_t>(value & 0xFFU)};
  bus->Expect(tx, rx);
}

std::uint32_t Chopconf(int microstep, std::uint8_t toff) {
  // MRES is pinned to 0 (native 256 microsteps) whatever the firmware's
  // divisor: the motion controller's units follow MRES (see EncodeChopconf).
  (void)microstep;
  const std::uint8_t mres = 0;
  return (static_cast<std::uint32_t>(mres) << 24) | (0x2U << 15) |
         (0x4U << 4) | static_cast<std::uint32_t>(toff & 0x0FU);
}

// Scripts one full Reinitialize() conversation for `cfg` -- IOIN version
// probe through the GCONF+CHOPCONF readback verify, in the exact order
// tmc5160_driver.cpp writes them -- using `ioin_version_byte` as the IOIN
// VERSION byte (31:24). Building blocks (CalculateCurrent, EncodeMres) are
// the driver's own *tested elsewhere* static helpers -- reused here to keep
// this integration script honest about what the driver actually computes,
// not to re-derive their correctness (that's Test 3/4/1's job).
//
// Scripting the FULL sequence even for the wrong-version case is
// deliberate: it's what makes the version-gate test load-bearing. A driver
// that dropped the gate would sail through the identical rest of the
// sequence (nothing past IOIN depends on the version byte) and come up
// healthy; a driver with the gate intact must stop at IOIN and leave
// everything after it unconsumed.
void ScriptReinitSequence(FakeSpiBus* bus, const Tmc5160Config& cfg,
                          std::uint8_t ioin_version_byte,
                          std::uint8_t ioin_pin_bits = 0,
                          std::uint8_t toff = 0) {
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool current_ok = Tmc5160Driver::CalculateCurrent(
      cfg.run_current_a_rms, cfg.sense_resistor_ohm, cfg.hold_current_frac,
      &globalscaler, &irun, &ihold);
  assert(current_ok);

  // GCONF is exactly the en_pwm_mode bit (bit 2) or nothing: every other
  // GCONF bit stays at its reset value of 0. Derived from cfg here rather
  // than hardcoded, so the stealth_chop fixtures below script what the
  // driver must actually write.
  const std::uint32_t gconf = cfg.stealth_chop ? 0x00000004U : 0x00000000U;
  // The chopper comes out of (re)initialisation in the motor's enabled
  // state: TOFF=0 for a disabled motor (boot, CHECK), 3 when enabled.
  const std::uint32_t chopconf_run = Chopconf(cfg.microstep, toff);
  const std::uint32_t gs_reg = globalscaler >= 256U ? 0U : globalscaler;
  const std::uint32_t ihold_irun =
      (static_cast<std::uint32_t>(ihold) & 0x1FU) |
      ((static_cast<std::uint32_t>(irun) & 0x1FU) << 8) | (6U << 16);

  ExpectRead(bus, kRegIOIN,
            (static_cast<std::uint32_t>(ioin_version_byte) << 24) |
                static_cast<std::uint32_t>(ioin_pin_bits));
  ExpectWrite(bus, kRegGCONF, gconf);
  ExpectWrite(bus, kRegCHOPCONF, chopconf_run);
  // StallGuard2 threshold + filter; coolStep stays off.
  ExpectWrite(bus, kRegCOOLCONF, Tmc5160Driver::EncodeCoolconf(cfg.stallguard_sgt));
  ExpectWrite(bus, kRegGLOBALSCALER, gs_reg);
  ExpectWrite(bus, kRegIHOLD_IRUN, ihold_irun);
  ExpectWrite(bus, kRegTPOWERDOWN, 10U);
  ExpectWrite(bus, kRegRAMPMODE, 0U);
  ExpectWrite(bus, kRegVSTART, 0U);
  ExpectWrite(bus, kRegVSTOP, 10U);
  ExpectWrite(bus, kRegA1, 0xFFFFU);
  ExpectWrite(bus, kRegAMAX, 0xFFFFU);
  ExpectWrite(bus, kRegV1, 0U);
  ExpectWrite(bus, kRegD1, 0xFFFFU);
  ExpectWrite(bus, kRegDMAX, 0xFFFFU);
  ExpectWrite(bus, kRegVMAX, 4U * 100U * 256U);
  ExpectWrite(bus, kRegXACTUAL, 0U);
  ExpectWrite(bus, kRegXTARGET, 0U);
  ExpectRead(bus, kRegGCONF, gconf);
  ExpectRead(bus, kRegCHOPCONF, chopconf_run);
  ExpectWrite(bus, kRegGSTAT, 0x7U);  // clear reset/drv_err/uv_cp: config is on the chip now
}

// Total Expect() entries ScriptReinitSequence() queues: IOIN (2) + 17
// register writes + GCONF/CHOPCONF readback (2*2) + the GSTAT clear (1). Used to prove the
// version-gate test stops exactly at IOIN, not "eventually, somehow".
constexpr std::size_t kFullReinitExpectationCount = 24;

void ScriptHealthyReinit(FakeSpiBus* bus, const Tmc5160Config& cfg,
                         std::uint8_t toff = 0) {
  ScriptReinitSequence(bus, cfg, /*ioin_version_byte=*/0x30, /*ioin_pin_bits=*/0, toff);
}

std::unique_ptr<Tmc5160Driver> MakeHealthyDriver(FakeSpiBus* bus,
                                                 const Tmc5160Config& cfg) {
  ScriptHealthyReinit(bus, cfg);
  auto driver = std::make_unique<Tmc5160Driver>(cfg, bus, /*use_gpio=*/false);
  assert(driver->healthy());
  assert(bus->mismatch_count() == 0);
  assert(bus->remaining_expectations() == 0);
  // Load-bearing: on real hardware, opening with the wrong mode/no_cs
  // corrupts every datagram and lets the kernel drive a native chip-select
  // into a MAX31865 click mid-motor-traffic. FakeSpiBus's Transfer() never
  // checks these against Open() -- only these explicit assertions do.
  assert(bus->open_device() == cfg.spi_device);
  assert(bus->open_mode() == 3);  // SPI mode 3
  assert(bus->open_speed_hz() == cfg.spi_speed_hz);
  assert(bus->open_no_cs() == true);  // soft CS: CE0/CE1 belong to the clicks
  return driver;
}

// Scripts the extra conversation Enable(true) issues unconditionally when
// the driver is already healthy_ (no Reinitialize()): the CHOPCONF(TOFF=3)
// write, then the DRV_STATUS thermal read that re-primes the ot/otpw
// tracking after the shutdown latch is cleared (2026-08-29).
void ScriptEnableTrueChopconf(FakeSpiBus* bus, const Tmc5160Config& cfg,
                              std::uint32_t drv_status = 0) {
  ExpectWrite(bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/3));
  ExpectRead(bus, kRegDRV_STATUS, drv_status);
}

// Enable(true) on a healthy, idle driver: GSTAT clean, TOFF=3, DRV_STATUS.
void EnableHealthy(FakeSpiBus* bus, Tmc5160Driver* driver,
                   const Tmc5160Config& cfg) {
  ExpectRead(bus, kRegGSTAT, 0U);
  ScriptEnableTrueChopconf(bus, cfg);
  assert(driver->Enable(true));
  assert(bus->mismatch_count() == 0);
  assert(bus->remaining_expectations() == 0);
}

// One idle Poll(): GSTAT, DRV_STATUS, then XACTUAL against the target.
void ScriptIdlePoll(FakeSpiBus* bus, std::uint32_t gstat, std::uint32_t drv_status,
                    std::uint32_t xactual) {
  ExpectRead(bus, kRegGSTAT, gstat);
  ExpectRead(bus, kRegDRV_STATUS, drv_status);
  ExpectRead(bus, kRegXACTUAL, xactual);
}

// `count` plain forward steps at divisor 4 from `*target` (64 units each).
void StepForward(FakeSpiBus* bus, Tmc5160Driver* driver, int count,
                 std::int32_t* target) {
  for (int i = 0; i < count; ++i) {
    *target += 64;
    ExpectWrite(bus, kRegXTARGET, static_cast<std::uint32_t>(*target));
    assert(driver->Step(true));
  }
}

// ---------------------------------------------------------------------
// EncodeMres
// ---------------------------------------------------------------------

void TestEncodeMresTable() {
  assert(Tmc5160Driver::EncodeMres(256) == 0);
  assert(Tmc5160Driver::EncodeMres(128) == 1);
  assert(Tmc5160Driver::EncodeMres(64) == 2);
  assert(Tmc5160Driver::EncodeMres(32) == 3);
  assert(Tmc5160Driver::EncodeMres(16) == 4);
  assert(Tmc5160Driver::EncodeMres(8) == 5);
  assert(Tmc5160Driver::EncodeMres(4) == 6);
  assert(Tmc5160Driver::EncodeMres(2) == 7);
  assert(Tmc5160Driver::EncodeMres(1) == 8);
}

void TestEncodeMresRejectsInvalidDivisor() {
  assert(Tmc5160Driver::EncodeMres(3) == Tmc5160Driver::kInvalidMres);
  assert(Tmc5160Driver::EncodeMres(0) == Tmc5160Driver::kInvalidMres);
  assert(Tmc5160Driver::EncodeMres(-4) == Tmc5160Driver::kInvalidMres);
  assert(Tmc5160Driver::EncodeMres(512) == Tmc5160Driver::kInvalidMres);
}

// ---------------------------------------------------------------------
// DeltaXtarget
// ---------------------------------------------------------------------

void TestDeltaXtargetTable() {
  assert(Tmc5160Driver::DeltaXtarget(4) == 64U);
  assert(Tmc5160Driver::DeltaXtarget(16) == 16U);
  assert(Tmc5160Driver::DeltaXtarget(256) == 1U);
  assert(Tmc5160Driver::DeltaXtarget(1) == 256U);
}

void TestDeltaXtargetRejectsInvalidDivisor() {
  assert(Tmc5160Driver::DeltaXtarget(3) == 0U);
  assert(Tmc5160Driver::DeltaXtarget(0) == 0U);
}

// ---------------------------------------------------------------------
// CalculateCurrent -- hand-computed cases (see task-4-report.md for the
// full derivation). Both cases hit the [32,256] GLOBALSCALER band; case 2
// is deliberately in the >50%-of-max-current regime, which is the one that
// forces GLOBALSCALER to pin at 256 and IRUN to drop below its max of 31.
// ---------------------------------------------------------------------

void TestCalculateCurrentLowRegimeKeepsIrunAtMax() {
  // a_rms=0.8, R=0.075 -> I_peak=1.13137 A, I_peak_max=4.33333 A,
  // fraction=0.26109 (<=0.5) -> IRUN stays at 31, GLOBALSCALER=round(256*
  // fraction)=67. IHOLD=round(31*0.30)=9.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool ok = Tmc5160Driver::CalculateCurrent(
      /*a_rms=*/0.8, /*sense_ohm=*/0.075, /*hold_frac=*/0.30, &globalscaler,
      &irun, &ihold);
  assert(ok);
  assert(globalscaler == 67U);
  assert(irun == 31);
  assert(ihold == 9);
  assert(globalscaler >= 32U && globalscaler <= 256U);

  // Reconstructed current stays within 5% of the 1.13137 A target:
  // (67/256)*(32/32)*(0.325/0.075) = 1.134... A.
  const double reconstructed =
      (static_cast<double>(globalscaler) / 256.0) *
      ((static_cast<double>(irun) + 1.0) / 32.0) * (0.325 / 0.075);
  const double target = 0.8 * 1.4142135623730951;
  const double rel_err = std::abs(reconstructed - target) / target;
  assert(rel_err < 0.05);
}

void TestCalculateCurrentHighRegimeReducesIrun() {
  // a_rms=2.0, R=0.075 -> I_peak=2.82843 A, I_peak_max=4.33333 A,
  // fraction=0.65296 (>0.5) -> GLOBALSCALER pins at 256, IRUN=round(32*
  // 0.65296)-1 = round(20.8948)-1 = 20 (reduced from the max of 31).
  // IHOLD=round((IRUN+1)*hold_frac)-1=round(21*0.30)-1=round(6.3)-1=5.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool ok = Tmc5160Driver::CalculateCurrent(
      /*a_rms=*/2.0, /*sense_ohm=*/0.075, /*hold_frac=*/0.30, &globalscaler,
      &irun, &ihold);
  assert(ok);
  assert(globalscaler == 256U);
  assert(irun == 20);  // < 31: forced IRUN reduction
  assert(ihold == 5);
  assert(globalscaler >= 32U && globalscaler <= 256U);

  // Reconstructed current: (256/256)*(21/32)*(0.325/0.075) = 2.84375 A,
  // within 5% of the 2.82843 A (~2.8 A peak) target.
  const double reconstructed =
      (static_cast<double>(globalscaler) / 256.0) *
      ((static_cast<double>(irun) + 1.0) / 32.0) * (0.325 / 0.075);
  const double target = 2.0 * 1.4142135623730951;
  const double rel_err = std::abs(reconstructed - target) / target;
  assert(rel_err < 0.05);
}

void TestCalculateCurrentRejectsUnreachableTarget() {
  // a_rms=5.0, R=0.075 -> I_peak=7.071 A > I_peak_max=4.333 A: no choice of
  // GLOBALSCALER/IRUN can reach it: neither factor can exceed its maximum.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  assert(!Tmc5160Driver::CalculateCurrent(5.0, 0.075, 0.30, &globalscaler,
                                          &irun, &ihold));
}

void TestCalculateCurrentRejectsInvalidInputs() {
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  assert(!Tmc5160Driver::CalculateCurrent(0.0, 0.075, 0.30, &globalscaler,
                                          &irun, &ihold));
  assert(!Tmc5160Driver::CalculateCurrent(0.8, 0.0, 0.30, &globalscaler,
                                          &irun, &ihold));
  assert(!Tmc5160Driver::CalculateCurrent(0.8, 0.075, 1.5, &globalscaler,
                                          &irun, &ihold));
  assert(!Tmc5160Driver::CalculateCurrent(0.8, 0.075, -0.1, &globalscaler,
                                          &irun, &ihold));
}

void TestCalculateCurrentLowCurrentFloorReducesIrun() {
  // a_rms=0.1, R=0.075 -> I_peak=0.14142 A, I_peak_max=4.33333 A,
  // fraction=0.032636 -> at IRUN=31 GLOBALSCALER would be round(256*
  // 0.032636)=8, under the 32 floor. GLOBALSCALER floor branch: pin
  // GLOBALSCALER=32, IRUN=round(256*0.14142*0.075/0.325)-1 =
  // round(8.3548)-1 = 8-1 = 7 (reduced from the max of 31).
  // IHOLD=round((7+1)*0.30)-1=round(2.4)-1=1.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool ok = Tmc5160Driver::CalculateCurrent(
      /*a_rms=*/0.1, /*sense_ohm=*/0.075, /*hold_frac=*/0.30, &globalscaler,
      &irun, &ihold);
  assert(ok);
  assert(globalscaler == 32U);
  assert(irun == 7);  // < 31: forced IRUN reduction (low-current mirror)
  assert(ihold == 1);

  // Reconstructed current: (32/256)*(8/32)*(0.325/0.075) = 0.135417 A_peak
  // = 0.095766 A_rms, about -4.3% vs. the 0.1 A_rms target -- an
  // acceptable undershoot, not the +283% overcurrent the pre-fix code
  // would have delivered by clamping GLOBALSCALER to 32 while leaving
  // IRUN at 31 ((32/256)*(32/32)*4.33333 = 0.54167 A_peak = 0.38314 A_rms).
  const double reconstructed_peak =
      (static_cast<double>(globalscaler) / 256.0) *
      ((static_cast<double>(irun) + 1.0) / 32.0) * (0.325 / 0.075);
  const double target_peak = 0.1 * 1.4142135623730951;
  const double rel_err =
      std::abs(reconstructed_peak - target_peak) / target_peak;
  assert(rel_err < 0.10);
}

void TestCalculateCurrentRejectsUltraLowCurrent() {
  // a_rms=0.02, R=0.075 -> I_peak=0.028284 A. GLOBALSCALER-floor solve
  // gives IRUN=round(256*0.028284*0.075/0.325)-1=round(1.67095)-1=2-1=1.
  // Delivered at GS=32,IRUN=1: (32/256)*(2/32)*4.33333 = 0.033854 A_peak,
  // a ~19.7% overshoot vs. the 0.028284 A_peak target -- past the 10%
  // tolerance, so this must be rejected outright rather than silently
  // overcurrenting by nearly 20%.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  assert(!Tmc5160Driver::CalculateCurrent(0.02, 0.075, 0.30, &globalscaler,
                                          &irun, &ihold));
}

void TestCalculateCurrentIholdEndpoints() {
  // hold_frac=0 must give IHOLD=0 (no standstill current) regardless of
  // IRUN; hold_frac=1 must give IHOLD==IRUN (full run current held).
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  assert(Tmc5160Driver::CalculateCurrent(0.8, 0.075, /*hold_frac=*/0.0,
                                         &globalscaler, &irun, &ihold));
  assert(irun == 31);
  assert(ihold == 0);

  assert(Tmc5160Driver::CalculateCurrent(0.8, 0.075, /*hold_frac=*/1.0,
                                         &globalscaler, &irun, &ihold));
  assert(irun == 31);
  assert(ihold == irun);
}

void TestCalculateCurrentLowCurrentSweepReconstructsWithinTolerance() {
  // Property check across the GLOBALSCALER-floor branch's operating range
  // (all below the 12.5%-of-max threshold that triggers it at R=0.075):
  // whatever GLOBALSCALER/IRUN the driver picks, the delivered current
  // must reconstruct within +-10% of what was asked for. Independent of
  // any specific IRUN/GLOBALSCALER pinned value -- a property the fixed
  // algorithm must hold, not a re-derivation of it.
  const double sense_ohm = 0.075;
  const double a_rms_values[] = {0.05, 0.1, 0.2, 0.3};
  for (double a_rms : a_rms_values) {
    std::uint32_t globalscaler = 0;
    std::uint8_t irun = 0;
    std::uint8_t ihold = 0;
    const bool ok = Tmc5160Driver::CalculateCurrent(
        a_rms, sense_ohm, 0.30, &globalscaler, &irun, &ihold);
    assert(ok);
    assert(globalscaler >= 32U && globalscaler <= 256U);
    const double reconstructed_peak =
        (static_cast<double>(globalscaler) / 256.0) *
        ((static_cast<double>(irun) + 1.0) / 32.0) * (0.325 / sense_ohm);
    const double target_peak = a_rms * 1.4142135623730951;
    const double rel_err =
        std::abs(reconstructed_peak - target_peak) / target_peak;
    assert(rel_err < 0.10);
  }
}

// ---------------------------------------------------------------------
// Version gate
// ---------------------------------------------------------------------

void TestVersionGateAcceptsTmc5160Version() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  assert(driver->healthy());
}

void TestVersionGateRejectsTmc2240Version() {
  // The TMC2240's IOIN VERSION byte (0x40) must NOT be accepted here --
  // this is the exact wrong-chip-family mixup the version gate exists to
  // catch. The FULL healthy sequence is scripted (see
  // ScriptReinitSequence's comment): everything past the IOIN exchange is
  // byte-for-byte identical to what a genuine TMC5160 conversation would
  // need, since none of it depends on the version byte. This is what makes
  // the test load-bearing -- a driver missing the gate would consume the
  // whole script and end up healthy(), not merely "unhealthy for some
  // unrelated reason".
  FakeSpiBus bus;
  Tmc5160Config cfg;
  ScriptReinitSequence(&bus, cfg, /*ioin_version_byte=*/0x40);

  Tmc5160Driver driver(cfg, &bus, /*use_gpio=*/false);
  assert(!driver.healthy());
  assert(bus.mismatch_count() == 0);
  // Only IOIN's 2 phases were consumed; the gate stopped Reinitialize()
  // before anything else was sent.
  assert(bus.remaining_expectations() == kFullReinitExpectationCount - 2);
}

// ---------------------------------------------------------------------
// Step(): XTARGET writes
// ---------------------------------------------------------------------

void TestStepForwardThenReverseAtDivisor4() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;  // Delta = 256/4 = 64
  auto driver = MakeHealthyDriver(&bus, cfg);

  ExpectRead(&bus, kRegGSTAT, 0U);  // reset check first
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));

  ExpectWrite(&bus, kRegXTARGET, 64U);
  ExpectWrite(&bus, kRegXTARGET, 128U);
  ExpectWrite(&bus, kRegXTARGET, 192U);
  ExpectWrite(&bus, kRegXTARGET, 128U);
  ExpectWrite(&bus, kRegXTARGET, 64U);

  assert(driver->Step(true));
  assert(driver->Step(true));
  assert(driver->Step(true));
  assert(driver->Step(false));
  assert(driver->Step(false));

  // 64 * (3 forward - 2 reverse) = 64.
  assert(driver->target() == 64);
  assert(driver->pulses_issued() == 5U);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

void TestStepHonoursInvertDirection() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  cfg.invert_direction = true;
  auto driver = MakeHealthyDriver(&bus, cfg);

  ExpectRead(&bus, kRegGSTAT, 0U);  // reset check first
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));

  // direction_forward=true with invert_direction=true must move XTARGET
  // *backward* (-64), the opposite of the non-inverted case above.
  ExpectWrite(&bus, kRegXTARGET, 0xFFFFFFC0U);  // -64 as int32
  assert(driver->Step(true));
  assert(driver->target() == -64);

  ExpectWrite(&bus, kRegXTARGET, 0x00000000U);
  assert(driver->Step(false));
  assert(driver->target() == 0);

  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Enable(false): freeze order
// ---------------------------------------------------------------------

void TestEnableFalseFreezesInOrder() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);

  ExpectRead(&bus, kRegGSTAT, 0U);  // reset check first
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));
  assert(driver->enabled());

  // Order under test: XACTUAL read (2 transfers) -> XTARGET=XACTUAL write
  // -> CHOPCONF TOFF=0 write. The strict FakeSpiBus queue enforces this
  // exact order: any reordering sends the wrong tx bytes against the front
  // expectation and is caught as a mismatch.
  ExpectRead(&bus, kRegXACTUAL, 128U);
  ExpectWrite(&bus, kRegXTARGET, 128U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/0));

  assert(driver->Enable(false));
  assert(!driver->enabled());
  assert(driver->target() == 128);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Enable line verification through IOIN.DRV_ENN
// ---------------------------------------------------------------------

// Enable(false) must prove the EN line can DISABLE the chip: with DRV_ENN
// still LOW afterwards the motor is usable but STEPPER_DISABLE cannot
// de-energise it through EN, so the driver reports a warning (bench,
// 2026-08-28: motor1's module). With DRV_ENN HIGH the line is effective
// and the warning clears. Enable(true) keeps refusing a line that leaves
// DRV_ENN HIGH.
void TestEnableFalseDetectsIneffectiveEnableLine() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  driver->set_verify_enable_line(true);
  assert(driver->enable_line_effective());
  assert(driver->warning().empty());

  // Enable(true): GSTAT reset check, IOIN verify (DRV_ENN=0, enabled), then CHOPCONF TOFF=3.
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegIOIN, 0x30000000U);
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));

  // Enable(false) with a line that has no effect: DRV_ENN stays 0.
  ExpectRead(&bus, kRegXACTUAL, 64U);
  ExpectWrite(&bus, kRegXTARGET, 64U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/0));
  ExpectRead(&bus, kRegIOIN, 0x30000000U);
  assert(driver->Enable(false));
  assert(!driver->enabled());
  assert(driver->healthy());  // a warning, not a fault
  assert(!driver->enable_line_effective());
  assert(driver->warning().find("enable line") != std::string::npos);
  assert(driver->warning().find("DRV_ENN") != std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);

  // Same cycle with a working line: DRV_ENN=1 after disabling.
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegIOIN, 0x30000000U);
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));
  ExpectRead(&bus, kRegXACTUAL, 64U);
  ExpectWrite(&bus, kRegXTARGET, 64U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/0));
  ExpectRead(&bus, kRegIOIN, 0x30000010U);
  assert(driver->Enable(false));
  assert(driver->enable_line_effective());
  assert(driver->warning().empty());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);

  // Enable(true) with DRV_ENN still HIGH is a refusal, as before.
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegIOIN, 0x30000010U);
  assert(!driver->Enable(true));
  assert(!driver->healthy());
  assert(driver->last_error().find("DRV_ENN still HIGH") != std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Chip reset detection (bench 2026-08-29: CHOPCONF read the power-on value
// 0x10410150 while the firmware believed the motor was enabled -- VMAX=0,
// so XTARGET moved and XACTUAL never did).
// ---------------------------------------------------------------------

void TestChipResetOnEnableIsRecovered() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  assert(bus.remaining_expectations() == 0);
  // Enable(true): GSTAT says reset -> full re-initialisation, then TOFF=3.
  ExpectRead(&bus, kRegGSTAT, 0x1U);
  ScriptHealthyReinit(&bus, cfg);
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->reset_count() == 1);
  assert(driver->warning().find("chip reset 1x") != std::string::npos);
  // The motor was not energised when the chip reset: nothing was holding a
  // position, so this is not a step-loss event.
  assert(driver->step_loss_events() == 0);
  assert(driver->DebugRegisters().empty());  // no script -> nothing, but resets= is wired:
  // MUTATION: make RecoverFromChipResetUnlocked ignore GSTAT bit 0 and
  // confirm this test fails on mismatch_count (the reinit never happens).
}

void TestChipResetAtIdleIsRecoveredByPoll() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  // Disabled: Poll() does not touch the bus at all.
  assert(driver->Poll());
  assert(bus.remaining_expectations() == 0);
  EnableHealthy(&bus, driver.get(), cfg);
  // Enabled and idle: a reset since the last check is repaired in place,
  // chopper restored (bench 2026-08-29: M1's chip reset right after its
  // move ended and sat with TOFF=0, holding nothing).
  ExpectRead(&bus, kRegGSTAT, 0x1U);
  ScriptHealthyReinit(&bus, cfg, /*toff=*/3);  // enabled: chopper restored by the reinit itself
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  ExpectRead(&bus, kRegXACTUAL, 0U);
  assert(driver->Poll());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->reset_count() == 1);
  assert(driver->enabled());
  // The motor was energised and let go: the position is no longer known.
  // Nothing was moving, so there is no move to stop.
  assert(driver->step_loss_events() == 1);
  assert(driver->step_loss_reason().find("chip reset") != std::string::npos);
  assert(!driver->TakeStepLossStop());
}

void TestChipResetMidMoveIsRecoveredWithinTheCheckInterval() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  // 63 plain steps, then the 64th re-reads GSTAT and finds the chip reset:
  // configuration rewritten, chopper restored, and the step continues from
  // the fresh XACTUAL=0 (target 64, not 64*64).
  for (int i = 1; i <= 63; ++i) {
    ExpectWrite(&bus, kRegXTARGET, static_cast<std::uint32_t>(64 * i));
    assert(driver->Step(true));
  }
  ExpectRead(&bus, kRegGSTAT, 0x1U);
  ScriptHealthyReinit(&bus, cfg, /*toff=*/3);  // enabled: chopper restored by the reinit itself
  // The same 64-step check also samples DRV_STATUS (0x6F) for the die
  // thermal flags before the step continues.
  ExpectRead(&bus, 0x6F, 0U);
  ExpectWrite(&bus, kRegXTARGET, 64U);
  assert(driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->target() == 64);
  assert(driver->reset_count() == 1);
  assert(driver->enabled());
  // A step-loss event, and the move goes on: stopping would not bring the
  // lost steps back, and under link loss nobody could resume it.
  assert(driver->step_loss_events() == 1);
  assert(!driver->TakeStepLossStop());
}

// A disabled motor must come out of (re)initialisation with the chopper
// OFF: on the motor-1 module EN does not reach DRV_ENN, so TOFF is the only
// thing keeping its power stage de-energised at boot and after every CHECK.
void TestReinitializeKeepsChopperOffWhileDisabled() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);   // constructor: disabled -> TOFF=0 scripted
  assert(bus.mismatch_count() == 0);
  ScriptHealthyReinit(&bus, cfg, /*toff=*/0);
  assert(driver->ActiveCheck());                // CHECK while disabled
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  // MUTATION: hard-code toff=3 in ReinitializeUnlocked and confirm this
  // test fails on mismatch_count.
}

// ---------------------------------------------------------------------
// MOTOR_DEBUG register read-out
// ---------------------------------------------------------------------

void TestDebugRegistersDecodeMotionTruth() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  // The ten reads, in the order DebugRegisters() issues them.
  ExpectRead(&bus, 0x21, 0xFFFFFFFBU);   // XACTUAL = -5
  ExpectRead(&bus, 0x2D, 800U);          // XTARGET
  ExpectRead(&bus, 0x22, 0x00FFFFFDU);   // VACTUAL 24-bit = -3
  ExpectRead(&bus, 0x6A, 544U);          // MSCNT
  // DRV_STATUS: stst=1, ola=1, cs_actual=9, stealth=1, sg_result=0x12
  ExpectRead(&bus, 0x6F, (1U << 31) | (1U << 29) | (9U << 16) | (1U << 14) | 0x12U);
  ExpectRead(&bus, 0x35, (1U << 10) | (1U << 9));  // RAMPSTAT vzero + position_reached
  ExpectRead(&bus, 0x12, 1234U);         // TSTEP
  ExpectRead(&bus, 0x04, 0x30000010U);   // IOIN: version 0x30, DRV_ENN=1
  ExpectRead(&bus, 0x01, 0x5U);          // GSTAT reset + uv_cp
  ExpectRead(&bus, 0x6C, 0x06010040U);   // CHOPCONF toff=0, mres=6 (µ4)
  ExpectRead(&bus, 0x71, (0x1F0U << 16) | 0xFFU);  // PWM_SCALE: sum saturated, auto=-16
  ExpectRead(&bus, 0x72, (0x0CU << 16) | 0x1EU);   // PWM_AUTO: grad 12, ofs 30
  const std::string kv = driver->DebugRegisters();
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  auto has = [&](const char* needle) { return kv.find(needle) != std::string::npos; };
  assert(has("xactual=-5;"));
  assert(has(";xtarget=800;"));
  assert(has(";vactual=-3;"));
  assert(has(";mscnt=544;"));
  assert(has(";stst=1;"));
  assert(has(";cs_actual=9;"));
  assert(has(";sg_result=18;"));
  assert(has(";ola=1;"));
  assert(has(";olb=0;"));
  assert(has(";stealth=1;"));
  assert(has(";vzero=1;"));
  assert(has(";pos_reached=1;"));
  assert(has(";tstep=1234;"));
  assert(has(";drv_enn=1;"));
  assert(has(";sd_mode=0;"));
  assert(has(";version=0x30;"));
  assert(has(";gstat=0x5;"));
  assert(has(";toff=0;"));
  assert(has(";mres=6;usteps=4"));
  assert(has(";pwm_scale_sum=255;pwm_scale_auto=-16;pwm_ofs_auto=30;pwm_grad_auto=12"));
  // Step-loss supervision statistics trail the registers; "-" until the
  // first StallGuard sample of a move.
  assert(has(";drv_loss=0;stall_mode=monitor;sgt=0;sg_thr=0;sg_last=-;sg_min=-;sg_n=0"
             ";stalls=0;uv=0;shorts=0;openload=0;xt_repairs=0"));
  // A bus failure mid-read yields nothing rather than a half-decoded lie.
  bus.FailNextTransfers(1);
  assert(driver->DebugRegisters().empty());
}

// ---------------------------------------------------------------------
// Transfer failure -> unhealthy; ActiveCheck() re-probes
// ---------------------------------------------------------------------

void TestTransferFailureMarksUnhealthyAndActiveCheckReprobes() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);

  // Force the very next transfer (IOIN's first phase, inside the
  // ActiveCheck->Reinitialize probe below) to fail at the transport layer.
  bus.FailNextTransfers(1);
  assert(!driver->ActiveCheck());
  assert(!driver->healthy());
  // The injected failure short-circuits before any expectation is touched.
  assert(bus.mismatch_count() == 0);

  // Re-probe: ActiveCheck() -> Reinitialize() must be able to recover once
  // the bus is healthy again, without needing to reconstruct the driver.
  ScriptHealthyReinit(&bus, cfg);
  assert(driver->ActiveCheck());
  assert(driver->healthy());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// SetMicrostep: invalid divisor
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// C1 + C2 (motor side): every 5-byte datagram must be ONE hold of the
// per-CONTROLLER bus lock, with this driver's own mode/speed re-applied
// inside that hold.
//
// Without the hold, a MAX31865 conversation can interleave between the
// motor's cs-low, its data ioctl and its cs-high (three syscalls) — the
// motor then latches a garbage datagram on CS rise, possibly as a WRITE
// (bit 7 of byte 0), while both chips drive MISO.
//
// Hand-computed expectations, pinned before the assertions:
//   * divisor 4 -> Delta = 256/4 = 64; one Step(true) from target 0 writes
//     XTARGET = 64, which is exactly ONE Transfer() (one WriteRegister).
//   * therefore: lock acquisitions +1, settings applications +1.
//   * the click on /dev/spidev0.1 shares this controller, so reading the
//     counter through THAT path must show the same value — that is the
//     canonicalisation under test.
// ---------------------------------------------------------------------

void TestEachDatagramIsOneControllerLockHoldWithModeReapplied() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.spi_device = "/dev/spidev0.0";
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);

  ExpectRead(&bus, kRegGSTAT, 0U);  // reset check first
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));

  const std::uint64_t locks_before = SpiBusLockAcquireCount(cfg.spi_device);
  const int applies_before = bus.settings_applications();

  ExpectWrite(&bus, kRegXTARGET, 64U);
  assert(driver->Step(true));

  assert(SpiBusLockAcquireCount(cfg.spi_device) == locks_before + 1);
  assert(bus.settings_applications() == applies_before + 1);

  // The click's device node must reach the SAME counter: one controller,
  // one lock. (Keying the mutex by raw device string breaks this line.)
  assert(SpiBusLockAcquireCount("/dev/spidev0.1") == locks_before + 1);

  // Re-applied settings are the motor's own, not a click's.
  assert(bus.applied_mode() == 3);
  assert(bus.applied_no_cs() == true);
  assert(bus.applied_speed_hz() == cfg.spi_speed_hz);

  assert(driver->target() == 64);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// I3: motorN.stealth_chop reaches the wire.
//
// The key was parsed and validated but the driver wrote GCONF's en_pwm_mode
// bit ON unconditionally, so setting it false did nothing. Both fixtures are
// scripted through the strict FakeSpiBus, so the assertion is on the exact
// bytes GCONF is written with -- and the driver's existing GCONF readback
// verify means a wrong value would also fail bring-up.
//
// Hand-computed, pinned before the assertions:
//   stealth_chop = true  -> GCONF = 0x00000004 (bit 2 set)
//   stealth_chop = false -> GCONF = 0x00000000 (all reset values)
// On the wire that is `0x80 0x00 0x00 0x00 0x04` vs
// `0x80 0x00 0x00 0x00 0x00` (address 0x00 | write bit).
// ---------------------------------------------------------------------

void TestStealthChopSelectsGconfEnPwmModeBit() {
  {
    FakeSpiBus bus;
    Tmc5160Config cfg;
    cfg.stealth_chop = true;
    // MakeHealthyDriver scripts GCONF = 0x00000004 for this fixture and
    // FakeSpiBus rejects any other tx bytes, so a driver writing the wrong
    // value cannot reach healthy().
    auto driver = MakeHealthyDriver(&bus, cfg);
    assert(driver->healthy());
    assert(bus.mismatch_count() == 0);
    assert(bus.remaining_expectations() == 0);
  }
  {
    FakeSpiBus bus;
    Tmc5160Config cfg;
    cfg.stealth_chop = false;
    // Same sequence, GCONF = 0x00000000. Before the wire-through this
    // fixture could not come up healthy: the driver wrote 0x04 into a script
    // expecting 0x00, so the Expect() mismatched and Reinitialize failed.
    auto driver = MakeHealthyDriver(&bus, cfg);
    assert(driver->healthy());
    assert(bus.mismatch_count() == 0);
    assert(bus.remaining_expectations() == 0);
  }
}

void TestSetMicrostepRejectsInvalidDivisor() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);

  // Script exactly what an UNGUARDED SetMicrostep(3) would emit: EncodeMres
  // falls back to safe_mres=6 for any invalid divisor, so the write+
  // readback it would send is indistinguishable from a genuine divisor=4
  // CHOPCONF write (driver isn't enabled_ yet, so TOFF=0). Without this
  // script, an unguarded call fails anyway (empty queue) and lands on the
  // same !healthy() outcome for the wrong reason, so the guard's absence
  // goes undetected -- scripting it is what makes this load-bearing.
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(/*microstep=*/4, /*toff=*/0));
  ExpectRead(&bus, kRegCHOPCONF, Chopconf(/*microstep=*/4, /*toff=*/0));
  const std::size_t before = bus.remaining_expectations();

  driver->SetMicrostep(3);  // not a supported power-of-two divisor

  assert(!driver->healthy());
  assert(driver->microstep() == 4);  // unchanged from cfg's default
  // The correctly-guarded call must reject before touching the bus at
  // all: nothing scripted above should have been consumed.
  assert(bus.remaining_expectations() == before);
  assert(bus.mismatch_count() == 0);
}

// ---------------------------------------------------------------------
// IOIN pin-state gates
// ---------------------------------------------------------------------

void TestIoinDecodersMatchDatasheetBitPositions() {
  // TMC5160 IOIN (0x04): DRV_ENN is bit 4, SD_MODE is bit 6. Getting these
  // positions wrong would either wave a dead motor through or ground a
  // working one, so pin them explicitly against neighbouring bits.
  assert(Tmc5160Driver::IoinDriverDisabled(0x30000010U));
  assert(!Tmc5160Driver::IoinDriverDisabled(0x30000000U));
  assert(!Tmc5160Driver::IoinDriverDisabled(0x300000EFU & ~0x10U));

  assert(Tmc5160Driver::IoinStepDirMode(0x30000040U));
  assert(!Tmc5160Driver::IoinStepDirMode(0x30000000U));
  assert(!Tmc5160Driver::IoinStepDirMode(0x300000BFU & ~0x40U));

  // The exact bench readings this gate was written from: motor0 strapped
  // for STEP/DIR with its driver disabled, motor1 strapped correctly.
  assert(Tmc5160Driver::IoinStepDirMode(0x30000050U));
  assert(Tmc5160Driver::IoinDriverDisabled(0x30000050U));
  assert(!Tmc5160Driver::IoinStepDirMode(0x30000012U));
}

void TestSdModeGateRejectsStepDirStrappedModule() {
  // A module strapped SD_MODE=1 takes motion from its STEP/DIR pins, which
  // the v3 pinout does not wire. Every SPI conversation still succeeds and
  // XACTUAL still tracks XTARGET, so without this gate the driver comes up
  // healthy and silently never moves the motor -- exactly what the bench
  // saw. The FULL healthy sequence is scripted (as for the version gate):
  // nothing after IOIN depends on the pin bits, so a driver missing the
  // gate would consume the whole script and report healthy().
  FakeSpiBus bus;
  Tmc5160Config cfg;
  ScriptReinitSequence(&bus, cfg, /*ioin_version_byte=*/0x30,
                       /*ioin_pin_bits=*/0x40);

  Tmc5160Driver driver(cfg, &bus, /*use_gpio=*/false);

  assert(!driver.healthy());
  assert(bus.mismatch_count() == 0);
  // Only IOIN's 2 phases were consumed: the gate stopped Reinitialize()
  // before it programmed a single register.
  assert(bus.remaining_expectations() == kFullReinitExpectationCount - 2);
}

void TestSdModeGateAcceptsCorrectlyStrappedModule() {
  // The mirror of the test above, and the reason it is load-bearing: the
  // other IOIN pin bits must not trip the gate. DRV_ENN high here (0x10)
  // is normal -- the driver is constructed before anything enables it.
  FakeSpiBus bus;
  Tmc5160Config cfg;
  ScriptReinitSequence(&bus, cfg, /*ioin_version_byte=*/0x30,
                       /*ioin_pin_bits=*/0x10);

  Tmc5160Driver driver(cfg, &bus, /*use_gpio=*/false);

  assert(driver.healthy());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

}  // namespace

// ---------------------------------------------------------------------
// SetRunCurrent (STEPPER_SET_CURRENT): live GLOBALSCALER + IHOLD_IRUN
// rewrite, persisted into cfg_ so reconfiguration keeps the new value.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Thermal flags (DRV_STATUS otpw bit 26 / ot bit 25): otpw is live and
// event-counted, ot latches until the next Enable(true). The TMC5160 has
// no numeric temperature ADC — these threshold flags are the whole story.
// ---------------------------------------------------------------------

void TestThermalFlagsLatchAndClearOnReenable() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);

  ExpectRead(&bus, kRegGSTAT, 0U);
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));
  assert(driver->thermal_state() == 0);

  // Poll (enabled, idle): GSTAT clean, then otpw -> pre-warning, counted.
  ScriptIdlePoll(&bus, 0U, 1U << 26, 0U);
  assert(driver->Poll());
  assert(driver->thermal_state() == 1);
  assert(driver->otpw_event_count() == 1);

  // ot -> shutdown, latched.
  ScriptIdlePoll(&bus, 0U, 1U << 25, 0U);
  assert(driver->Poll());
  assert(driver->thermal_state() == 2);

  // The chip's own flag clears as the die cools — the latch must NOT.
  ScriptIdlePoll(&bus, 0U, 0U, 0U);
  assert(driver->Poll());
  assert(driver->thermal_state() == 2);

  // Operator re-enable releases the latch; the fresh read would re-latch
  // a still-hot chip (scripted cool here).
  ExpectRead(&bus, kRegGSTAT, 0U);
  ScriptEnableTrueChopconf(&bus, cfg, /*drv_status=*/0U);
  assert(driver->Enable(true));
  assert(driver->thermal_state() == 0);
  assert(driver->otpw_event_count() == 1);  // history survives
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

void TestSetRunCurrentRewritesRegistersAndPersists() {
  FakeSpiBus bus;
  Tmc5160Config cfg;  // boot current 0.8 A RMS
  auto driver = MakeHealthyDriver(&bus, cfg);

  // Exactly two writes, derived the same way initialisation derives them.
  std::uint32_t gs = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  assert(Tmc5160Driver::CalculateCurrent(0.4, cfg.sense_resistor_ohm,
                                         cfg.hold_current_frac, &gs, &irun,
                                         &ihold));
  const std::uint32_t gs_reg = gs >= 256U ? 0U : gs;
  const std::uint32_t ihold_irun =
      (static_cast<std::uint32_t>(ihold) & 0x1FU) |
      ((static_cast<std::uint32_t>(irun) & 0x1FU) << 8) | (6U << 16);
  ExpectWrite(&bus, kRegGLOBALSCALER, gs_reg);
  ExpectWrite(&bus, kRegIHOLD_IRUN, ihold_irun);

  std::string err;
  assert(driver->SetRunCurrent(0.4, &err));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(std::fabs(driver->run_current_a_rms() - 0.4) < 1e-12);

  // A later reconfiguration (ActiveCheck, chip-reset recovery) must derive
  // its current registers from the NEW value, not the boot config.
  Tmc5160Config cfg_after = cfg;
  cfg_after.run_current_a_rms = 0.4;
  ScriptHealthyReinit(&bus, cfg_after);
  assert(driver->ActiveCheck());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

void TestSetRunCurrentRejectsUnreachableTargetWithoutBusTraffic() {
  FakeSpiBus bus;
  Tmc5160Config cfg;  // 0.075 ohm sense: RMS ceiling ~3.06 A
  auto driver = MakeHealthyDriver(&bus, cfg);

  std::string err;
  assert(!driver->SetRunCurrent(3.1, &err));
  assert(!err.empty());
  // Rejected before any datagram: no scripted expectations were needed.
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  // The stored current is untouched, so recovery paths keep the old value.
  assert(std::fabs(driver->run_current_a_rms() - cfg.run_current_a_rms) <
         1e-12);
}

// ---------------------------------------------------------------------
// Step-loss protection.
//
// The TMC5160 runs open loop: the position is a count of the steps that
// were commanded. Each test below is one way that count stops being true,
// and what the driver does about it.
// ---------------------------------------------------------------------

// COOLCONF as written at initialisation. Hand-computed: sfilt is bit 24,
// sgt a 7-bit two's-complement field in bits 22:16, everything else 0 so
// coolStep (semin, bits 3:0) stays off.
void TestCoolconfEncodesSgtAndFilterWithCoolStepOff() {
  assert(Tmc5160Driver::EncodeCoolconf(0) == 0x01000000U);
  assert(Tmc5160Driver::EncodeCoolconf(1) == 0x01010000U);
  assert(Tmc5160Driver::EncodeCoolconf(63) == 0x013F0000U);
  assert(Tmc5160Driver::EncodeCoolconf(-1) == 0x017F0000U);
  assert(Tmc5160Driver::EncodeCoolconf(-64) == 0x01400000U);
  // Out of range is clamped, never wrapped into the neighbouring fields.
  assert(Tmc5160Driver::EncodeCoolconf(200) == 0x013F0000U);
  assert(Tmc5160Driver::EncodeCoolconf(-200) == 0x01400000U);
  // And it reaches the wire: a non-zero sgt comes up healthy only if the
  // init sequence writes exactly that value.
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.stallguard_sgt = -7;
  auto driver = MakeHealthyDriver(&bus, cfg);
  assert(driver->healthy());
}

void TestParseStallDetect() {
  Tmc5160Config::StallDetect mode = Tmc5160Config::StallDetect::kOff;
  assert(Tmc5160Driver::ParseStallDetect("monitor", &mode));
  assert(mode == Tmc5160Config::StallDetect::kMonitor);
  assert(Tmc5160Driver::ParseStallDetect("stop", &mode));
  assert(mode == Tmc5160Config::StallDetect::kStop);
  assert(Tmc5160Driver::ParseStallDetect("off", &mode));
  assert(mode == Tmc5160Config::StallDetect::kOff);
  assert(!Tmc5160Driver::ParseStallDetect("STOP", &mode));
  assert(!Tmc5160Driver::ParseStallDetect("", &mode));
  assert(mode == Tmc5160Config::StallDetect::kOff);  // untouched on failure
  assert(std::string(Tmc5160Driver::StallDetectName(Tmc5160Config::StallDetect::kStop)) == "stop");
}

// The reply to every datagram starts with the chip's status byte. Its
// reset flag on an XTARGET write means the chip lost its configuration
// before that write: the driver recovers on that very step instead of
// stepping into a dead ramp generator for up to 63 more.
void TestStatusByteResetFlagRecoversOnTheSameStep() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 5, &target);
  assert(driver->step_loss_events() == 0);

  ExpectWriteWithStatus(&bus, kRegXTARGET, 64U * 6U, kStatusReset);
  ExpectRead(&bus, kRegGSTAT, kGstatReset);
  ScriptHealthyReinit(&bus, cfg, /*toff=*/3);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  assert(driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->reset_count() == 1);
  assert(driver->target() == 0);  // the chip's coordinates restart at 0
  assert(driver->step_loss_events() == 1);
  assert(driver->step_loss_reason().find("chip reset") != std::string::npos);
  assert(!driver->TakeStepLossStop());  // the move continues
  // ...and it does, from the fresh origin.
  target = 0;
  StepForward(&bus, driver.get(), 3, &target);
  assert(driver->target() == 192);
  assert(bus.mismatch_count() == 0);
  // MUTATION: drop the status-byte check at the end of Step() and confirm
  // this test fails on remaining_expectations (the recovery never runs).
}

// A flag the chip will not let go of must not turn every step of a move
// into a supervision read: one read, then eight plain steps.
void TestStuckStatusFlagIsRecheckedSparingly() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);

  // driver_error in the status byte, but GSTAT and DRV_STATUS read clean.
  ExpectWriteWithStatus(&bus, kRegXTARGET, 64U, kStatusDriverError);
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  assert(driver->Step(true));
  for (std::uint32_t i = 2; i <= 9; ++i) {
    ExpectWriteWithStatus(&bus, kRegXTARGET, 64U * i, kStatusDriverError);
    assert(driver->Step(true));
  }
  // The tenth step looks again.
  ExpectWriteWithStatus(&bus, kRegXTARGET, 64U * 10U, kStatusDriverError);
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  assert(driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->step_loss_events() == 0);
}

// A short: the chip has switched the bridge off. The step fails (the
// channel disables the motor), the event is recorded, and no further step
// is attempted until STEPPER_ENABLE re-arms the bridge.
void TestShortCircuitFailsTheStepUntilReenabled() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);

  ExpectWriteWithStatus(&bus, kRegXTARGET, 64U, kStatusDriverError);
  ExpectRead(&bus, kRegGSTAT, kGstatDrvErr);
  ExpectWrite(&bus, kRegGSTAT, kGstatDrvErr);  // counted, then cleared
  ExpectRead(&bus, kRegDRV_STATUS, kDrvS2ga | kDrvS2vsa);
  assert(!driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->healthy());  // the chip answers; it is the motor side that is shorted
  assert(driver->short_count() == 1);
  assert(driver->step_loss_events() == 1);  // one event: the short explains the drv_err
  assert(driver->step_loss_reason().find("short circuit") != std::string::npos);
  assert(driver->step_loss_reason().find("s2ga s2vsa") != std::string::npos);
  // Not a "stop and keep holding": the refused step makes the channel
  // disable the motor, which is also what re-arms the bridge.
  assert(!driver->TakeStepLossStop());
  assert(driver->warning().find("short circuit on a motor coil 1x ACTIVE") != std::string::npos);

  // No bus traffic for a step that cannot be taken.
  assert(!driver->Step(true));
  assert(bus.mismatch_count() == 0);

  // Disable + enable re-arms; a clean DRV_STATUS lets it step again.
  ExpectRead(&bus, kRegXACTUAL, 64U);
  ExpectWrite(&bus, kRegXTARGET, 64U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/0));
  assert(driver->Enable(false));
  EnableHealthy(&bus, driver.get(), cfg);
  ExpectWrite(&bus, kRegXTARGET, 128U);
  assert(driver->Step(true));
  assert(driver->short_count() == 1);
  assert(driver->warning().find("ACTIVE") == std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  // MUTATION: make CheckDriverStatusUnlocked ignore kDrvShortMask and
  // confirm this test fails on the first `!driver->Step(true)`.
}

// A short found while idle (holding): there is no step to fail, so the
// event asks the channel to end the hold, and the next step is refused.
void TestShortCircuitWhileIdleAsksForTheHoldToEnd() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  ScriptIdlePoll(&bus, 0U, kDrvS2ga, 0U);
  assert(driver->Poll());
  assert(driver->step_loss_events() == 1);
  assert(driver->TakeStepLossStop());
  assert(!driver->TakeStepLossStop());  // read once
  assert(!driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// Supply undervoltage (GSTAT.uv_cp): the power stage was off while the
// supply was low. One event per episode; the flag is cleared so the next
// read shows whether it is still there; the move continues.
void TestUndervoltageIsOneEventPerEpisodeAndTheMoveContinues() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 63, &target);

  // 64th step: the periodic supervision read.
  ExpectRead(&bus, kRegGSTAT, kGstatUvCp);
  ExpectWrite(&bus, kRegGSTAT, kGstatUvCp);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  StepForward(&bus, driver.get(), 1, &target);
  assert(driver->undervoltage_count() == 1);
  assert(driver->step_loss_events() == 1);
  assert(driver->step_loss_reason().find("undervoltage") != std::string::npos);
  assert(!driver->TakeStepLossStop());
  assert(driver->healthy());

  // Still low at the next check: the same episode, not a second event.
  StepForward(&bus, driver.get(), 63, &target);
  ExpectRead(&bus, kRegGSTAT, kGstatUvCp);
  ExpectWrite(&bus, kRegGSTAT, kGstatUvCp);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  StepForward(&bus, driver.get(), 1, &target);
  assert(driver->undervoltage_count() == 1);
  assert(driver->step_loss_events() == 1);

  // Recovered, then low again: a new episode.
  StepForward(&bus, driver.get(), 63, &target);
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  StepForward(&bus, driver.get(), 1, &target);
  StepForward(&bus, driver.get(), 63, &target);
  ExpectRead(&bus, kRegGSTAT, kGstatUvCp);
  ExpectWrite(&bus, kRegGSTAT, kGstatUvCp);
  ExpectRead(&bus, kRegDRV_STATUS, 0U);
  StepForward(&bus, driver.get(), 1, &target);
  assert(driver->undervoltage_count() == 2);
  assert(driver->step_loss_events() == 2);
  assert(driver->warning().find("motor supply undervoltage 2x") != std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// An undervoltage flagged before the motor was enabled lost nothing: it is
// cleared and counted, not a step-loss event.
void TestUndervoltageBeforeEnableIsNotStepLoss() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);
  ExpectRead(&bus, kRegGSTAT, kGstatUvCp);
  ExpectWrite(&bus, kRegGSTAT, kGstatUvCp);
  ScriptEnableTrueChopconf(&bus, cfg);
  assert(driver->Enable(true));
  assert(driver->undervoltage_count() == 1);
  assert(driver->step_loss_events() == 0);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// drv_err with nothing in DRV_STATUS to explain it: the power stage was
// shut down and has recovered. An event; the move continues. With the
// over-temperature flag set it is the thermal path's business instead
// (the channel disables the motor on thermal_state 2).
void TestDriverErrorIsAnEventUnlessThermalExplainsIt() {
  {
    FakeSpiBus bus;
    Tmc5160Config cfg;
    auto driver = MakeHealthyDriver(&bus, cfg);
    EnableHealthy(&bus, driver.get(), cfg);
    ExpectRead(&bus, kRegGSTAT, kGstatDrvErr);
    ExpectWrite(&bus, kRegGSTAT, kGstatDrvErr);
    ExpectRead(&bus, kRegDRV_STATUS, 0U);
    ExpectRead(&bus, kRegXACTUAL, 0U);
    assert(driver->Poll());
    assert(driver->step_loss_events() == 1);
    assert(driver->step_loss_reason().find("driver error") != std::string::npos);
    assert(!driver->TakeStepLossStop());
    assert(bus.mismatch_count() == 0);
    assert(bus.remaining_expectations() == 0);
  }
  {
    FakeSpiBus bus;
    Tmc5160Config cfg;
    auto driver = MakeHealthyDriver(&bus, cfg);
    EnableHealthy(&bus, driver.get(), cfg);
    ExpectRead(&bus, kRegGSTAT, kGstatDrvErr);
    ExpectWrite(&bus, kRegGSTAT, kGstatDrvErr);
    ExpectRead(&bus, kRegDRV_STATUS, kDrvOt);
    ExpectRead(&bus, kRegXACTUAL, 0U);
    assert(driver->Poll());
    assert(driver->thermal_state() == 2);
    assert(driver->step_loss_events() == 0);
    assert(bus.mismatch_count() == 0);
    assert(bus.remaining_expectations() == 0);
  }
}

// Open-load flags are an indication only: counted and warned about, never
// an event, never a refused step.
void TestOpenLoadIsCountedAndNeverStops() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 63, &target);
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectRead(&bus, kRegDRV_STATUS, kDrvOla);
  StepForward(&bus, driver.get(), 1, &target);
  assert(driver->open_load_count() == 1);
  assert(driver->step_loss_events() == 0);
  assert(driver->warning().find("open load flagged 1x") != std::string::npos);
  // At standstill the flags mean nothing and are not looked at.
  ScriptIdlePoll(&bus, 0U, kDrvOla, static_cast<std::uint32_t>(target));
  assert(driver->Poll());
  assert(driver->open_load_count() == 1);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// The last XTARGET write of a move has no later write to supersede it: a
// corrupted one would send the motor to a position nobody asked for.
// ConfirmTarget() reads it back and rewrites it once.
void TestConfirmTargetRepairsACorruptedXtarget() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  // Disabled: nothing to confirm, no bus traffic.
  assert(driver->ConfirmTarget());
  assert(bus.remaining_expectations() == 0);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 2, &target);  // target 128

  ExpectRead(&bus, kRegXTARGET, 128U);
  assert(driver->ConfirmTarget());
  assert(driver->xtarget_repair_count() == 0);

  // Bit 20 flipped in the last write: 128 + 1048576.
  ExpectRead(&bus, kRegXTARGET, 128U + (1U << 20));
  ExpectWrite(&bus, kRegXTARGET, 128U);
  ExpectRead(&bus, kRegXTARGET, 128U);
  assert(driver->ConfirmTarget());
  assert(driver->healthy());
  assert(driver->xtarget_repair_count() == 1);
  assert(driver->step_loss_events() == 0);  // repaired before anything was lost
  assert(driver->warning().find("XTARGET rewritten 1x") != std::string::npos);

  // A register that will not take the value: the driver cannot be trusted.
  ExpectRead(&bus, kRegXTARGET, 0U);
  ExpectWrite(&bus, kRegXTARGET, 128U);
  ExpectRead(&bus, kRegXTARGET, 0U);
  assert(!driver->ConfirmTarget());
  assert(!driver->healthy());
  assert(driver->last_error().find("XTARGET does not hold") != std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// At standstill the chip must be where it was told to go. One mismatch can
// be the ramp generator finishing its last hop; two polls in a row is a
// chip that did not execute the move (or is not there at all: an absent
// chip with MISO low reads XACTUAL=0 and every flag clean).
void TestIdlePositionMismatchOnTwoPollsIsStepLoss() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 2, &target);  // target 128

  ScriptIdlePoll(&bus, 0U, 0U, 64U);  // one hop short: still arriving
  assert(driver->Poll());
  assert(driver->step_loss_events() == 0);
  ScriptIdlePoll(&bus, 0U, 0U, 128U);  // arrived: the count starts over
  assert(driver->Poll());
  ScriptIdlePoll(&bus, 0U, 0U, 0U);
  assert(driver->Poll());
  assert(driver->healthy());
  assert(driver->step_loss_events() == 0);

  ScriptIdlePoll(&bus, 0U, 0U, 0U);
  assert(!driver->Poll());
  assert(!driver->healthy());
  assert(driver->step_loss_events() == 1);
  assert(driver->step_loss_reason().find("chip position 0 is not the commanded 128") != std::string::npos);
  assert(driver->TakeStepLossStop());  // ends a hold; the motor is re-probed
  assert(driver->last_error().find("did not execute the move") != std::string::npos);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  // MUTATION: drop CheckPositionUnlocked() from Poll() and confirm this
  // test fails on remaining_expectations.
}

// StallGuard2. Hand-computed cadence at divisor 4: one electrical period is
// 4 full steps = 16 Step() calls, so DRV_STATUS is read at steps 16, 32, 48
// and -- together with the periodic GSTAT check -- 64. The first read after
// cruise begins is discarded (the filter still holds the ramp), so the
// samples are steps 32, 48, 64, and with stall_confirm_samples=3 a stall is
// called on step 64.
void RunToStallVerdict(FakeSpiBus* bus, Tmc5160Driver* driver,
                       std::uint32_t sg_a, std::uint32_t sg_b, std::uint32_t sg_c,
                       bool expect_last_step) {
  std::int32_t target = 0;
  driver->NoteStepRate(100.0, /*steady=*/true);
  StepForward(bus, driver, 15, &target);
  ExpectRead(bus, kRegDRV_STATUS, 5U);  // discarded
  StepForward(bus, driver, 1, &target);
  StepForward(bus, driver, 15, &target);
  ExpectRead(bus, kRegDRV_STATUS, sg_a);
  StepForward(bus, driver, 1, &target);
  StepForward(bus, driver, 15, &target);
  ExpectRead(bus, kRegDRV_STATUS, sg_b);
  StepForward(bus, driver, 1, &target);
  StepForward(bus, driver, 15, &target);
  ExpectRead(bus, kRegGSTAT, 0U);
  ExpectRead(bus, kRegDRV_STATUS, sg_c);
  if (expect_last_step) {
    StepForward(bus, driver, 1, &target);
  } else {
    assert(!driver->Step(true));
  }
  assert(bus->mismatch_count() == 0);
  assert(bus->remaining_expectations() == 0);
}

void TestStallGuardMonitorCountsAndTheMoveContinues() {
  FakeSpiBus bus;
  Tmc5160Config cfg;  // stall_detect = monitor, stall_sg_min = 0, 3 samples
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  RunToStallVerdict(&bus, driver.get(), 0U, 0U, 0U, /*expect_last_step=*/true);
  assert(driver->stall_verdict_count() == 1);
  assert(driver->stallguard_samples() == 3);
  assert(driver->stallguard_last() == 0);
  assert(driver->stallguard_min() == 0);
  // Monitor: recorded, never a step-loss event, never a stop.
  assert(driver->step_loss_events() == 0);
  assert(!driver->TakeStepLossStop());
  assert(driver->warning().find("StallGuard stall verdict 1x (stall_detect=monitor)") != std::string::npos);
}

void TestStallGuardStopRefusesTheStepAndAsksForAStop() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  cfg.stall_detect = Tmc5160Config::StallDetect::kStop;
  cfg.stall_sg_min = 40;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  RunToStallVerdict(&bus, driver.get(), 40U, 12U, 0U, /*expect_last_step=*/false);
  assert(driver->stall_verdict_count() == 1);
  assert(driver->step_loss_events() == 1);
  assert(driver->step_loss_reason().find("StallGuard stall") != std::string::npos);
  assert(driver->TakeStepLossStop());
  // Still healthy and enabled: the channel stops the move and keeps holding.
  assert(driver->healthy());
  assert(driver->enabled());
  assert(driver->target() == 63 * 64);  // the refused step was not written
  // MUTATION: compare `sg < cfg_.stall_sg_min` instead of `<=` and confirm
  // this test fails (the first sample, exactly at the threshold, no longer counts).
}

// A healthy reading in between breaks the run: three low samples in a row,
// not three in total.
void TestStallGuardNeedsConsecutiveLowSamples() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  cfg.stall_detect = Tmc5160Config::StallDetect::kStop;
  cfg.stall_sg_min = 40;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  RunToStallVerdict(&bus, driver.get(), 10U, 300U, 10U, /*expect_last_step=*/true);
  assert(driver->stall_verdict_count() == 0);
  assert(driver->step_loss_events() == 0);
  assert(driver->stallguard_min() == 10);
  assert(driver->stallguard_last() == 10);
  assert(driver->stallguard_samples() == 3);
}

// No sampling -- no extra datagrams at all -- unless the channel reports a
// steady rate at or above the minimum, in spreadCycle, with detection on.
void TestStallGuardIsSampledOnlyAtASteadyRateInSpreadCycle() {
  struct Case {
    const char* name;
    double hz;
    bool steady;
    bool stealth;
    Tmc5160Config::StallDetect mode;
  };
  const Case cases[] = {
      {"ramping", 100.0, false, false, Tmc5160Config::StallDetect::kMonitor},
      {"too slow", 49.0, true, false, Tmc5160Config::StallDetect::kMonitor},
      {"stealthChop", 100.0, true, true, Tmc5160Config::StallDetect::kMonitor},
      {"off", 100.0, true, false, Tmc5160Config::StallDetect::kOff},
  };
  for (const Case& c : cases) {
    FakeSpiBus bus;
    Tmc5160Config cfg;
    cfg.microstep = 4;
    cfg.stealth_chop = c.stealth;
    cfg.stall_detect = c.mode;
    auto driver = MakeHealthyDriver(&bus, cfg);
    EnableHealthy(&bus, driver.get(), cfg);
    driver->NoteStepRate(c.hz, c.steady);
    std::int32_t target = 0;
    // 63 steps with only XTARGET writes scripted: any DRV_STATUS read would
    // hit the strict fake as a mismatch.
    StepForward(&bus, driver.get(), 63, &target);
    assert(bus.mismatch_count() == 0);
    assert(driver->stallguard_samples() == 0);
    (void)c.name;
  }
  // Never told a rate at all (every pre-existing test): no sampling either.
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  std::int32_t target = 0;
  StepForward(&bus, driver.get(), 63, &target);
  assert(bus.mismatch_count() == 0);
}

// The statistics belong to one move: a rate of 0 closes it and the next
// move starts them afresh. MOTOR_DEBUG reports them.
void TestStallGuardStatisticsArePerMoveAndReachMotorDebug() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  cfg.stallguard_sgt = 5;
  cfg.stall_sg_min = 20;
  auto driver = MakeHealthyDriver(&bus, cfg);
  EnableHealthy(&bus, driver.get(), cfg);
  RunToStallVerdict(&bus, driver.get(), 310U, 280U, 295U, /*expect_last_step=*/true);
  assert(driver->stallguard_samples() == 3);
  assert(driver->stallguard_min() == 280);
  assert(driver->stallguard_last() == 295);

  // The twelve MOTOR_DEBUG reads, all zero: only the trailing statistics matter here.
  for (std::uint8_t reg : {0x21, 0x2D, 0x22, 0x6A, 0x6F, 0x35, 0x12, 0x04, 0x01, 0x6C, 0x71, 0x72}) {
    ExpectRead(&bus, reg, 0U);
  }
  const std::string kv = driver->DebugRegisters();
  assert(kv.find(";stall_mode=monitor;sgt=5;sg_thr=20;sg_last=295;sg_min=280;sg_n=3;stalls=0") != std::string::npos);

  driver->NoteStepRate(0.0, false);  // the move ended
  assert(driver->stallguard_samples() == 3);  // kept until the next one starts
  driver->NoteStepRate(100.0, true);
  assert(driver->stallguard_samples() == 0);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

int main() {
  TestEncodeMresTable();
  TestEncodeMresRejectsInvalidDivisor();
  TestDeltaXtargetTable();
  TestDeltaXtargetRejectsInvalidDivisor();
  TestCalculateCurrentLowRegimeKeepsIrunAtMax();
  TestCalculateCurrentHighRegimeReducesIrun();
  TestCalculateCurrentRejectsUnreachableTarget();
  TestCalculateCurrentRejectsInvalidInputs();
  TestCalculateCurrentLowCurrentFloorReducesIrun();
  TestCalculateCurrentRejectsUltraLowCurrent();
  TestCalculateCurrentIholdEndpoints();
  TestCalculateCurrentLowCurrentSweepReconstructsWithinTolerance();
  TestVersionGateAcceptsTmc5160Version();
  TestVersionGateRejectsTmc2240Version();
  TestIoinDecodersMatchDatasheetBitPositions();
  TestSdModeGateRejectsStepDirStrappedModule();
  TestSdModeGateAcceptsCorrectlyStrappedModule();
  TestStepForwardThenReverseAtDivisor4();
  TestStepHonoursInvertDirection();
  TestEnableFalseFreezesInOrder();
  TestEnableFalseDetectsIneffectiveEnableLine();
  TestReinitializeKeepsChopperOffWhileDisabled();
  TestChipResetOnEnableIsRecovered();
  TestChipResetAtIdleIsRecoveredByPoll();
  TestChipResetMidMoveIsRecoveredWithinTheCheckInterval();
  TestDebugRegistersDecodeMotionTruth();
  TestTransferFailureMarksUnhealthyAndActiveCheckReprobes();
  TestEachDatagramIsOneControllerLockHoldWithModeReapplied();
  TestStealthChopSelectsGconfEnPwmModeBit();
  TestSetMicrostepRejectsInvalidDivisor();
  TestSetRunCurrentRewritesRegistersAndPersists();
  TestSetRunCurrentRejectsUnreachableTargetWithoutBusTraffic();
  TestThermalFlagsLatchAndClearOnReenable();
  TestCoolconfEncodesSgtAndFilterWithCoolStepOff();
  TestParseStallDetect();
  TestStatusByteResetFlagRecoversOnTheSameStep();
  TestStuckStatusFlagIsRecheckedSparingly();
  TestShortCircuitFailsTheStepUntilReenabled();
  TestShortCircuitWhileIdleAsksForTheHoldToEnd();
  TestUndervoltageIsOneEventPerEpisodeAndTheMoveContinues();
  TestUndervoltageBeforeEnableIsNotStepLoss();
  TestDriverErrorIsAnEventUnlessThermalExplainsIt();
  TestOpenLoadIsCountedAndNeverStops();
  TestConfirmTargetRepairsACorruptedXtarget();
  TestIdlePositionMismatchOnTwoPollsIsStepLoss();
  TestStallGuardMonitorCountsAndTheMoveContinues();
  TestStallGuardStopRefusesTheStepAndAsksForAStop();
  TestStallGuardNeedsConsecutiveLowSamples();
  TestStallGuardIsSampledOnlyAtASteadyRateInSpreadCycle();
  TestStallGuardStatisticsArePerMoveAndReachMotorDebug();
  return 0;
}
