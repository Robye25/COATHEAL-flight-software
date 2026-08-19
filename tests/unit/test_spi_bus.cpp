// Contract tests for the SPI transport seam and its test double.

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "coatheal/hal/spi_bus.hpp"
#include "coatheal/hal/spi_bus_lock.hpp"
#include "fake_spi_bus.hpp"

using namespace coatheal;

namespace {

void TestFakeRejectsTransferBeforeOpen() {
  // Mirrors LinuxSpiBus returning false when fd_ < 0; a driver that forgets
  // to Open() must fail here rather than on hardware.
  FakeSpiBus bus;
  bus.Expect({0x01, 0x02}, {0xAA, 0xBB});

  std::uint8_t tx[2] = {0x01, 0x02};
  std::uint8_t rx[2] = {};
  assert(!bus.Transfer(tx, rx, 2));
}

void TestFakeRejectsTransferAfterClose() {
  FakeSpiBus bus;
  bus.Expect({0x01}, {0xAA});
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));

  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(bus.Transfer(tx, rx, 1));

  bus.Close();
  // Same tx bytes as before: if this fails, it must be because the bus is
  // closed, not because of an unrelated content mismatch (isolation).
  bus.Expect({0x01}, {0xBB});
  assert(!bus.Transfer(tx, rx, 1));
}

void TestFakeRejectsTransferAfterFailedOpen() {
  // A failed Open() must leave the bus just as unusable as one that was
  // never opened — mirrors LinuxSpiBus, where a failed ::open()/ioctl()
  // leaves fd_ at -1.
  FakeSpiBus bus;
  bus.SetOpenFails(true);
  assert(!bus.Open("/dev/spidev0.0", 3, 1000000, false));

  bus.Expect({0x01}, {0xAA});
  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(!bus.Transfer(tx, rx, 1));
}

void TestFakeRejectsNullPointers() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0x01}, {0xAA});

  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(!bus.Transfer(nullptr, rx, 1));
  assert(!bus.Transfer(tx, nullptr, 1));
}

void TestFakeServesScriptedExchange() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0xAD, 0x00, 0x00, 0x00, 0x05}, {0x20, 0x00, 0x00, 0x00, 0x00});

  std::uint8_t tx[5] = {0xAD, 0x00, 0x00, 0x00, 0x05};
  std::uint8_t rx[5] = {};
  assert(bus.Transfer(tx, rx, 5));
  assert(rx[0] == 0x20 && rx[1] == 0x00 && rx[2] == 0x00 && rx[3] == 0x00 &&
         rx[4] == 0x00);
  assert(bus.remaining_expectations() == 0);
  assert(bus.mismatch_count() == 0);
}

void TestFakeDetectsTxContentMismatch() {
  // A driver that puts the wrong byte on the wire must be caught, not
  // silently accepted — this is the exact failure mode the I2C fake's
  // permissiveness missed.
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0xAD, 0x00, 0x00, 0x00, 0x05}, {0x20, 0x00, 0x00, 0x00, 0x00});

  std::uint8_t tx[5] = {0xAD, 0x00, 0x00, 0x00, 0x06};  // last byte wrong
  std::uint8_t rx[5] = {};
  assert(!bus.Transfer(tx, rx, 5));
  assert(bus.mismatch_count() == 1);
  assert(bus.remaining_expectations() == 0);
}

void TestFakeDetectsLengthMismatch() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0xAD, 0x00, 0x00, 0x00, 0x05}, {0x20, 0x00, 0x00, 0x00, 0x00});

  std::uint8_t tx[4] = {0xAD, 0x00, 0x00, 0x00};  // one byte short
  std::uint8_t rx[4] = {};
  assert(!bus.Transfer(tx, rx, 4));
  assert(bus.mismatch_count() == 1);
}

