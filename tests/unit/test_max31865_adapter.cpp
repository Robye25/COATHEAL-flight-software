// Contract tests for Max31865Adapter: the clean-room MAX31865 driver
// measuring each coating specimen's resistance over the strict FakeSpiBus
// (Task 3). The coating's resistance range is UNTESTED, so the property
// under test throughout is that saturation/faults are reported as such --
// never averaged, clamped, or otherwise turned into a plausible ohm value.

#include <cassert>
#include <cstdint>
#include <string>

#include "coatheal/hal/max31865_adapter.hpp"
#include "coatheal/hal/spi_bus_lock.hpp"
#include "fake_spi_bus.hpp"

using namespace coatheal;

namespace {

// ---------------------------------------------------------------------
// Shared SPI-script helpers
// ---------------------------------------------------------------------

constexpr std::uint8_t kRegConfig = 0x00;
constexpr std::uint8_t kRegRtdMsb = 0x01;
constexpr std::uint8_t kRegFaultStatus = 0x07;
constexpr std::uint8_t kWriteBit = 0x80;

constexpr std::uint8_t kCfgBiasOn = 0x81;       // VBIAS | 50HZ
constexpr std::uint8_t kCfgBiasOn1Shot = 0xA1;  // VBIAS | 1SHOT | 50HZ
constexpr std::uint8_t kCfgBiasOff = 0x01;      // 50HZ
constexpr std::uint8_t kCfgFaultClear = 0x03;   // FAULTCLR | 50HZ
constexpr std::uint8_t kCfgProbe = 0x01;        // 50HZ (benign)

void ExpectConfigWrite(FakeSpiBus* bus, std::uint8_t value) {
  bus->Expect({static_cast<std::uint8_t>(kRegConfig | kWriteBit), value},
              {0, 0});
}

void ExpectConfigRead(FakeSpiBus* bus, std::uint8_t value) {
  bus->Expect({kRegConfig, 0}, {0, value});
}

void ExpectRtdRead(FakeSpiBus* bus, std::uint8_t msb, std::uint8_t lsb) {
  bus->Expect({kRegRtdMsb, 0, 0}, {0, msb, lsb});
}

void ExpectFaultRead(FakeSpiBus* bus, std::uint8_t value) {
  bus->Expect({kRegFaultStatus, 0}, {0, value});
}

Max31865Adapter::Options TestOptions() {
  Max31865Adapter::Options opts;
  opts.spi_device = "/dev/spidev0.1";
  opts.reference_ohm = 470.0;
  opts.spi_speed_hz = 500000;
  // Datasheet minimums are 10 ms / 65 ms; zeroed here so the suite doesn't
  // burn 75+ ms of real wall time per one-shot test. Production call
  // sites must leave Options' defaults (10/65) alone.
  opts.settle_ms = 0;
  opts.conversion_ms = 0;
  return opts;
}

// Load-bearing per the Task 4 review finding: FakeSpiBus's Transfer()
// never checks mode/no_cs/device/speed against Open() -- only an explicit
// assertion does. On real hardware, opening with the wrong mode or
// no_cs=true would corrupt every datagram and let the kernel drive a
// native chip-select the MAX31865 click doesn't expect.
void AssertOpenParams(const FakeSpiBus& bus,
                      const Max31865Adapter::Options& opts) {
  assert(bus.open_device() == opts.spi_device);
  assert(bus.open_mode() == 1);  // SPI mode 1
  assert(bus.open_speed_hz() == opts.spi_speed_hz);
  assert(bus.open_no_cs() == false);  // native CE: CE0/CE1 belong to the clicks
}

// ---------------------------------------------------------------------
// CodeToOhms -- hand-computed cases (see task-6-report.md for the
// derivations), pinned before the implementation existed.
// ---------------------------------------------------------------------

void TestCodeToOhmsExactCases() {
  // R = code * reference_ohm / 32768.
  assert(Max31865Adapter::CodeToOhms(8192, 470.0) == 117.5);
  assert(Max31865Adapter::CodeToOhms(8192, 400.0) == 100.0);
  // "full-scale-1": code = 32767, the RTD ADC's actual maximum
  // representable 15-bit value (32768 itself never appears on the wire).
  // 32767/32768 = 1 - 1/32768, so R = 470*(1 - 1/32768) =
  // 470 - 470/32768 = 470 - 0.01434326171875 = 469.98565673828125,
  // exact in double (32768 is a power of two and the numerator fits well
  // within the 53-bit mantissa).
  assert(Max31865Adapter::CodeToOhms(32767, 470.0) == 469.98565673828125);
}

// ---------------------------------------------------------------------
// Options{} defaults: pins the datasheet-minimum one-shot timing floors
// (settle >=10 ms after VBIAS on, conversion >=65 ms). Every other test in
// this file zeroes both fields via TestOptions() to keep the suite fast,
// so nothing else here would notice a future edit that quietly shortened
// a production default below the datasheet floor -- that failure mode
// only manifests as invalid conversions on real hardware.
// ---------------------------------------------------------------------

void TestOptionsDefaultsMatchDatasheetTimingFloors() {
  Max31865Adapter::Options defaults;
  assert(defaults.settle_ms == 10);
  assert(defaults.conversion_ms == 65);
}

// ---------------------------------------------------------------------
// ReadOneShot: healthy sequence order + open-parameter assertions.
// Mutation target: reordering the bias-off write before the RTD read --
// the strict FIFO FakeSpiBus sends the RTD-read tx bytes against
// whichever expectation is at the front of the queue, so a reordered
// implementation mismatches here and ReadOneShot returns false.
// ---------------------------------------------------------------------

void TestOneShotHealthySequenceOrderAndOpenParams() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgBiasOn);
  ExpectConfigWrite(&bus, kCfgBiasOn1Shot);
  // raw = 0x4000 -> code = raw>>1 = 8192, LSB bit0 = 0 (no fault).
  ExpectRtdRead(&bus, /*msb=*/0x40, /*lsb=*/0x00);
  ExpectConfigWrite(&bus, kCfgBiasOff);

  Max31865Adapter::Reading reading;
  std::string error;
  assert(adapter.ReadOneShot(&reading, &error));
  assert(error.empty());
  assert(reading.valid);
  assert(!reading.out_of_range);
  assert(reading.fault_bits == 0);
  assert(reading.resistance_ohm == 117.5);  // code 8192 @ 470 ohm ref
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  AssertOpenParams(bus, opts);
}

