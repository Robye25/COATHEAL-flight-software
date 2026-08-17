// Sequent Microsystems RTD HAT register-conversation tests.
// Runs with no hardware attached via FakeI2cBus.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "fake_i2c_bus.hpp"

using namespace coatheal;

namespace {

// Builds a 140-byte register image with sane defaults.
std::vector<std::uint8_t> BlankImage() {
  std::vector<std::uint8_t> image(140, 0);
  image[sequent_rtd::kCardType] = 7;   // hardware >= 5.0
  image[sequent_rtd::kRevMajor] = 1;
  image[sequent_rtd::kRevMinor] = 5;
  image[sequent_rtd::kRevHwMajor] = 7;
  image[sequent_rtd::kRevHwMinor] = 0;
  image[sequent_rtd::kPt1000] = 0;     // PT100
  return image;
}

void PutFloat(std::vector<std::uint8_t>* image, int offset, float value) {
  std::memcpy(image->data() + offset, &value, sizeof(float));
}

void TestRegisterOffsetsMatchVendorDerivation() {
  assert(sequent_rtd::kRtdVal1 == 0);
  assert(sequent_rtd::kDiagTemp == 32);
  assert(sequent_rtd::kDiag5V == 33);
  assert(sequent_rtd::kRevHwMajor == 55);
  assert(sequent_rtd::kRevMajor == 57);
  assert(sequent_rtd::kRtdRes1 == 59);
  assert(sequent_rtd::kRtdReinit == 91);
  assert(sequent_rtd::kCardType == 99);
  assert(sequent_rtd::kPt1000 == 133);
}

void TestStackAddressArithmetic() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  SequentRtdAdapter::Options options;
  options.stack = 3;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(bus.address() == 0x43);
}

void TestStackOutOfRangeRejected() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  for (int stack : {-1, 8, 99}) {
    SequentRtdAdapter::Options options;
    options.stack = stack;
    SequentRtdAdapter adapter(&bus, options);

    SequentRtdAdapter::Identity id;
    std::string error;
    assert(!adapter.Probe(&id, &error));
    assert(error.find("STACK") != std::string::npos);
  }
}

void TestProbeReadsIdentity() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.card_type == 7);
  assert(id.fw_major == 1);
  assert(id.fw_minor == 5);
  assert(id.hw_major == 7);
  assert(!id.pt1000);
}

void TestProbeFailsWhenCardAbsent() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.FailNextReads(1);  // revision read fails, as doBoardInit detects

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("NOT_DETECTED") != std::string::npos);
}

void TestProbeFailsWhenOpenFails() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.SetOpenFails(true);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("BUS_OPEN") != std::string::npos);
}

void TestProbeRejectsSensorTypeMismatch() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 1;  // card set to PT1000

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = false;  // but we wired PT100 probes
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("SENSOR_TYPE_MISMATCH") != std::string::npos);
}

void TestProbeAcceptsMatchingPt1000() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 1;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = true;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.pt1000);
}

void TestSensorTypeMasksLowNibble() {
  // Vendor reads sensor type as `0x0f & buff`; high bits are not ours.
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 0xF1;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = true;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.pt1000);
}

void TestProbeReportsUnverifiableSensorTypeOnOldHardware() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kCardType] = 0;  // hardware < 5.0

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("SENSOR_TYPE_UNVERIFIABLE") != std::string::npos);
}

}  // namespace

int main() {
  TestRegisterOffsetsMatchVendorDerivation();
  TestStackAddressArithmetic();
  TestStackOutOfRangeRejected();
  TestProbeReadsIdentity();
  TestProbeFailsWhenCardAbsent();
  TestProbeFailsWhenOpenFails();
  TestProbeRejectsSensorTypeMismatch();
  TestProbeAcceptsMatchingPt1000();
  TestSensorTypeMasksLowNibble();
  TestProbeReportsUnverifiableSensorTypeOnOldHardware();
  return 0;
}
