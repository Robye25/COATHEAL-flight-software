#include "coatheal/hal/max31865_adapter.hpp"

#include <chrono>
#include <thread>

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
// Probe()'s benign value: no VBIAS, no 1SHOT -- see the header comment on
// why that makes the readback check a plain equality.
constexpr std::uint8_t kCfgProbe = kBit50Hz;                             // 0x01

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
  if (!bus_->Open(options_.spi_device, /*mode=*/1, options_.spi_speed_hz,
                  /*no_cs=*/false)) {
    SetError(error, "BUS_OPEN_FAILED");
    return false;
  }
  open_ = true;
  return true;
}

bool Max31865Adapter::WriteConfig(std::uint8_t value) {
  std::uint8_t tx[2] = {static_cast<std::uint8_t>(kRegConfig | kWriteBit),
                        value};
  std::uint8_t rx[2] = {0, 0};
  return bus_->Transfer(tx, rx, 2);
}

bool Max31865Adapter::ReadRegister(std::uint8_t addr, std::uint8_t* value) {
  std::uint8_t tx[2] = {static_cast<std::uint8_t>(addr & 0x7FU), 0};
  std::uint8_t rx[2] = {0, 0};
  if (!bus_->Transfer(tx, rx, 2)) return false;
  *value = rx[1];
  return true;
}

bool Max31865Adapter::ReadRtdCode(std::uint8_t* msb, std::uint8_t* lsb) {
  // Auto-increment: one 3-byte exchange reads RTD MSB (0x01) then LSB
  // (0x02) in a single time-coherent conversation.
  std::uint8_t tx[3] = {kRegRtdMsb, 0, 0};
  std::uint8_t rx[3] = {0, 0, 0};
  if (!bus_->Transfer(tx, rx, 3)) return false;
  *msb = rx[1];
  *lsb = rx[2];
  return true;
}

double Max31865Adapter::CodeToOhms(std::uint16_t code, double reference_ohm) {
  return static_cast<double>(code) * reference_ohm / 32768.0;
}

bool Max31865Adapter::Probe(std::string* error) {
  if (!EnsureOpen(error)) return false;

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
  if (readback != kCfgProbe) {
    // The bus conversation itself succeeded; the click just isn't
    // answering as configured. Configuration rejection, not an I/O
    // failure -- open_ is left as-is (mirrors SequentRtdAdapter's
    // I/O-failure-vs-configuration-rejection convention).
    SetError(error, "CONFIG_READBACK_MISMATCH");
    return false;
  }
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