// ---------------------------------------------------------------------
// Fault-bit set -> out_of_range + fault register read + FAULTCLR.
// Mutation target: dropping the fault-bit check (`fault_flag ||`) from the
// `saturated` expression -- this code sits comfortably below the
// full-scale threshold, so ONLY the fault bit can trigger out_of_range
// here, isolating this check from the full-scale one below.
// ---------------------------------------------------------------------

void TestFaultBitSetTriggersOutOfRangeAndFaultClear() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgBiasOn);
  ExpectConfigWrite(&bus, kCfgBiasOn1Shot);
  // raw = 0x4001 -> code = raw>>1 = 8192 (well under kNearFullScaleCode),
  // LSB bit0 = 1 (fault flag set).
  ExpectRtdRead(&bus, /*msb=*/0x40, /*lsb=*/0x01);
  ExpectConfigWrite(&bus, kCfgBiasOff);
  ExpectFaultRead(&bus, 0x04);  // arbitrary nonzero fault byte
  ExpectConfigWrite(&bus, kCfgFaultClear);

  Max31865Adapter::Reading reading;
  std::string error;
  assert(adapter.ReadOneShot(&reading, &error));
  assert(error.empty());
  assert(!reading.valid);
  assert(reading.out_of_range);
  assert(reading.fault_bits == 0x04);
  // Diagnostic value still computed even though invalid.
  assert(reading.resistance_ohm == 117.5);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Near-full-scale code (>= 32760) without the fault bit -> out_of_range.
// Mutation target: dropping the full-scale check (`|| code >=
// kNearFullScaleCode`) from `saturated`. Boundary asserted both
// directions: 32760 (threshold itself) is out_of_range; 32759 (one below)
// is valid -- proving the threshold sits exactly at 32760, not somewhere
// nearby.
// ---------------------------------------------------------------------

void TestNearFullScaleCodeTriggersOutOfRangeWithoutFaultBit() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  // code = 32760 (threshold), fault bit clear: raw = code*2 = 65520 =
  // 0xFFF0. MSB=0xFF, LSB=0xF0 (bit0=0).
  ExpectConfigWrite(&bus, kCfgBiasOn);
  ExpectConfigWrite(&bus, kCfgBiasOn1Shot);
  ExpectRtdRead(&bus, /*msb=*/0xFF, /*lsb=*/0xF0);
  ExpectConfigWrite(&bus, kCfgBiasOff);
  ExpectFaultRead(&bus, 0x00);  // fault register itself reports nothing
  ExpectConfigWrite(&bus, kCfgFaultClear);

  Max31865Adapter::Reading reading;
  std::string error;
  assert(adapter.ReadOneShot(&reading, &error));
  assert(error.empty());
  assert(!reading.valid);
  assert(reading.out_of_range);
  assert(reading.fault_bits == 0x00);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

