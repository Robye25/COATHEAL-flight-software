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

void ExpectWrite(FakeSpiBus* bus, std::uint8_t addr, std::uint32_t value) {
  std::vector<std::uint8_t> tx = {
      static_cast<std::uint8_t>(addr | 0x80U),
      static_cast<std::uint8_t>((value >> 24) & 0xFFU),
      static_cast<std::uint8_t>((value >> 16) & 0xFFU),
      static_cast<std::uint8_t>((value >> 8) & 0xFFU),
      static_cast<std::uint8_t>(value & 0xFFU)};
  bus->Expect(tx, {0, 0, 0, 0, 0});
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
  const std::uint8_t mres = Tmc5160Driver::EncodeMres(microstep);
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
                          std::uint8_t ioin_pin_bits = 0) {
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
  const std::uint32_t chopconf_run = Chopconf(cfg.microstep, /*toff=*/3);
  const std::uint32_t gs_reg = globalscaler >= 256U ? 0U : globalscaler;
  const std::uint32_t ihold_irun =
      (static_cast<std::uint32_t>(ihold) & 0x1FU) |
      ((static_cast<std::uint32_t>(irun) & 0x1FU) << 8) | (6U << 16);

  ExpectRead(bus, kRegIOIN,
            (static_cast<std::uint32_t>(ioin_version_byte) << 24) |
                static_cast<std::uint32_t>(ioin_pin_bits));
  ExpectWrite(bus, kRegGCONF, gconf);
  ExpectWrite(bus, kRegCHOPCONF, chopconf_run);
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

// Total Expect() entries ScriptReinitSequence() queues: IOIN (2) + 16
// register writes + GCONF/CHOPCONF readback (2*2) + the GSTAT clear (1). Used to prove the
// version-gate test stops exactly at IOIN, not "eventually, somehow".
constexpr std::size_t kFullReinitExpectationCount = 23;

void ScriptHealthyReinit(FakeSpiBus* bus, const Tmc5160Config& cfg) {
  ScriptReinitSequence(bus, cfg, /*ioin_version_byte=*/0x30);
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

// Scripts the single extra CHOPCONF(TOFF=3) write Enable(true) issues
// unconditionally when the driver is already healthy_ (no Reinitialize()).
void ScriptEnableTrueChopconf(FakeSpiBus* bus, const Tmc5160Config& cfg) {
  ExpectWrite(bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/3));
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
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/3));
  assert(driver->Enable(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->reset_count() == 1);
  assert(driver->warning().find("chip reset 1x") != std::string::npos);
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
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/3));
  assert(driver->Enable(true));
  // Enabled and idle: a reset since the last check is repaired in place,
  // chopper restored (bench 2026-08-29: M1's chip reset right after its
  // move ended and sat with TOFF=0, holding nothing).
  ExpectRead(&bus, kRegGSTAT, 0x1U);
  ScriptHealthyReinit(&bus, cfg);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(cfg.microstep, /*toff=*/3));
  assert(driver->Poll());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->reset_count() == 1);
  assert(driver->enabled());
}

void TestChipResetMidMoveIsRecoveredWithinTheCheckInterval() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  cfg.microstep = 4;
  auto driver = MakeHealthyDriver(&bus, cfg);
  ExpectRead(&bus, kRegGSTAT, 0U);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(4, /*toff=*/3));
  assert(driver->Enable(true));
  // 63 plain steps, then the 64th re-reads GSTAT and finds the chip reset:
  // configuration rewritten, chopper restored, and the step continues from
  // the fresh XACTUAL=0 (target 64, not 64*64).
  for (int i = 1; i <= 63; ++i) {
    ExpectWrite(&bus, kRegXTARGET, static_cast<std::uint32_t>(64 * i));
    assert(driver->Step(true));
  }
  ExpectRead(&bus, kRegGSTAT, 0x1U);
  ScriptHealthyReinit(&bus, cfg);
  ExpectWrite(&bus, kRegCHOPCONF, Chopconf(4, /*toff=*/3));
  ExpectWrite(&bus, kRegXTARGET, 64U);
  assert(driver->Step(true));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  assert(driver->target() == 64);
  assert(driver->reset_count() == 1);
  assert(driver->enabled());
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
  TestChipResetOnEnableIsRecovered();
  TestChipResetAtIdleIsRecoveredByPoll();
  TestChipResetMidMoveIsRecoveredWithinTheCheckInterval();
  TestDebugRegistersDecodeMotionTruth();
  TestTransferFailureMarksUnhealthyAndActiveCheckReprobes();
  TestEachDatagramIsOneControllerLockHoldWithModeReapplied();
  TestStealthChopSelectsGconfEnPwmModeBit();
  TestSetMicrostepRejectsInvalidDivisor();
  return 0;
}
