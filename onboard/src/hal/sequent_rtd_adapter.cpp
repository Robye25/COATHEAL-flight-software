#include "coatheal/hal/sequent_rtd_adapter.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coatheal {

namespace {

void SetError(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
}

double FloatAt(const std::uint8_t* bytes) {
  float value = 0.0f;
  std::memcpy(&value, bytes, sizeof(float));
  return static_cast<double>(value);
}

}  // namespace

SequentRtdAdapter::SequentRtdAdapter(I2cBus* bus, const Options& options)
    : bus_(bus), options_(options) {}

bool SequentRtdAdapter::EnsureOpen(std::string* error) {
  if (bus_ == nullptr) {
    SetError(error, "NO_BUS");
    return false;
  }
  if (options_.stack < kStackMin || options_.stack > kStackMax) {
    SetError(error, "STACK_OUT_OF_RANGE");
    return false;
  }
  if (open_) return true;
  if (!bus_->Open(address())) {
    SetError(error, "BUS_OPEN_FAILED");
    return false;
  }
  open_ = true;
  return true;
}

bool SequentRtdAdapter::Probe(Identity* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "NULL_OUT");
    return false;
  }
  if (!EnsureOpen(error)) return false;

  // Failure exits below fall into two classes, and they are handled
  // differently on purpose:
  //   - I/O failures (a ReadRegisters call itself returns false) mean the
  //     bus conversation failed, so the connection is suspect. We clear
  //     open_ so the next Probe() re-opens rather than trusting a link that
  //     just misbehaved.
  //   - Configuration rejections (card type too old to verify, or the
  //     card's configured sensor type doesn't match what we expect) mean
  //     the bus is healthy and answered correctly, but the card is not the
  //     one we require. Re-opening cannot change that answer, so open_ is
  //     left as-is.

  // Presence is proven exactly as the vendor's doBoardInit does: read the
  // firmware revision and require success.
  std::uint8_t rev[2] = {0, 0};
  if (!bus_->ReadRegisters(sequent_rtd::kRevMajor, rev, sizeof(rev))) {
    SetError(error, "CARD_NOT_DETECTED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  out->fw_major = rev[0];
  out->fw_minor = rev[1];

  std::uint8_t hw[2] = {0, 0};
  if (!bus_->ReadRegisters(sequent_rtd::kRevHwMajor, hw, sizeof(hw))) {
    SetError(error, "HW_REVISION_READ_FAILED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  out->hw_major = hw[0];
  out->hw_minor = hw[1];

  std::uint8_t card_type = 0;
  if (!bus_->ReadRegisters(sequent_rtd::kCardType, &card_type, 1)) {
    SetError(error, "CARD_TYPE_READ_FAILED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  out->card_type = card_type;

  // Vendor gates sensor-type access on card type >= 1 ("Available only for
  // hardware version >= 5.0"). Below that we cannot confirm the card is
  // configured for the probes actually wired to it, so refuse rather than
  // read plausibly-wrong temperatures. Configuration rejection: open_ is
  // left as-is, per the comment above.
  if (card_type < 1) {
    SetError(error, "SENSOR_TYPE_UNVERIFIABLE_OLD_HARDWARE");
    return false;
  }

  std::uint8_t sensor = 0;
  if (!bus_->ReadRegisters(sequent_rtd::kPt1000, &sensor, 1)) {
    SetError(error, "SENSOR_TYPE_READ_FAILED");
    open_ = false;  // I/O failure: force a re-open next attempt.
    return false;
  }
  out->pt1000 = (sensor & 0x0FU) != 0U;

  if (out->pt1000 != options_.expect_pt1000) {
    SetError(error, "SENSOR_TYPE_MISMATCH");
    // Configuration rejection: open_ is left as-is, per the comment above.
    return false;
  }

  SetError(error, "");
  return true;
}

// Reads eight consecutive float32 values starting at `base`, then applies
// the 1-indexed channel map. Tries one 32-byte burst first for a
// time-coherent snapshot across channels; if the firmware refuses reads
// longer than four bytes, latches into per-channel mode permanently.
bool SequentRtdAdapter::ReadFloatBlock(int base,
                                       std::array<double, kChannelCount>* out) {
  std::uint8_t raw[kChannelCount * 4] = {};

  if (burst_mode_) {
    if (bus_->ReadRegisters(static_cast<std::uint8_t>(base), raw, sizeof(raw))) {
      for (std::size_t i = 0; i < kChannelCount; ++i) {
        const std::uint8_t channel = options_.channel_map[i];
        (*out)[i] = FloatAt(raw + 4 * (channel - 1));
      }
      return true;
    }
    burst_mode_ = false;
  }

  for (std::size_t i = 0; i < kChannelCount; ++i) {
    const std::uint8_t channel = options_.channel_map[i];
    std::uint8_t bytes[4] = {};
    const int offset = base + 4 * (static_cast<int>(channel) - 1);
    if (!bus_->ReadRegisters(static_cast<std::uint8_t>(offset), bytes,
                             sizeof(bytes))) {
      return false;
    }
    (*out)[i] = FloatAt(bytes);
  }
  return true;
}

bool SequentRtdAdapter::ReadAll(Reading* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "NULL_OUT");
    return false;
  }
  if (!EnsureOpen(error)) return false;

  if (!ReadFloatBlock(sequent_rtd::kRtdVal1, &out->temperature_c)) {
    SetError(error, "TEMPERATURE_READ_FAILED");
    open_ = false;
    return false;
  }
  if (!ReadFloatBlock(sequent_rtd::kRtdRes1, &out->resistance_ohm)) {
    SetError(error, "RESISTANCE_READ_FAILED");
    open_ = false;
    return false;
  }

  // Diagnostics only. Byte interpretations are inferred from the register
  // map; a wrong guess degrades a log line, never a control value.
  std::uint8_t diag[3] = {};
  if (bus_->ReadRegisters(sequent_rtd::kDiagTemp, diag, sizeof(diag))) {
    out->card_temp_c = static_cast<double>(static_cast<std::int8_t>(diag[0]));
    const std::uint16_t millivolts =
        static_cast<std::uint16_t>(diag[1]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(diag[2]) << 8U);
    out->rail_5v = static_cast<double>(millivolts) / 1000.0;
  }

  std::uint8_t reinit[4] = {};
  if (bus_->ReadRegisters(sequent_rtd::kRtdReinit, reinit, sizeof(reinit))) {
    out->adc_reinit_count = static_cast<std::uint32_t>(reinit[0]) |
                            (static_cast<std::uint32_t>(reinit[1]) << 8U) |
                            (static_cast<std::uint32_t>(reinit[2]) << 16U) |
                            (static_cast<std::uint32_t>(reinit[3]) << 24U);
  }

  out->channel_valid.fill(true);  // narrowed in Task 4
  SetError(error, "");
  return true;
}

}  // namespace coatheal