void TestFakeRejectsShortScriptedRx() {
  // Real SPI is full-duplex: every Transfer clocks exactly `len` bytes into
  // rx, no exceptions — the fake must not permit a bus state hardware can't
  // produce. A scripted rx shorter than len (e.g. an author dropping the
  // TMC5160 datagram's leading status byte) must be a mismatch, not a
  // partial fill that leaves the rest of rx holding stale/uninitialized
  // bytes. The sentinel fill is what makes this load-bearing: it proves no
  // partial copy happened, not just that the call returned false.
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0x01, 0x02}, {0xAA});  // rx one byte short of len=2

  std::uint8_t tx[2] = {0x01, 0x02};
  std::uint8_t rx[2] = {0x5A, 0x5A};  // sentinel: must stay untouched
  assert(!bus.Transfer(tx, rx, 2));
  assert(bus.mismatch_count() == 1);
  assert(rx[0] == 0x5A && rx[1] == 0x5A);
}

void TestFakeInjectsTransferFailures() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));
  bus.Expect({0x01}, {0xAA});
  bus.FailNextTransfers(2);

  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(!bus.Transfer(tx, rx, 1));
  assert(!bus.Transfer(tx, rx, 1));
  // Injected failures must not consume the queued expectation.
  assert(bus.remaining_expectations() == 1);
  assert(bus.mismatch_count() == 0);
  assert(bus.Transfer(tx, rx, 1));
  assert(rx[0] == 0xAA);
}

void TestFakeExhaustedQueueTransferFails() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 1000000, false));

  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(!bus.Transfer(tx, rx, 1));
  // Running out of script is not a content mismatch.
  assert(bus.mismatch_count() == 0);
}

void TestFakeRecordsOpenParameters() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.1", 1, 500000, true));
  assert(bus.open_device() == "/dev/spidev0.1");
  assert(bus.open_mode() == 1);
  assert(bus.open_speed_hz() == 500000u);
  assert(bus.open_no_cs() == true);
  assert(bus.open_count() == 1);

  assert(bus.Open("/dev/spidev0.0", 3, 8000000, false));
  assert(bus.open_device() == "/dev/spidev0.0");
  assert(bus.open_mode() == 3);
  assert(bus.open_speed_hz() == 8000000u);
  assert(bus.open_no_cs() == false);
  assert(bus.open_count() == 2);
}

// ---------------------------------------------------------------------
// C1: settings re-application before EVERY data exchange.
//
// Three devices open /dev/spidev0.0 (motor0/motor1 at mode 3 | SPI_NO_CS,
// MAX31865 click 2 at mode 1 with native CE0) and the kernel keeps ONE
// struct spi_device per node, so whoever opened last owns the mode until
// somebody re-asserts it. SpiBus::Transfer is non-virtual precisely so no
// implementation can skip that re-assertion; these tests pin the contract
// at the seam, where both the real bus and the fake inherit it.
// ---------------------------------------------------------------------

void TestTransferAppliesSettingsOncePerTransfer() {
  FakeSpiBus bus;
  assert(bus.Open("/dev/spidev0.0", 3, 4000000, true));
  // Nothing has been transferred yet, so nothing has been re-applied.
  assert(bus.settings_applications() == 0);

  bus.Expect({0x01}, {0xAA});
  bus.Expect({0x02}, {0xBB});
  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(bus.Transfer(tx, rx, 1));
  assert(bus.settings_applications() == 1);
  tx[0] = 0x02;
  assert(bus.Transfer(tx, rx, 1));
  assert(bus.settings_applications() == 2);

  // What gets re-applied is this opener's OWN settings, not whatever the
  // node last saw — that is the whole point of the fix.
  assert(bus.applied_mode() == 3);
  assert(bus.applied_speed_hz() == 4000000u);
  assert(bus.applied_no_cs() == true);
}

void TestTransferAppliesNothingWhenNotOpen() {
  // Fail-closed: the re-application step is also the open-gate, so a bus
  // that was never opened (or was closed) must neither apply nor exchange.
  FakeSpiBus bus;
  bus.Expect({0x01}, {0xAA});
  std::uint8_t tx[1] = {0x01};
  std::uint8_t rx[1] = {};
  assert(!bus.Transfer(tx, rx, 1));
  assert(bus.settings_applications() == 0);

  assert(bus.Open("/dev/spidev0.1", 1, 500000, false));
  assert(bus.Transfer(tx, rx, 1));
  assert(bus.settings_applications() == 1);
  assert(bus.applied_mode() == 1);
  assert(bus.applied_no_cs() == false);

  bus.Close();
  bus.Expect({0x01}, {0xAA});
  assert(!bus.Transfer(tx, rx, 1));
  assert(bus.settings_applications() == 1);  // unchanged by the closed call
}

