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
  config.sensors.daq132m_enabled = false;
  config.sensors.rtd_click_enabled = false;
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
  assert(snap.daq132m.state == ComponentState::kDisabled);
  assert(snap.rtd_click.state == ComponentState::kDisabled);
  sm.Stop();
}

void TestMax31865Pt100Conversion() {
  const double r0 = SensorManager::Max31865CodeToResistance(8192, 400.0);
  assert(std::fabs(r0 - 100.0) < 1e-9);

  double temp = 0.0;
  assert(SensorManager::Pt100TemperatureFromResistance(100.0, &temp));
  assert(std::fabs(temp) < 0.05);

  assert(SensorManager::Pt100TemperatureFromResistance(138.5055, &temp));
  assert(std::fabs(temp - 100.0) < 0.1);

  assert(SensorManager::Pt100TemperatureFromResistance(80.306, &temp));
  assert(std::fabs(temp - (-50.0)) < 0.2);

  assert(!SensorManager::Pt100TemperatureFromResistance(1000.0, &temp));
}

}  // namespace

int main() {
  TestDisabledResistanceSerializesAsDashes();
  TestDisabledResistanceIgnoresPullNotifications();
  TestMissingSensorsReturnImmediatelyWithInvalidNanValues();
  TestMax31865Pt100Conversion();
  return 0;
}
