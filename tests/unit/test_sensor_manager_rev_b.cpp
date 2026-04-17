// Rev-B.1 SensorManager regression test.
//
// Added 2026-04-17 by Agent C after bench-mode telemetry showed samples 6
// and 7 emitting a 100.0 ohm value instead of the "-" placeholder. The bug
// was in the constructor's resistance-vector init (all 8 slots seeded with
// kInitialResistanceOhm), causing the serializer's `> 0.0` guard to pass.
// The fix zeroes slots 6 and 7 so they serialize as "-" and stay zero even
// after motor-1 pulls.

#include <cassert>
#include <cmath>

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
  config.hardware.heater_count = 6;
  return SensorManager(config, spi, i2c, rtc, ina);
}

void TestInitialResistanceVectorHasDashTailOnWire() {
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm = MakeSensorManager(&ina, &spi, &i2c, &rtc);

  const SensorSnapshot snap = sm.ReadSnapshot(MissionPhase::kAscent,
                                              std::vector<double>(6, 0.0),
                                              0.1);
  assert(snap.sample_resistance_ohm.size() == 8);
  // Samples 0..5 must be measured (> 0); samples 6 and 7 must be unmeasured
  // so the telemetry layer emits "-".
  for (std::size_t i = 0; i < 6; ++i) {
    assert(snap.sample_resistance_ohm[i] > 0.0);
  }
  assert(snap.sample_resistance_ohm[6] == 0.0);
  assert(snap.sample_resistance_ohm[7] == 0.0);

  TelemetryRecord rec;
  rec.seq = 0;
  rec.sensors = snap;
  rec.phase = MissionPhase::kAscent;
  rec.mode = SystemMode::kStandby;
  rec.heater_duty = std::vector<double>(6, 0.0);
  rec.steppers.resize(2);
  const std::string line = SerializeTelemetryDataFrame(rec, "sess-init");

  // Structural: the RESISTANCE= field must end with "|-|-" (samples 6 & 7).
  const auto pos = line.find("RESISTANCE=");
  assert(pos != std::string::npos);
  const auto end = line.find(',', pos);
  const std::string rest = line.substr(pos, end - pos);
  // Split by '|' — expect 8 tokens; last two must be "-".
  // (Header + 7 pipes.)
  std::size_t pipes = 0;
  for (char c : rest) if (c == '|') ++pipes;
  assert(pipes == 7);
  // Convenient suffix check:
  assert(rest.size() >= 4);
  assert(rest.substr(rest.size() - 4) == "|-|-");
}

void TestMotor1PullsLeaveSample6And7Dashed() {
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
  // Samples 4 and 5 should have decayed by ~5% per pull × 5 pulls.
  assert(snap.sample_resistance_ohm[4] > 0.0);
  assert(snap.sample_resistance_ohm[4] < 100.0);
  assert(snap.sample_resistance_ohm[5] > 0.0);
  assert(snap.sample_resistance_ohm[5] < 100.0);
  // Samples 6 and 7 have no INA3221 channel, so they remain zero no matter
  // how many pulls motor 1 executes.
  assert(snap.sample_resistance_ohm[6] == 0.0);
  assert(snap.sample_resistance_ohm[7] == 0.0);
}

}  // namespace

int main() {
  TestInitialResistanceVectorHasDashTailOnWire();
  TestMotor1PullsLeaveSample6And7Dashed();
  return 0;
}
