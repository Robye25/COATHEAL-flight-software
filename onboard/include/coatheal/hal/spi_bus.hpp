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
  //
  // NON-VIRTUAL on purpose. The kernel keeps ONE `struct spi_device` per
  // /dev/spidevB.C node, and mode/speed/bits-per-word set by ANY opener of
  // that node apply to every opener of it. Three devices open
  // /dev/spidev0.0 here (motor0 and motor1 at mode 3 | SPI_NO_CS, MAX31865
  // click 2 at mode 1 with native CE0), so "last opener wins" would leave
  // click 2 mute (CE never asserts under SPI_NO_CS) or run the motors at
  // the wrong CPOL/CPHA with CE0 firing on every datagram. Making this
  // method non-virtual guarantees, structurally and in exactly one place,
  // that every implementation re-applies its OWN settings immediately
  // before the data exchange — so each transfer is self-consistent no
  // matter which other opener touched the node in between. Callers make the
  // pair indivisible by holding SpiBusLock (hal/spi_bus_lock.hpp) across
  // the call; see the locking rule stated there.
  bool Transfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) {
    if (tx == nullptr || rx == nullptr) return false;
    if (!ApplyBusSettings()) return false;
    return TransferData(tx, rx, len);
  }

  virtual void Close() = 0;

  // False on build hosts with no Linux SPI support, letting callers report
  // DISABLED rather than FAILED.
  virtual bool available() const = 0;

 protected:
  // Re-asserts this opener's own mode / speed / bits-per-word on the shared
  // node. Called by Transfer() before every data exchange (see above).
  // Returns false when the bus is not open, which is what makes Transfer()
  // fail closed on an unopened bus.
  virtual bool ApplyBusSettings() = 0;

  // The data exchange itself, with settings already re-applied.
  virtual bool TransferData(const std::uint8_t* tx, std::uint8_t* rx,
                            std::size_t len) = 0;
};

class LinuxSpiBus : public SpiBus {
 public:
  LinuxSpiBus();
  ~LinuxSpiBus() override;

  LinuxSpiBus(const LinuxSpiBus&) = delete;
  LinuxSpiBus& operator=(const LinuxSpiBus&) = delete;

  bool Open(const std::string& device, std::uint8_t mode,
            std::uint32_t speed_hz, bool no_cs) override;
  void Close() override;
  bool available() const override;

 protected:
  bool ApplyBusSettings() override;
  bool TransferData(const std::uint8_t* tx, std::uint8_t* rx,
                    std::size_t len) override;

 private:
  int fd_ = -1;
  // Settings this opener asked for, replayed onto the shared node before
  // every transfer. wire_mode_ already carries SPI_NO_CS when requested.
  std::uint8_t wire_mode_ = 0;
  std::uint8_t bits_per_word_ = 8;
  std::uint32_t speed_hz_ = 0;
};

}  // namespace coatheal
