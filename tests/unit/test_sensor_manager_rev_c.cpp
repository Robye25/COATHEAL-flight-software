// Rev C SensorManager resistance compatibility coverage.

#include <cassert>
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

}  // namespace

int main() {
  TestDisabledResistanceSerializesAsDashes();
  TestDisabledResistanceIgnoresPullNotifications();
  return 0;
}
