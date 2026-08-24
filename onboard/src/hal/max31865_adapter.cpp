#include "coatheal/hal/max31865_adapter.hpp"

#include <chrono>
#include <thread>

#include "coatheal/hal/spi_bus_lock.hpp"

namespace coatheal {

namespace {

void SetError(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
}

constexpr std::uint8_t kRegConfig = 0x00;
constexpr std::uint8_t kRegRtdMsb = 0x01;
constexpr std::uint8_t kRegFaultStatus = 0x07;
constexpr std::uint8_t kWriteBit = 0x80;

constexpr std::uint8_t kBitVbias = 0x80;
constexpr std::uint8_t kBit1Shot = 0x20;
// 3WIRE (bit4) is never set below -- schematic v3 wires both clicks
// 4-wire Kelvin.
constexpr std::uint8_t kBitFaultClr = 0x02;
constexpr std::uint8_t kBit50Hz = 0x01;

constexpr std::uint8_t kCfgBiasOn = kBitVbias | kBit50Hz;                // 0x81
constexpr std::uint8_t kCfgBiasOn1Shot =
    kBitVbias | kBit1Shot | kBit50Hz;                                    // 0xA1
constexpr std::uint8_t kCfgBiasOff = kBit50Hz;                           // 0x01
// Issued after ReadOneShot() has already written VBIAS off, so this
// carries VBIAS=0 forward and adds FAULTCLR.
constexpr std::uint8_t kCfgFaultClear = kBitFaultClr | kBit50Hz;         // 0x03
// The presence check's benign value: no VBIAS, no 1SHOT -- see the header
// comment on why that makes the readback check a plain equality.
//
// Byte-identical to kCfgBiasOff on purpose: this IS the adapter's resting
// config, so writing it can never perturb the conversion that follows and
// can never leave the 50 Hz filter bit anywhere other than where every
// other write in this file leaves it (set). Every constant above carries
// kBit50Hz for exactly that reason -- the notch never moves between
// conversions, so a presence check can never change a reading.
constexpr std::uint8_t kCfgProbe = kBit50Hz;                             // 0x01

// Nothing answered as configured on a bus conversation that itself worked.
// Deliberately distinct from every transport error string below: "the
// click is not there / is not answering" and "the transfer failed" call
// for different actions on the ground.
constexpr char kClickNotDetected[] = "CLICK_NOT_DETECTED";

}  // namespace

Max31865Adapter::Max31865Adapter(SpiBus* bus, const Options& options)
    : bus_(bus), options_(options) {}

bool Max31865Adapter::EnsureOpen(std::string* error) {
  if (bus_ == nullptr) {
    SetError(error, "NO_BUS");
    return false;
  }
  if (open_) return true;
  // Native CE (no_cs=false): CE0/CE1 are hard-wired to the MAX31865
  // clicks, unlike the TMC5160 steppers sharing this SPI0 bus on soft
  // GPIO chip-selects.
  //
  // Under the controller lock: Open() programs the shared node's mode and
  // must not land inside another driver's transfer unit. See
  // hal/spi_bus_lock.hpp for the full rule.
  bool opened = false;
  {
    SpiBusLock bus_lock(options_.spi_device);
    opened = bus_->Open(options_.spi_device, /*mode=*/1, options_.spi_speed_hz,
                        /*no_cs=*/false);
  }
  if (!opened) {
    SetError(error, "BUS_OPEN_FAILED");
    return false;
  }
  open_ = true;
  return true;
}

// Each of the three helpers below is exactly ONE register conversation, and
// each takes the per-controller bus lock for exactly that conversation --
// the MAX31865 half of the locking rule in hal/spi_bus_lock.hpp. The lock is
// deliberately NOT held by the callers across the one-shot's settle and
// conversion sleeps: those gaps are CS-framed by the native CE line, so the
// motors are free to use the bus during them.

bool Max31865Adapter::WriteConfig(std::uint8_t value) {
  std::uint8_t tx[2] = {static_cast<std::uint8_t>(kRegConfig | kWriteBit),
                        value};
  std::uint8_t rx[2] = {0, 0};
  SpiBusLock bus_lock(options_.spi_device);
  return bus_->Transfer(tx, rx, 2);
}

bool Max31865Adapter::ReadRegister(std::uint8_t addr, std::uint8_t* value) {
  std::uint8_t tx[2] = {static_cast<std::uint8_t>(addr & 0x7FU), 0};
  std::uint8_t rx[2] = {0, 0};
  {
    SpiBusLock bus_lock(options_.spi_device);
    if (!bus_->Transfer(tx, rx, 2)) return false;
  }
  *value = rx[1];
  return true;
}

bool Max31865Adapter::ReadRtdCode(std::uint8_t* msb, std::uint8_t* lsb) {
  // Auto-increment: one 3-byte exchange reads RTD MSB (0x01) then LSB
  // (0x02) in a single time-coherent conversation.
  std::uint8_t tx[3] = {kRegRtdMsb, 0, 0};
  std::uint8_t rx[3] = {0, 0, 0};
  {
    SpiBusLock bus_lock(options_.spi_device);
    if (!bus_->Transfer(tx, rx, 3)) return false;
  }
  *msb = rx[1];
  *lsb = rx[2];
  return true;
}

double Max31865Adapter::CodeToOhms(std::uint16_t code, double reference_ohm) {
  return static_cast<double>(code) * reference_ohm / 32768.0;
}

// Presence proof: write the benign config value, read it straight back.
// Two register conversations, each taking the controller lock for itself
// (see the block comment above) and no sleeps between them, so this adds
// nothing to the time the motors are locked out of the bus.
//
// THIS IS THE ONLY THING BETWEEN AN ABSENT CLICK AND A PLAUSIBLE-LOOKING
// MEASUREMENT. SPI has no acknowledgement. With no chip answering, the
// master still clocks a perfectly "successful" transfer and simply samples
// whatever the idle MISO line sits at -- 0x00 where it floats or is pulled
// low, 0xFF where it is pulled high. So every transfer of a one-shot
// sequence SUCCEEDS against a click that is not there: the RTD code reads
// back 0x0000, the fault bit is clear, and the adapter hands out a
// perfectly healthy 0-ohm reading. That is exactly what an unpopulated
// flight stack reported on the bench -- RESISTANCE_OK on the wire with
// every resistance channel blank -- and it is why this check exists.
// Contrast the neighbouring devices, which self-detect: an absent I2C card
// never ACKs (SequentRtdAdapter reports CARD_NOT_DETECTED) and the TMC5160
// has a VERSION identity byte. The MAX31865 has neither, so presence has
// to be proven explicitly.
//
// kCfgProbe (0x01) is chosen so neither idle level can forge the readback:
// it is neither 0x00 nor 0xFF.
bool Max31865Adapter::VerifyPresence(std::string* error) {
  if (!WriteConfig(kCfgProbe)) {
    SetError(error, "CONFIG_WRITE_FAILED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  std::uint8_t readback = 0;
  if (!ReadRegister(kRegConfig, &readback)) {
    SetError(error, "CONFIG_READ_FAILED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  // Mask the auto-clearing 1SHOT bit out of the comparison. If a previous
  // process instance was killed between "write 1SHOT" and the conversion
  // completing (systemd's watchdog SIGABRT did exactly this on the bench),
  // the chip keeps the conversion pending -- and with VBIAS off it never
  // completes, so bit 5 reads back as 1 indefinitely. That chip is present
  // and healthy; an exact-equality check declared it CLICK_NOT_DETECTED on
  // every boot until power cycle. Neither idle-line forgery level (0x00 /
  // 0xFF) survives the masked comparison either, so the absent-click
  // detection this check exists for is unchanged.
  if ((readback & static_cast<std::uint8_t>(~kBit1Shot)) != kCfgProbe) {
    // The bus conversation itself succeeded; the click just isn't
    // answering as configured. Configuration rejection, not an I/O
    // failure -- open_ is left as-is (mirrors SequentRtdAdapter's
    // I/O-failure-vs-configuration-rejection convention).
    SetError(error, kClickNotDetected);
    return false;
  }
  return true;
}

bool Max31865Adapter::Probe(std::string* error) {
  if (!EnsureOpen(error)) return false;
  if (!VerifyPresence(error)) return false;
  SetError(error, "");
  return true;
}

bool Max31865Adapter::ReadOneShot(Reading* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "NULL_OUT");
    return false;
  }
  *out = Reading{};
  if (!EnsureOpen(error)) return false;

  // Presence FIRST, before VBIAS is ever asserted. Folding it in here
  // rather than leaving it to the callers is the point: ReadOneShot is the
  // only entry point the poll loop and CHECK both go through, so a click
  // that is not on the bus can no longer report a successful conversation
  // by any route. Two register conversations buy that; a doomed one-shot
  // would instead burn the full ~75 ms of settle + conversion sleeps
  // before returning a fake 0-ohm reading.
  //
  // No bias-off recovery is needed on this path: VBIAS has not been
  // touched yet and kCfgProbe leaves it off. `*out` was reset above, so a
  // caller that ignores the return value still sees valid == false.
  if (!VerifyPresence(error)) return false;

  if (!WriteConfig(kCfgBiasOn)) {
    SetError(error, "BIAS_ON_WRITE_FAILED");
    open_ = false;
    // The write never reached the chip: its VBIAS state is whatever it
    // already was (this adapter always leaves VBIAS off at the end of a
    // successful cycle), so there is nothing to recover here.
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(options_.settle_ms));

  if (!WriteConfig(kCfgBiasOn1Shot)) {
    SetError(error, "ONE_SHOT_WRITE_FAILED");
    open_ = false;
    // Specimen safety: the VBIAS-on write above succeeded, so bias is now
    // on, but the chip never got the 1SHOT command. Best-effort recovery
    // write; its own failure is not separately reported (already
    // unhealthy) -- the alternative is leaving the specimen self-heating
    // indefinitely, which is worse.
    WriteConfig(kCfgBiasOff);
    return false;
  }
  std::this_thread::sleep_for(
      std::chrono::milliseconds(options_.conversion_ms));

  std::uint8_t msb = 0;
  std::uint8_t lsb = 0;
  if (!ReadRtdCode(&msb, &lsb)) {
    SetError(error, "RTD_READ_FAILED");
    open_ = false;
    // Same specimen-safety reasoning as above: bias is on, the conversion
    // may even have completed, but the result couldn't be read out.
    // Best-effort bias-off rather than leaving it energized.
    WriteConfig(kCfgBiasOff);
    return false;
  }

  if (!WriteConfig(kCfgBiasOff)) {
    SetError(error, "BIAS_OFF_WRITE_FAILED");
    open_ = false;
    return false;
  }

  const std::uint16_t raw = (static_cast<std::uint16_t>(msb) << 8) |
                            static_cast<std::uint16_t>(lsb);
  const bool fault_flag = (lsb & 0x01U) != 0U;
  const std::uint16_t code = static_cast<std::uint16_t>(raw >> 1);

  // Diagnostic only -- computed unconditionally, trusted only when valid.
  out->resistance_ohm = CodeToOhms(code, options_.reference_ohm);

  const bool saturated = fault_flag || code >= kNearFullScaleCode;
  if (saturated) {
    out->valid = false;
    out->out_of_range = true;
    std::uint8_t fault_reg = 0;
    if (ReadRegister(kRegFaultStatus, &fault_reg)) {
      out->fault_bits = fault_reg;
    }
    // Bias is already off (write above), so this carries VBIAS=0 forward
    // and adds FAULTCLR. Best-effort: a failure here doesn't change the
    // Reading already computed, it just leaves the fault latched on the
    // chip until the next cycle.
    WriteConfig(kCfgFaultClear);
  } else {
    out->valid = true;
    out->out_of_range = false;
  }

  SetError(error, "");
  return true;
}

}  // namespace coatheal
