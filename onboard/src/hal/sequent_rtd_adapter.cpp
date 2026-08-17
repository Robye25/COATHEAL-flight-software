#include "coatheal/hal/sequent_rtd_adapter.hpp"

#include <cstring>

namespace coatheal {

namespace {

void SetError(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
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

bool SequentRtdAdapter::ReadFloatBlock(int, std::array<double, kChannelCount>*) {
  return false;  // Task 3
}

bool SequentRtdAdapter::ReadAll(Reading*, std::string* error) {
  SetError(error, "NOT_IMPLEMENTED");
  return false;  // Task 3
}

}  // namespace coatheal
