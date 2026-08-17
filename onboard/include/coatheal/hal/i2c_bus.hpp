#pragma once

#include <cstddef>
#include <cstdint>

namespace coatheal {

// Read-only transport seam for byte-addressed I2C register devices.
//
// There is deliberately no write method. The Sequent RTD card's calibration
// and watchdog registers must never be written by flight software, and
// omitting writes from the seam makes that unrepresentable rather than
// merely documented.
class I2cBus {
 public:
  virtual ~I2cBus() = default;

  // Opens the bus and selects `address` as the active slave.
  virtual bool Open(int address) = 0;

  // Writes the register pointer, then reads `size` bytes into `data`.
  // Returns false on a short read.
  virtual bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                             std::size_t size) = 0;

  virtual void Close() = 0;

  // False on build hosts with no Linux I2C support, letting callers report
  // DISABLED rather than FAILED.
  virtual bool available() const = 0;
};

class LinuxI2cBus : public I2cBus {
 public:
  explicit LinuxI2cBus(const char* device = "/dev/i2c-1");
  ~LinuxI2cBus() override;

  LinuxI2cBus(const LinuxI2cBus&) = delete;
  LinuxI2cBus& operator=(const LinuxI2cBus&) = delete;

  bool Open(int address) override;
  bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                     std::size_t size) override;
  void Close() override;
  bool available() const override;

 private:
  const char* device_;
  int fd_ = -1;
};

}  // namespace coatheal
