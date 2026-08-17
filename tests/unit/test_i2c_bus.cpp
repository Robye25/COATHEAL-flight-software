// Contract tests for the I2C transport seam and its test double.

#include <cassert>
#include <cstdint>
#include <vector>

#include "coatheal/hal/i2c_bus.hpp"
#include "fake_i2c_bus.hpp"

using namespace coatheal;

namespace {

std::vector<std::uint8_t> Ramp(std::size_t size) {
  std::vector<std::uint8_t> image(size);
  for (std::size_t i = 0; i < size; ++i) {
    image[i] = static_cast<std::uint8_t>(i);
  }
  return image;
}

void TestFakeServesRegisterWindow() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  assert(bus.Open(0x40));
  assert(bus.address() == 0x40);

  std::uint8_t buf[4] = {0, 0, 0, 0};
  assert(bus.ReadRegisters(59, buf, 4));
  assert(buf[0] == 59 && buf[1] == 60 && buf[2] == 61 && buf[3] == 62);
}

void TestFakeRejectsReadPastEndOfImage() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(40));
  // Must be open, or this would pass for the wrong reason (rejected as
  // "not open" rather than as "runs off the end of the image").
  assert(bus.Open(0x40));

  std::uint8_t buf[8] = {};
  assert(!bus.ReadRegisters(36, buf, 8));
}

void TestFakeHonoursMaxReadLength() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  bus.SetMaxReadLength(4);
  assert(bus.Open(0x40));

  std::uint8_t big[32] = {};
  assert(!bus.ReadRegisters(0, big, 32));

  std::uint8_t small[4] = {};
  assert(bus.ReadRegisters(0, small, 4));
}

void TestFakeInjectsReadFailures() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  bus.FailNextReads(2);
  assert(bus.Open(0x40));

  std::uint8_t buf[1] = {};
  assert(!bus.ReadRegisters(0, buf, 1));
  assert(!bus.ReadRegisters(0, buf, 1));
  assert(bus.ReadRegisters(0, buf, 1));
}

void TestFakeRejectsReadBeforeOpen() {
  // LinuxI2cBus returns false when fd_ < 0; the double must agree, or
  // driver code that forgets to Open() passes here and fails on hardware.
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));

  std::uint8_t buf[4] = {};
  assert(!bus.ReadRegisters(0, buf, 4));
}

void TestFakeRejectsReadAfterClose() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  assert(bus.Open(0x40));

  std::uint8_t buf[4] = {};
  assert(bus.ReadRegisters(0, buf, 4));

  bus.Close();
  assert(!bus.ReadRegisters(0, buf, 4));
}

void TestFakeRejectsReadAfterFailedOpen() {
  // A failed Open() must leave the bus just as unusable as one that was
  // never opened at all — mirrors LinuxI2cBus, where a failed ::open() or
  // ioctl() leaves fd_ at -1.
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  bus.SetOpenFails(true);
  assert(!bus.Open(0x40));

  std::uint8_t buf[4] = {};
  assert(!bus.ReadRegisters(0, buf, 4));
}

void TestLinuxBusReportsAvailabilityWithoutCrashing() {
  // On a non-Linux build host available() is false and Open() must fail
  // cleanly rather than trap.
  LinuxI2cBus bus;
  if (!bus.available()) {
    assert(!bus.Open(0x40));
  }
  bus.Close();
}

}  // namespace

int main() {
  TestFakeServesRegisterWindow();
  TestFakeRejectsReadPastEndOfImage();
  TestFakeHonoursMaxReadLength();
  TestFakeInjectsReadFailures();
  TestFakeRejectsReadBeforeOpen();
  TestFakeRejectsReadAfterClose();
  TestFakeRejectsReadAfterFailedOpen();
  TestLinuxBusReportsAvailabilityWithoutCrashing();
  return 0;
}
