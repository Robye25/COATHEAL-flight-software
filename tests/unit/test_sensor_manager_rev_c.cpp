// Rev C SensorManager resistance compatibility coverage.

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "fake_i2c_bus.hpp"

using namespace coatheal;

namespace {

// Mirrors BlankImage() in test_sequent_rtd_adapter.cpp: the minimum register
// image for Probe()/ReadAll() to succeed structurally against a
// default-configured SequentRtdAdapter (PT100, stack 0, channel_map 1..8).
std::vector<std::uint8_t> BlankRtdImage() {
  std::vector<std::uint8_t> image(140, 0);
  image[sequent_rtd::kCardType] = 7;   // hardware >= 5.0
  image[sequent_rtd::kRevMajor] = 1;
  image[sequent_rtd::kRevMinor] = 5;
  image[sequent_rtd::kRevHwMajor] = 7;
  image[sequent_rtd::kRevHwMinor] = 0;
  image[sequent_rtd::kPt1000] = 0;     // PT100, matches OnboardConfig default
  return image;
}

SensorManager MakeSensorManager(Ina3221Adapter* ina,
                                SpiAdapter* spi,
                                I2cAdapter* i2c,
                                RtcAdapter* rtc) {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.runtime.use_simulated_sensors = true;
  config.sensors.resistance_source = "disabled";
  return SensorManager(config, spi, i2c, rtc, ina);
}

void TestDisabledResistanceSerializesAsDashes() {
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm = MakeSensorManager(&ina, &spi, &i2c, &rtc);

  const SensorSnapshot snap = sm.ReadSnapshot(MissionPhase::kAscent,
                                              std::vector<double>(6, 0.0),
                                              0.1);
  assert(sm.resistance_ok());
  assert(snap.sample_resistance_ohm.size() == 8);
  for (double value : snap.sample_resistance_ohm) {
    assert(value == 0.0);
  }

  TelemetryRecord rec;
  rec.seq = 0;
  rec.sensors = snap;
  rec.phase = MissionPhase::kAscent;
  rec.mode = SystemMode::kStandby;
  rec.heater_duty = std::vector<double>(6, 0.0);
  rec.steppers.resize(2);
  const std::string line = SerializeTelemetryDataFrame(rec, "sess-init");

  const auto pos = line.find("RESISTANCE=");
  assert(pos != std::string::npos);
  const auto end = line.find(',', pos);
  const std::string rest = line.substr(pos, end - pos);
  assert(rest == "RESISTANCE=-|-|-|-|-|-|-|-");
}

void TestDisabledResistanceIgnoresPullNotifications() {
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm = MakeSensorManager(&ina, &spi, &i2c, &rtc);

  for (int k = 0; k < 5; ++k) {
    sm.NotePullCompleted(1);
  }
  const SensorSnapshot snap = sm.ReadSnapshot(MissionPhase::kFloat,
                                              std::vector<double>(6, 0.0),
                                              0.1);
  for (double value : snap.sample_resistance_ohm) {
    assert(value == 0.0);
  }
}

void TestMissingSensorsReturnImmediatelyWithInvalidNanValues() {
  OnboardConfig config;
  config.runtime.use_simulated_sensors = false;
  config.sensors.dps310_enabled = false;
  config.sensors.ads1115_enabled = false;
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina);
  sm.Start();
  const auto begin = std::chrono::steady_clock::now();
  const SensorSnapshot snap = sm.ReadSnapshot(
      MissionPhase::kAscent, std::vector<double>(6, 0.0), 1.0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin);
  assert(elapsed.count() < 50);
  assert(!snap.ambient_temp_valid);
  assert(!snap.ambient_pressure_valid);
  assert(!snap.uv_valid);
  assert(std::isnan(snap.ambient_temp_c));
  assert(std::isnan(snap.ambient_pressure_mbar));
  assert(std::isnan(snap.uv));
  assert(snap.sample_temps_c.size() == 8);
  assert(snap.sample_temp_valid.size() == 8);
  for (std::size_t i = 0; i < snap.sample_temps_c.size(); ++i) {
    assert(std::isnan(snap.sample_temps_c[i]));
    assert(!snap.sample_temp_valid[i]);
    assert(snap.sample_temp_age_ms[i] == -1);
  }
  assert(snap.dps310.state == ComponentState::kDisabled);
  assert(snap.ads1115.state == ComponentState::kDisabled);
  // The Sequent card is polled unconditionally, so this slot now reports the
  // real card. On a host with no Linux I2C bus that is DISABLED; on Linux
  // with no card wired it is DISCOVERING then FAILED. What holds everywhere
  // is that a card which never answered is never reported OK.
  assert(snap.sequent_rtd.state != ComponentState::kOk);
  // Smoke check only: every sample is invalid in this fixture, so this
  // assertion holds under both the old any_of policy and the new
  // heated-channels policy. The policy itself is covered by the
  // HeatedChannelsValid tests below, which were verified to fail when the
  // policy is mutated. Differentiating coverage at the ReadSnapshot level
  // needs an I2cBus injection seam that SensorManager does not yet have:
  // sample_cache_ entries only become valid inside SequentRtdLoop, off a
  // real card, and no public API can mark one valid from a test.
  assert(!sm.sample_temp_ok());
  sm.Stop();
}