// ---------------------------------------------------------------------
// C2(b): the lock is keyed per PHYSICAL CONTROLLER, not per device node.
// /dev/spidev0.0 and /dev/spidev0.1 are two chip-select views of one set
// of SCLK/MOSI/MISO wires; keying by string would give click 1 its own
// mutex and let it clock the bus mid-motor-datagram.
// ---------------------------------------------------------------------

void TestControllerKeyCollapsesChipSelect() {
  assert(SpiControllerKey("/dev/spidev0.0") == "spi0");
  assert(SpiControllerKey("/dev/spidev0.1") == "spi0");
  assert(SpiControllerKey("/dev/spidev1.0") == "spi1");
  assert(SpiControllerKey("/dev/spidev10.2") == "spi10");
  // Unrecognised transports serialise against themselves only, under a
  // prefix that cannot alias a canonical "spiN" key.
  assert(SpiControllerKey("fake-bus") == "raw:fake-bus");
  assert(SpiControllerKey("/dev/spidev0") == "raw:/dev/spidev0");
}

void TestBusMutexIsSharedAcrossChipSelectsOfOneController() {
  assert(&SpiBusMutex("/dev/spidev0.0") == &SpiBusMutex("/dev/spidev0.1"));
  // Different controller, different wires, different mutex — the other
  // direction of the same policy.
  assert(&SpiBusMutex("/dev/spidev0.0") != &SpiBusMutex("/dev/spidev1.0"));
}

void TestSpiBusLockCountsHoldsPerController() {
  const std::uint64_t spi0_before = SpiBusLockAcquireCount("/dev/spidev0.0");
  const std::uint64_t spi1_before = SpiBusLockAcquireCount("/dev/spidev1.0");

  { SpiBusLock hold("/dev/spidev0.0"); }
  { SpiBusLock hold("/dev/spidev0.1"); }  // same controller, same counter

  assert(SpiBusLockAcquireCount("/dev/spidev0.0") == spi0_before + 2);
  assert(SpiBusLockAcquireCount("/dev/spidev0.1") == spi0_before + 2);
  // A hold on a different controller must not show up on this one.
  assert(SpiBusLockAcquireCount("/dev/spidev1.0") == spi1_before);

  { SpiBusLock hold("/dev/spidev1.0"); }
  assert(SpiBusLockAcquireCount("/dev/spidev1.0") == spi1_before + 1);
  assert(SpiBusLockAcquireCount("/dev/spidev0.0") == spi0_before + 2);
}

void TestLinuxSpiBusReportsAvailabilityWithoutCrashing() {
  // On a non-Linux build host available() is false and Open() must fail
  // cleanly rather than trap.
  LinuxSpiBus bus;
  if (!bus.available()) {
    assert(!bus.Open("/dev/spidev0.0", 3, 1000000, false));
  }
  bus.Close();
}

}  // namespace

int main() {
  TestFakeRejectsTransferBeforeOpen();
  TestFakeRejectsTransferAfterClose();
  TestFakeRejectsTransferAfterFailedOpen();
  TestFakeRejectsNullPointers();
  TestFakeServesScriptedExchange();
  TestFakeDetectsTxContentMismatch();
  TestFakeDetectsLengthMismatch();
  TestFakeRejectsShortScriptedRx();
  TestFakeInjectsTransferFailures();
  TestFakeExhaustedQueueTransferFails();
  TestFakeRecordsOpenParameters();
  TestTransferAppliesSettingsOncePerTransfer();
  TestTransferAppliesNothingWhenNotOpen();
  TestControllerKeyCollapsesChipSelect();
  TestBusMutexIsSharedAcrossChipSelectsOfOneController();
  TestSpiBusLockCountsHoldsPerController();
  TestLinuxSpiBusReportsAvailabilityWithoutCrashing();
  return 0;
}
