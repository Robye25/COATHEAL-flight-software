#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace coatheal {

// =====================================================================
// THE SPI0 LOCKING RULE (schematic v3) — stated here, where the mutex
// lives, because every other file only references it.
//
// Four devices share ONE physical SPI0 controller and therefore one set of
// SCLK / MOSI / MISO wires:
//
//   motor0  TMC5160   soft CS BCM 22   /dev/spidev0.0   mode 3 | SPI_NO_CS
//   motor1  TMC5160   soft CS BCM 27   /dev/spidev0.0   mode 3 | SPI_NO_CS
//   click 2 MAX31865  native CE0       /dev/spidev0.0   mode 1
//   click 1 MAX31865  native CE1       /dev/spidev0.1   mode 1
//
// 1. THE LOCK IS KEYED PER CONTROLLER, NEVER PER DEVICE NODE.
//    /dev/spidev0.0 and /dev/spidev0.1 are two chip-select views of the
//    SAME wires; keying the mutex by the device string would hand click 1
//    a different mutex from everyone else and leave it free to clock the
//    bus in the middle of a motor datagram. SpiControllerKey() collapses
//    "/dev/spidevB.C" to "spiB" for exactly this reason.
//
// 2. EVERY DRIVER HOLDS SpiBusLock FOR ONE INDIVISIBLE BUS UNIT.
//    * TMC5160: cs-low -> [settings re-apply + data ioctl] -> cs-high, as
//      a single hold. That triplet is three syscalls; if a click message
//      interleaves inside it, the shared bus is clocked while the motor's
//      soft CS is still asserted, the motor latches a garbage 40-bit
//      datagram on CS rise (possibly as a WRITE — bit 7 of byte 0), and
//      both chips drive MISO at once.
//    * MAX31865: each single Transfer() — one complete 2- or 3-byte
//      register conversation, framed by the native CE line — plus the
//      Open() that programs the node's mode.
//
// 3. THE LOCK IS NOT HELD ACROSS THE MAX31865 ONE-SHOT'S SLEEPS.
//    The >=10 ms bias settle and >=65 ms conversion wait sit BETWEEN
//    transfers. Those gaps are CS-framed by the native CE line and need no
//    bus exclusivity, and holding across them would starve both motors for
//    ~150 ms per specimen poll.
//
// 4. THE LOCK DOES NOT REPLACE PER-TRANSFER SETTINGS RE-APPLICATION.
//    It serialises this process's own traffic; SpiBus::Transfer's
//    re-application (spi_bus.hpp) is what survives an opener outside this
//    process — and what makes rule 2's "one unit" self-consistent.
// =====================================================================

// Canonicalises a spidev path to its physical controller: "/dev/spidev0.1"
// and "/dev/spidev0.0" both map to "spi0". Unrecognised paths fall back to
// a "raw:"-prefixed copy of the string, which can never alias a canonical
// key.
std::string SpiControllerKey(const std::string& spi_device);

// Process-wide mutex for the physical SPI controller owning `spi_device`.
// Prefer SpiBusLock below; this accessor exists so tests can assert the
// controller-keying directly (two device nodes, one mutex object).
std::mutex& SpiBusMutex(const std::string& spi_device);

namespace internal {
// Per-controller mutex + acquisition counter. Opaque here: only
// spi_bus_lock.cpp needs its shape.
struct SpiControllerLock;
}  // namespace internal

// RAII hold of that mutex, plus the acquisition counter that makes the
// locking rule observable in unit tests (see SpiBusLockAcquireCount).
class SpiBusLock {
 public:
  explicit SpiBusLock(const std::string& spi_device);
  ~SpiBusLock();

  SpiBusLock(const SpiBusLock&) = delete;
  SpiBusLock& operator=(const SpiBusLock&) = delete;

 private:
  internal::SpiControllerLock& controller_;
};

// Test spy: total SpiBusLock acquisitions on the controller owning
// `spi_device` since process start. Monotonic, so tests compare a delta
// across the call under test rather than an absolute value. This is the
// whole lock-spy machinery — one counter, no injection, no instrumented
// mutex type — deliberately chosen over an injectable lock seam because
// the lock is process-wide state by definition and both drivers reach it
// through this one header.
std::uint64_t SpiBusLockAcquireCount(const std::string& spi_device);

}  // namespace coatheal
