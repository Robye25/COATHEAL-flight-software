#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "coatheal/hal/i2c_bus.hpp"

namespace coatheal {

// Serves a canned register image so register conversations can be tested
// with no hardware. Mirrors the LinuxI2cBus contract: a read starting past
// the end of the image fails, a read running off the end is short and
// therefore also fails, and — like the real bus's `fd_ < 0` guard — a read
// on a bus that has not been successfully opened (or has since been closed)
// fails rather than quietly succeeding.
class FakeI2cBus : public I2cBus {
 public:
  void SetImage(std::vector<std::uint8_t> image) { image_ = std::move(image); }
  void FailNextReads(int count) { fail_reads_ = count; }
  void SetMaxReadLength(std::size_t max_len) { max_read_len_ = max_len; }
  void SetOpenFails(bool value) { open_fails_ = value; }
  int open_count() const { return open_count_; }
  int address() const { return address_; }
  std::size_t last_read_length() const { return last_read_len_; }

  bool Open(int address) override {
    ++open_count_;
    address_ = address;
    open_ = !open_fails_;
    return open_;
  }

  bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                     std::size_t size) override {
    if (!open_ || data == nullptr) return false;
    last_read_len_ = size;
    if (fail_reads_ > 0) {
      --fail_reads_;
      return false;
    }
    if (size > max_read_len_) return false;
    if (static_cast<std::size_t>(reg) + size > image_.size()) return false;
    std::copy(image_.begin() + reg, image_.begin() + reg + size, data);
    return true;
  }

  void Close() override { open_ = false; }
  bool available() const override { return true; }

 private:
  std::vector<std::uint8_t> image_;
  int fail_reads_ = 0;
  std::size_t max_read_len_ = 32;
  bool open_fails_ = false;
  bool open_ = false;
  int open_count_ = 0;
  int address_ = -1;
  std::size_t last_read_len_ = 0;
};

}  // namespace coatheal
