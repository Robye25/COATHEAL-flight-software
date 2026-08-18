// Contract tests for the SPI transport seam and its test double.

#include <cassert>
#include <cstdint>
#include <vector>

#include "coatheal/hal/spi_bus.hpp"
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
  TestLinuxSpiBusReportsAvailabilityWithoutCrashing();
  return 0;
}
