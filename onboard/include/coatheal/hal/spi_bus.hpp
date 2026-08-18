#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coatheal {

// Transport seam shared by every SPI0 device: two MAX31865 clicks on
// hardware CE0/CE1 and two TMC5160 steppers on soft GPIO chip-selects.
// Mirrors I2cBus.
class SpiBus {
 public:
  virtual ~SpiBus() = default;

  // Opens `device` (e.g. "/dev/spidev0.0") with the given SPI mode and
  // clock. no_cs: caller frames the transaction with its own GPIO
  // chip-select; the kernel must NOT assert CE0/CE1 (they are wired to the
  // MAX31865 clicks) — load-bearing for the TMC5160 soft-CS devices, which
  // share SPI0 with the clicks.
  virtual bool Open(const std::string& device, std::uint8_t mode,
                    std::uint32_t speed_hz, bool no_cs) = 0;

  // Full-duplex exchange of `len` bytes: `tx` is clocked out while `rx` is
  // clocked in, one bit per clock. Returns false on a null tx/rx pointer,
  // a bus that is not open, or an I/O failure.
  virtual bool Transfer(const std::uint8_t* tx, std::uint8_t* rx,
                        std::size_t len) = 0;

  virtual void Close() = 0;

  // False on build hosts with no Linux SPI support, letting callers report
  // DISABLED rather than FAILED.
  virtual bool available() const = 0;
};

class LinuxSpiBus : public SpiBus {
 public:
  LinuxSpiBus();
  ~LinuxSpiBus() override;

  LinuxSpiBus(const LinuxSpiBus&) = delete;
  LinuxSpiBus& operator=(const LinuxSpiBus&) = delete;

  bool Open(const std::string& device, std::uint8_t mode,
            std::uint32_t speed_hz, bool no_cs) override;
  bool Transfer(const std::uint8_t* tx, std::uint8_t* rx,
                std::size_t len) override;
  void Close() override;
  bool available() const override;

 private:
  int fd_ = -1;
};

}  // namespace coatheal