void TestJustBelowFullScaleThresholdWithoutFaultBitIsValid() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  // code = 32759 (one below the threshold), fault bit clear: raw =
  // 32759*2 = 65518 = 0xFFEE. MSB=0xFF, LSB=0xEE (bit0=0).
  ExpectConfigWrite(&bus, kCfgBiasOn);
  ExpectConfigWrite(&bus, kCfgBiasOn1Shot);
  ExpectRtdRead(&bus, /*msb=*/0xFF, /*lsb=*/0xEE);
  ExpectConfigWrite(&bus, kCfgBiasOff);
  // No fault-register read/FAULTCLR expected: not saturated.

  Max31865Adapter::Reading reading;
  std::string error;
  assert(adapter.ReadOneShot(&reading, &error));
  assert(error.empty());
  assert(reading.valid);
  assert(!reading.out_of_range);
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Probe: config write + readback, both directions.
// ---------------------------------------------------------------------

void TestProbeSucceedsOnMatchingReadback() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgProbe);
  ExpectConfigRead(&bus, kCfgProbe);

  std::string error;
  assert(adapter.Probe(&error));
  assert(error.empty());
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
  AssertOpenParams(bus, opts);
}

void TestProbeReadbackMismatchReturnsFalse() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgProbe);
  ExpectConfigRead(&bus, 0x99);  // click answers with something else

  std::string error;
  assert(!adapter.Probe(&error));
  assert(!error.empty());
  // Both scripted exchanges matched their wire content exactly -- the
  // mismatch is semantic (readback != written value), not a wire-level
  // one, so mismatch_count stays 0.
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// Transfer failure mid-sequence -> false, with the documented
// specimen-safety recovery: best-effort VBIAS-off after a failure past
// the VBIAS-on write, and no extra traffic when the VBIAS-on write itself
// is what failed.
// ---------------------------------------------------------------------

void TestTransferFailureDuringOneShotWriteAttemptsBiasOff() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgBiasOn);  // succeeds normally
  // FailNextTransfers() counts from "now" -- set before ReadOneShot()
  // even starts, it can only ever hit that call's *first* Transfer(),
  // never its second or third. To fail the 1SHOT write specifically (the
  // second Transfer() of this sequence) without disturbing its position
  // in the queue, script a deliberately-wrong exchange there instead: a
  // content mismatch makes Transfer() return false at exactly this point,
  // the same observable outcome the adapter reacts to.
  bus.Expect({0xEE, 0xEE}, {0xEE, 0xEE});
  ExpectConfigWrite(&bus, kCfgBiasOff);  // best-effort recovery write

  Max31865Adapter::Reading reading;
  std::string error;
  assert(!adapter.ReadOneShot(&reading, &error));
  assert(!error.empty());
  assert(bus.mismatch_count() == 1);
  assert(bus.remaining_expectations() == 0);  // the recovery write landed
}

void TestTransferFailureDuringRtdReadAttemptsBiasOff() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  ExpectConfigWrite(&bus, kCfgBiasOn);
  ExpectConfigWrite(&bus, kCfgBiasOn1Shot);
  // Same technique as above (see its comment), positioned at the RTD
  // MSB/LSB read (the third Transfer() of this sequence, a 3-byte
  // exchange).
  bus.Expect({0xEE, 0xEE, 0xEE}, {0xEE, 0xEE, 0xEE});
  ExpectConfigWrite(&bus, kCfgBiasOff);  // best-effort recovery write

  Max31865Adapter::Reading reading;
  std::string error;
  assert(!adapter.ReadOneShot(&reading, &error));
  assert(!error.empty());
  assert(bus.mismatch_count() == 1);
  assert(bus.remaining_expectations() == 0);
}

