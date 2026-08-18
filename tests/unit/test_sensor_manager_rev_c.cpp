// Rev C SensorManager resistance compatibility coverage.

#include <cassert>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/telemetry.hpp"

using namespace coatheal;

namespace {

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
  // Transitional (Task 6): the DAQ132M acquisition path no longer exists, so
  // its still-present wire slot must report DISABLED rather than be left at
  // the ComponentHealth default of DISCOVERING. Task 7 deletes the field.
  assert(snap.daq132m.state == ComponentState::kDisabled);
  // The Sequent card is polled unconditionally, so this slot now reports the
  // real card. On a host with no Linux I2C bus that is DISABLED; on Linux
  // with no card wired it is DISCOVERING then FAILED. What holds everywhere
  // is that a card which never answered is never reported OK.
  assert(snap.rtd_click.state != ComponentState::kOk);
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

}  // namespace

int main() {
  TestDisabledResistanceSerializesAsDashes();
  TestDisabledResistanceIgnoresPullNotifications();
  TestMissingSensorsReturnImmediatelyWithInvalidNanValues();
  TestPt100WrapperStillForwards();
  TestHeatedChannelPolicyIgnoresUnheatedSamples();
  TestHeatedChannelPolicyRejectsOutOfRangeMapping();
  TestHeatedChannelPolicyFailsClosedOnEmptyMapping();
  return 0;
}