// SensorManager::Pt100TemperatureFromResistance is now a thin forwarder onto
// the HAL's Pt100TemperatureFromOhms. These are the conversion assertions
// that used to live in TestMax31865Pt100Conversion; the MAX31865 raw-code
// half of that test went away with the RTD Click driver.
void TestPt100WrapperStillForwards() {
  double temp = 0.0;
  assert(SensorManager::Pt100TemperatureFromResistance(100.0, &temp));
  assert(std::fabs(temp) < 0.05);

  assert(SensorManager::Pt100TemperatureFromResistance(138.5055, &temp));
  assert(std::fabs(temp - 100.0) < 0.1);

  assert(SensorManager::Pt100TemperatureFromResistance(80.306, &temp));
  assert(std::fabs(temp - (-50.0)) < 0.2);

  // Out of PT100 range: the wrapper must propagate the rejection, not
  // silently succeed with a clamped value.
  assert(!SensorManager::Pt100TemperatureFromResistance(1000.0, &temp));
}

void TestHeatedChannelPolicyIgnoresUnheatedSamples() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.heaters.temperature_channels = {0, 1, 2, 3, 4, 5};

  // Samples 6 and 7 are pulled but unheated: their validity must not
  // affect sample_temp_ok_.
  std::vector<bool> valid(8, true);
  valid[6] = false;
  valid[7] = false;
  assert(SensorManager::HeatedChannelsValid(config, valid));

  valid[3] = false;  // a heated channel drops
  assert(!SensorManager::HeatedChannelsValid(config, valid));
}

void TestHeatedChannelPolicyRejectsOutOfRangeMapping() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.heaters.temperature_channels = {0, 1, 99};
  const std::vector<bool> valid(8, true);
  // A mapping past the end of the sample vector must fail closed.
  assert(!SensorManager::HeatedChannelsValid(config, valid));

  // Same policy seen from the other side: an in-range mapping against a
  // short validity vector is still a mapping past the end. This is the case
  // that distinguishes "fail closed" from "skip channels we cannot index",
  // which would otherwise report the thermal path healthy off two channels.
  const std::vector<bool> short_valid(2, true);
  config.heaters.temperature_channels = {0, 1, 2};
  assert(!SensorManager::HeatedChannelsValid(config, short_valid));

  // And the same helper does say yes once the vector actually covers the
  // mapping, so the assertions above are rejecting the mapping and not the
  // helper rejecting everything.
  const std::vector<bool> long_enough(3, true);
  assert(SensorManager::HeatedChannelsValid(config, long_enough));
}

void TestHeatedChannelPolicyFailsClosedOnEmptyMapping() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.heaters.temperature_channels.clear();
  const std::vector<bool> all_valid(8, true);
  // An empty mapping means we cannot know which channels gate heating.
  // Fail closed: never report the thermal path as safe on a config error.
  assert(!SensorManager::HeatedChannelsValid(config, all_valid));
}

// Shared setup for the two i2c_ok()/RTD-bus-health tests below. DPS310 and
// ADS1115 are disabled so the `!enabled || valid` terms in ReadSnapshot's
// i2c_ok_ formula both collapse to `true`, leaving i2c_ok() driven entirely
// by rtd_bus_ok_ — exactly the scenario the controller ruling calls out
// ("with DPS310 and ADS1115 disabled, I2C_OK reports healthy while the RTD
// card is dead"). sequent_rtd_poll_ms is set small so the bounded waits
// below stay short without racing the worker thread's first pass.
OnboardConfig MakeRtdBusTestConfig() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.runtime.use_simulated_sensors = false;
  config.sensors.dps310_enabled = false;
  config.sensors.ads1115_enabled = false;
  config.sensors.sequent_rtd_poll_ms = 5;
  return config;
}

// i2c_ok's RTD contribution is bus-level (Probe/ReadAll succeeding), not
// per-channel plausibility, so a card that never lets the bus open is the
// right fixture: it never has to touch resistance/temperature plausibility
// to prove the point.
void TestI2cOkStaysFailedOnUnreachableRtdBus() {
  const OnboardConfig config = MakeRtdBusTestConfig();

  // A dedicated FakeI2cBus per test, mutated only before Start() and never
  // touched again from the test thread — SequentRtdLoop is the only thread
  // that reads it afterwards, so there is no cross-thread data race to
  // reason about, and the outcome does not depend on precise timing.
  FakeI2cBus bad_bus;
  bad_bus.SetImage(BlankRtdImage());
  bad_bus.SetOpenFails(true);  // card present in config, unreachable on wire

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &bad_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool ever_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.i2c_ok()) {
      ever_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // A bus that never opens must never be reported OK. This is the half that
  // catches a regression where the rtd_bus_ok_ AND is dropped from i2c_ok_'s
  // formula (with dps/ads disabled that bug makes i2c_ok() true on the very
  // first ReadSnapshot() call, regardless of the RTD card).
  assert(!ever_ok);
}

void TestI2cOkRecoversOnHealthyRtdBus() {
  const OnboardConfig config = MakeRtdBusTestConfig();

  FakeI2cBus good_bus;
  good_bus.SetImage(BlankRtdImage());

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &good_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool became_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.i2c_ok()) {
      became_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // A card that answers cleanly must bring i2c_ok() up. This is the half
  // that catches rtd_bus_ok_ being wired dead (e.g. never set, or the AND
  // term inverted/stuck false) — that bug would time out here instead.
  assert(became_ok);
}

}  // namespace

int main() {
  TestDisabledResistanceSerializesAsDashes();
  TestDisabledResistanceIgnoresPullNotifications();
  TestMissingSensorsReturnImmediatelyWithInvalidNanValues();
  TestPt100WrapperStillForwards();
  TestHeatedChannelPolicyIgnoresUnheatedSamples();
  TestHeatedChannelPolicyRejectsOutOfRangeMapping();
  TestHeatedChannelPolicyFailsClosedOnEmptyMapping();
  TestI2cOkStaysFailedOnUnreachableRtdBus();
  TestI2cOkRecoversOnHealthyRtdBus();
  return 0;
}