void TestTransferFailureDuringBiasOnWriteAttemptsNoExtraTransfer() {
  FakeSpiBus bus;
  const auto opts = TestOptions();
  Max31865Adapter adapter(&bus, opts);

  bus.FailNextTransfers(1);  // the very first write (VBIAS on) fails
  // Deliberately nothing else scripted: the failed write never reached
  // the chip, so there is no bias state to recover from. If the adapter
  // wrongly attempted a bias-off write here anyway, remaining_expectations
  // would still read 0 (nothing was queued to consume), which is why this
  // is checked together with the sibling tests above that DO expect a
  // recovery write to land -- together they isolate "recovers exactly
  // when it should, and only then."
  Max31865Adapter::Reading reading;
  std::string error;
  assert(!adapter.ReadOneShot(&reading, &error));
  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

// ---------------------------------------------------------------------
// C2(a) + C1 (click side): the adapter used to take NO bus lock at all --
// SensorManager's clicks_io_mu_ is click-vs-click only and knows nothing
// about the two TMC5160s on the same physical SPI0 controller. Each single
// register conversation must now be one hold of the per-CONTROLLER lock,
// with this opener's own mode re-applied inside it.
//
// Hand-computed expectations, pinned before the assertions:
//   * a healthy ReadOneShot() is exactly FOUR Transfer() calls:
//     WriteConfig(bias on), WriteConfig(bias on|1shot), ReadRtdCode (one
//     3-byte auto-increment read), WriteConfig(bias off). No fault read,
//     no FAULTCLR, because msb/lsb 0x40/0x00 -> code 8192, fault bit 0.
//   * so across the SECOND one-shot (bus already open, no Open() hold):
//     lock acquisitions +4, settings applications +4.
//   * this click sits on /dev/spidev0.1; the motors on /dev/spidev0.0 must
//     read the SAME counter — one controller, one lock.
// ---------------------------------------------------------------------

void ScriptHealthyOneShot(FakeSpiBus* bus) {
  ExpectConfigWrite(bus, kCfgBiasOn);
  ExpectConfigWrite(bus, kCfgBiasOn1Shot);
  ExpectRtdRead(bus, /*msb=*/0x40, /*lsb=*/0x00);
  ExpectConfigWrite(bus, kCfgBiasOff);
}

void TestEachTransferIsOneControllerLockHoldWithModeReapplied() {
  FakeSpiBus bus;
  const auto opts = TestOptions();  // spi_device = "/dev/spidev0.1"
  Max31865Adapter adapter(&bus, opts);

  // First cycle opens the bus (an extra hold we deliberately exclude).
  ScriptHealthyOneShot(&bus);
  Max31865Adapter::Reading reading;
  std::string error;
  assert(adapter.ReadOneShot(&reading, &error));
  assert(reading.valid);

  const std::uint64_t locks_before = SpiBusLockAcquireCount(opts.spi_device);
  const int applies_before = bus.settings_applications();

  ScriptHealthyOneShot(&bus);
  assert(adapter.ReadOneShot(&reading, &error));
  assert(reading.valid);

  assert(SpiBusLockAcquireCount(opts.spi_device) == locks_before + 4);
  assert(bus.settings_applications() == applies_before + 4);

  // Controller-keyed, not device-keyed: the motors' node sees these holds.
  assert(SpiBusLockAcquireCount("/dev/spidev0.0") == locks_before + 4);

  // Re-applied settings are the click's own (mode 1, native CE), not a
  // motor's (mode 3 | SPI_NO_CS).
  assert(bus.applied_mode() == 1);
  assert(bus.applied_no_cs() == false);
  assert(bus.applied_speed_hz() == opts.spi_speed_hz);

  assert(bus.mismatch_count() == 0);
  assert(bus.remaining_expectations() == 0);
}

}  // namespace

int main() {
  TestCodeToOhmsExactCases();
  TestOptionsDefaultsMatchDatasheetTimingFloors();
  TestOneShotHealthySequenceOrderAndOpenParams();
  TestFaultBitSetTriggersOutOfRangeAndFaultClear();
  TestNearFullScaleCodeTriggersOutOfRangeWithoutFaultBit();
  TestJustBelowFullScaleThresholdWithoutFaultBitIsValid();
  TestProbeSucceedsOnMatchingReadback();
  TestProbeReadbackMismatchReturnsFalse();
  TestTransferFailureDuringOneShotWriteAttemptsBiasOff();
  TestTransferFailureDuringRtdReadAttemptsBiasOff();
  TestTransferFailureDuringBiasOnWriteAttemptsNoExtraTransfer();
  TestEachTransferIsOneControllerLockHoldWithModeReapplied();
  return 0;
}
