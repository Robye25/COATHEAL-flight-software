// Sequent Microsystems RTD HAT register-conversation tests.
// Runs with no hardware attached via FakeI2cBus.

#include <cassert>
#include <cmath>
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
  // 0xF0 masks to 0x00 -> PT100. Without the 0x0f mask the raw byte is
  // nonzero and would be misread as PT1000, so this value — unlike a value
  // with the low nibble set — actually discriminates masked from unmasked.
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 0xF0;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = false;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(!id.pt1000);
}

void TestSensorTypeIgnoresHighNibbleWhenPt1000() {
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

void TestProbeReopensAfterIoFailure() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.FailNextReads(1);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;

  assert(!adapter.Probe(&id, &error));
  const int opens_after_failure = bus.open_count();

  // An I/O failure must leave the adapter ready to re-open, not stuck
  // believing it still holds a good connection.
  assert(adapter.Probe(&id, &error));
  assert(bus.open_count() > opens_after_failure);
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

std::vector<std::uint8_t> ImageWithChannels(const float* temps,
                                            const float* resistances) {
  std::vector<std::uint8_t> image = BlankImage();
  for (int i = 0; i < sequent_rtd::kChannels; ++i) {
    PutFloat(&image, sequent_rtd::kRtdVal1 + 4 * i, temps[i]);
    PutFloat(&image, sequent_rtd::kRtdRes1 + 4 * i, resistances[i]);
  }
  return image;
}

void TestReadAllDecodesFloat32Channels() {
  const float temps[8] = {0.0f, 10.5f, -40.25f, 85.0f,
                          21.0f, 22.0f, 23.0f, 24.0f};
  const float res[8] = {100.0f, 104.1f, 84.27f, 132.8f,
                        108.2f, 108.6f, 109.0f, 109.4f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  for (int i = 0; i < 8; ++i) {
    assert(std::fabs(reading.temperature_c[i] - temps[i]) < 1e-4);
    assert(std::fabs(reading.resistance_ohm[i] - res[i]) < 1e-4);
  }
}

void TestChannelMapRemapsLogicalSamples() {
  // Card channel 8 is dead; remap logical sample 0 onto card channel 3.
  const float temps[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  const float res[8] = {101.f, 102.f, 103.f, 104.f, 105.f, 106.f, 107.f, 108.f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter::Options options;
  options.channel_map = {3, 2, 1, 4, 5, 6, 7, 8};
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  // Logical 0 <- card channel 3 (1-indexed) == temps[2].
  assert(std::fabs(reading.temperature_c[0] - 3.0) < 1e-4);
  assert(std::fabs(reading.temperature_c[1] - 2.0) < 1e-4);
  assert(std::fabs(reading.temperature_c[2] - 1.0) < 1e-4);
  assert(std::fabs(reading.resistance_ohm[0] - 103.0) < 1e-4);
}

void TestFallbackMatchesBurstResults() {
  const float temps[8] = {0.0f, 10.5f, -40.25f, 85.0f,
                          21.0f, 22.0f, 23.0f, 24.0f};
  const float res[8] = {100.0f, 104.1f, 84.27f, 132.8f,
                        108.2f, 108.6f, 109.0f, 109.4f};
  const std::vector<std::uint8_t> image = ImageWithChannels(temps, res);

  FakeI2cBus burst_bus;
  burst_bus.SetImage(image);
  SequentRtdAdapter burst(&burst_bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading burst_reading;
  std::string error;
  assert(burst.ReadAll(&burst_reading, &error));
  assert(burst.burst_mode());

  FakeI2cBus slow_bus;
  slow_bus.SetImage(image);
  slow_bus.SetMaxReadLength(4);  // firmware refuses long reads
  SequentRtdAdapter slow(&slow_bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading slow_reading;
  assert(slow.ReadAll(&slow_reading, &error));
  assert(!slow.burst_mode());

  for (int i = 0; i < 8; ++i) {
    assert(std::fabs(burst_reading.temperature_c[i] -
                     slow_reading.temperature_c[i]) < 1e-9);
    assert(std::fabs(burst_reading.resistance_ohm[i] -
                     slow_reading.resistance_ohm[i]) < 1e-9);
  }
}

void TestFallbackLatchesOnce() {
  const float temps[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const float res[8] = {101, 102, 103, 104, 105, 106, 107, 108};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));
  bus.SetMaxReadLength(4);  // firmware refuses long reads

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;

  assert(adapter.ReadAll(&reading, &error));
  assert(!adapter.burst_mode());

  // Second poll must not re-attempt the 32-byte burst. Without the latch
  // the adapter would retry a doomed long read on every poll for the whole
  // flight, so assert directly that no oversized read was attempted.
  bus.ClearReadLog();
  assert(adapter.ReadAll(&reading, &error));
  for (const std::size_t len : bus.read_lengths()) {
    assert(len <= 4);
  }
  assert(!bus.read_lengths().empty());
}

void TestReadAllDecodesDiagnostics() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kDiagTemp] = static_cast<std::uint8_t>(
      static_cast<std::int8_t>(-12));            // -12 degC
  image[sequent_rtd::kDiag5V] = 0x88;            // 5000 mV little-endian
  image[sequent_rtd::kDiag5V + 1] = 0x13;
  image[sequent_rtd::kRtdReinit] = 0x05;         // 5 re-inits
  image[sequent_rtd::kRtdReinit + 1] = 0x00;
  image[sequent_rtd::kRtdReinit + 2] = 0x00;
  image[sequent_rtd::kRtdReinit + 3] = 0x00;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  assert(std::fabs(reading.card_temp_c - (-12.0)) < 1e-9);
  assert(std::fabs(reading.rail_5v - 5.0) < 1e-6);
  assert(reading.adc_reinit_count == 5U);
}

void TestReadAllFailsWhenBusFails() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.FailNextReads(100);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(!adapter.ReadAll(&reading, &error));
  assert(!error.empty());
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
  TestSensorTypeIgnoresHighNibbleWhenPt1000();
  TestProbeReopensAfterIoFailure();
  TestProbeReportsUnverifiableSensorTypeOnOldHardware();
  TestReadAllDecodesFloat32Channels();
  TestChannelMapRemapsLogicalSamples();
  TestFallbackMatchesBurstResults();
  TestFallbackLatchesOnce();
  TestReadAllDecodesDiagnostics();
  TestReadAllFailsWhenBusFails();
  return 0;
}
