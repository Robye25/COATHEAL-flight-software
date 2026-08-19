#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "coatheal/hal/spi_bus.hpp"

namespace coatheal {

// Scripts full-duplex SPI datagram exchanges so the TMC5160/MAX31865
// drivers can be tested with no hardware. Mirrors the LinuxSpiBus contract:
// Transfer fails unless a successful Open() preceded it (and after
// Close()) — like the real bus's fd_ < 0 guard — and it fails on a null
// tx/rx pointer.
//
// Strictness is deliberate: the fake I2C bus shipped more permissive than
// the real bus and cost a fix round. These SPI drivers cannot be exercised
// on real hardware from this Windows dev host, so a mismatched byte on the
// wire must fail the test loudly rather than pass silently.
class FakeSpiBus : public SpiBus {
 public:
  // Queues one scripted exchange: the next Transfer must send exactly `tx`
  // (same length, same bytes, in order). An expectation's rx must be
  // exactly len bytes — full-duplex SPI always fills the whole buffer, so
  // a short (or long) scripted rx is a mismatch, not a partial fill. On
  // match, `rx` is copied out in full and the expectation is popped. On
  // mismatch (tx content, tx length, or rx length), the expectation is
  // still popped, mismatch_count() increments, nothing is copied, and
  // Transfer returns false.
  void Expect(std::vector<std::uint8_t> tx, std::vector<std::uint8_t> rx) {
    expectations_.push_back({std::move(tx), std::move(rx)});
  }

  void FailNextTransfers(int count) { fail_transfers_ = count; }
  void SetOpenFails(bool value) { open_fails_ = value; }

  // Models an ABSENT device instead of a scripted one.
  //
  // This is the fidelity gap that let a real defect through: every
  // expectation above models a chip that ANSWERS, and SPI absence looks
  // nothing like that. SPI has no acknowledgement, so with no chip
  // populated (or none selected) the master still clocks a completely
  // successful transfer and simply samples whatever the idle MISO line
  // sits at -- 0x00 where it floats or is pulled low, 0xFF where a pull-up
  // holds it high. Both levels really occur in the field, which is why
  // this takes the level as a parameter rather than assuming zeros.
  //
  // So: transfers SUCCEED while the device is absent. That is the whole
  // hazard. A driver that treats "the transfer returned true" as "the
  // device is there" reports a healthy measurement of nothing at all, and
  // no amount of scripting can express that -- only this can.
  //
  // While a floating level is set the expectation queue is bypassed
  // entirely (a chip that is not there cannot answer a script) and
  // remaining expectations are left untouched.
  void SetFloatingLevel(std::uint8_t level) {
    floating_ = true;
    floating_level_ = level;
  }
  void ClearFloatingLevel() { floating_ = false; }

  const std::string& open_device() const { return open_device_; }
  std::uint8_t open_mode() const { return open_mode_; }
  std::uint32_t open_speed_hz() const { return open_speed_hz_; }
  bool open_no_cs() const { return open_no_cs_; }
  int open_count() const { return open_count_; }
  int mismatch_count() const { return mismatch_count_; }
  std::size_t remaining_expectations() const { return expectations_.size(); }

  // How many times SpiBus::Transfer re-applied this opener's own settings
  // to the (shared) node, and what it re-applied the last time. On real
  // hardware these are the SPI_IOC_WR_MODE/BITS/SPEED ioctls LinuxSpiBus
  // replays before each data ioctl so that motors (mode 3 | SPI_NO_CS) and
  // a click (mode 1, native CE0) can share /dev/spidev0.0 without one
  // opener's Open() muting the other. Recorders only — nothing about the
  // fake's strict tx/rx matching is relaxed by them.
  int settings_applications() const { return settings_applications_; }
  std::uint8_t applied_mode() const { return applied_mode_; }
  std::uint32_t applied_speed_hz() const { return applied_speed_hz_; }
  bool applied_no_cs() const { return applied_no_cs_; }

  bool Open(const std::string& device, std::uint8_t mode,
            std::uint32_t speed_hz, bool no_cs) override {
    ++open_count_;
    open_device_ = device;
    open_mode_ = mode;
    open_speed_hz_ = speed_hz;
    open_no_cs_ = no_cs;
    open_ = !open_fails_;
    return open_;
  }

 protected:
  bool ApplyBusSettings() override {
    // Mirrors LinuxSpiBus: no fd, nothing to apply — and that is what makes
    // Transfer() fail closed on an unopened or closed bus.
    if (!open_) return false;
    ++settings_applications_;
    applied_mode_ = open_mode_;
    applied_speed_hz_ = open_speed_hz_;
    applied_no_cs_ = open_no_cs_;
    return true;
  }

  bool TransferData(const std::uint8_t* tx, std::uint8_t* rx,
                    std::size_t len) override {
    if (!open_ || tx == nullptr || rx == nullptr) return false;

    if (fail_transfers_ > 0) {
      --fail_transfers_;
      return false;
    }

    // Absent device: the transfer succeeds and clocks back the idle line
    // level. Deliberately after the FailNextTransfers() hook -- a genuine
    // transport failure still wins -- and before the expectation queue,
    // which an absent chip can never reach.
    if (floating_) {
      std::fill(rx, rx + len, floating_level_);
      return true;
    }

    if (expectations_.empty()) return false;

    Exchange next = std::move(expectations_.front());
    expectations_.pop_front();

    bool matches = next.tx.size() == len && next.rx.size() == len &&
                   std::equal(next.tx.begin(), next.tx.end(), tx);
    if (!matches) {
      ++mismatch_count_;
      return false;
    }

    std::copy(next.rx.begin(), next.rx.end(), rx);
    return true;
  }

 public:
  void Close() override { open_ = false; }
  bool available() const override { return true; }

 private:
  struct Exchange {
    std::vector<std::uint8_t> tx;
    std::vector<std::uint8_t> rx;
  };

  std::deque<Exchange> expectations_;
  int fail_transfers_ = 0;
  bool floating_ = false;
  std::uint8_t floating_level_ = 0x00;
  bool open_fails_ = false;
  bool open_ = false;
  int open_count_ = 0;
  int mismatch_count_ = 0;
  std::string open_device_;
  std::uint8_t open_mode_ = 0;
  std::uint32_t open_speed_hz_ = 0;
  bool open_no_cs_ = false;
  int settings_applications_ = 0;
  std::uint8_t applied_mode_ = 0xFF;
  std::uint32_t applied_speed_hz_ = 0;
  bool applied_no_cs_ = false;
};

}  // namespace coatheal
