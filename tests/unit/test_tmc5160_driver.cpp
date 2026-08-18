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

#include "coatheal/tmc5160_driver.hpp"
#include "fake_spi_bus.hpp"

using namespace coatheal;

namespace {

// ---------------------------------------------------------------------
// Shared SPI-script helpers
// ---------------------------------------------------------------------

constexpr std::uint8_t kRegGCONF = 0x00;
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
                          std::uint8_t ioin_version_byte) {
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool current_ok = Tmc5160Driver::CalculateCurrent(
      cfg.run_current_a_rms, cfg.sense_resistor_ohm, cfg.hold_current_frac,
      &globalscaler, &irun, &ihold);
  assert(current_ok);

  const std::uint32_t gconf = 0x00000004U;
  const std::uint32_t chopconf_run = Chopconf(cfg.microstep, /*toff=*/3);
  const std::uint32_t gs_reg = globalscaler >= 256U ? 0U : globalscaler;
  const std::uint32_t ihold_irun =
      (static_cast<std::uint32_t>(ihold) & 0x1FU) |
      ((static_cast<std::uint32_t>(irun) & 0x1FU) << 8) | (6U << 16);

  ExpectRead(bus, kRegIOIN,
            static_cast<std::uint32_t>(ioin_version_byte) << 24);
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
}

// Total Expect() entries ScriptReinitSequence() queues: IOIN (2) + 16
// register writes + GCONF/CHOPCONF readback (2*2). Used to prove the
// version-gate test stops exactly at IOIN, not "eventually, somehow".
constexpr std::size_t kFullReinitExpectationCount = 22;

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
  // IHOLD=round(20*0.30)=6.
  std::uint32_t globalscaler = 0;
  std::uint8_t irun = 0;
  std::uint8_t ihold = 0;
  const bool ok = Tmc5160Driver::CalculateCurrent(
      /*a_rms=*/2.0, /*sense_ohm=*/0.075, /*hold_frac=*/0.30, &globalscaler,
      &irun, &ihold);
  assert(ok);
  assert(globalscaler == 256U);
  assert(irun == 20);  // < 31: forced IRUN reduction
  assert(ihold == 6);
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

void TestSetMicrostepRejectsInvalidDivisor() {
  FakeSpiBus bus;
  Tmc5160Config cfg;
  auto driver = MakeHealthyDriver(&bus, cfg);

  driver->SetMicrostep(3);  // not a supported power-of-two divisor
  assert(!driver->healthy());
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
  TestVersionGateAcceptsTmc5160Version();
  TestVersionGateRejectsTmc2240Version();
  TestStepForwardThenReverseAtDivisor4();
  TestStepHonoursInvertDirection();
  TestEnableFalseFreezesInOrder();
  TestTransferFailureMarksUnhealthyAndActiveCheckReprobes();
  TestSetMicrostepRejectsInvalidDivisor();
  return 0;
}
