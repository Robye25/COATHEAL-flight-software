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
  // (same length, same bytes, in order). On match, `rx` is copied out (up
  // to the shorter of `rx.size()`/`len`) and the expectation is popped. On
  // mismatch, the expectation is still popped, mismatch_count() increments,
  // and Transfer returns false.
  void Expect(std::vector<std::uint8_t> tx, std::vector<std::uint8_t> rx) {
    expectations_.push_back({std::move(tx), std::move(rx)});
  }

  void FailNextTransfers(int count) { fail_transfers_ = count; }
  void SetOpenFails(bool value) { open_fails_ = value; }

  const std::string& open_device() const { return open_device_; }
  std::uint8_t open_mode() const { return open_mode_; }
  std::uint32_t open_speed_hz() const { return open_speed_hz_; }
  bool open_no_cs() const { return open_no_cs_; }
  int open_count() const { return open_count_; }
  int mismatch_count() const { return mismatch_count_; }
  std::size_t remaining_expectations() const { return expectations_.size(); }

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

  bool Transfer(const std::uint8_t* tx, std::uint8_t* rx,
                std::size_t len) override {
    if (!open_ || tx == nullptr || rx == nullptr) return false;

    if (fail_transfers_ > 0) {
      --fail_transfers_;
      return false;
    }

    if (expectations_.empty()) return false;

    Exchange next = std::move(expectations_.front());
    expectations_.pop_front();

    bool matches =
        next.tx.size() == len && std::equal(next.tx.begin(), next.tx.end(), tx);
    if (!matches) {
      ++mismatch_count_;
      return false;
    }

    std::size_t n = std::min(next.rx.size(), len);
    std::copy(next.rx.begin(), next.rx.begin() + n, rx);
    return true;
  }

  void Close() override { open_ = false; }
  bool available() const override { return true; }

 private:
  struct Exchange {
    std::vector<std::uint8_t> tx;
    std::vector<std::uint8_t> rx;
  };

  std::deque<Exchange> expectations_;
  int fail_transfers_ = 0;
  bool open_fails_ = false;
  bool open_ = false;
  int open_count_ = 0;
  int mismatch_count_ = 0;
  std::string open_device_;
  std::uint8_t open_mode_ = 0;
  std::uint32_t open_speed_hz_ = 0;
  bool open_no_cs_ = false;
};

}  // namespace coatheal
