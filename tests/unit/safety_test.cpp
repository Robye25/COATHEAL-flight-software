// Safety regression tests: per-channel over-temperature cutoff latch,
// uniformity monitor during float hold, ambient-range flagging, and
// StorageManager SAFE-mode fsync durability.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/status_flags.hpp"
#include "coatheal/storage_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/thermal_controller.hpp"

namespace {

coatheal::OnboardConfig MakeConfig() {
  coatheal::OnboardConfig cfg;
  cfg.hardware.heater_count = 10;
  cfg.hardware.electronics_heater_index = 9;
  cfg.heater_safety.max_sample_temp_c = 85.0;
  cfg.heater_safety.max_box_temp_c = 60.0;
  cfg.phase.uniformity_tolerance_c = 2.0;
  cfg.phase.float_target_c = 70.0;
  return cfg;
}

coatheal::SensorSnapshot MakeSnapshot(std::size_t n, double value) {
  coatheal::SensorSnapshot s;
  s.sample_temps_c.assign(n, value);
  s.box_temp_c = 20.0;
  s.ambient_temp_c = -40.0;
  s.ambient_pressure_mbar = 500.0;
  return s;
}

void TestOvertempCutoffLatches() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;

  // Channel 3 is over the 85 C cutoff.
  coatheal::SensorSnapshot hot = MakeSnapshot(10, 50.0);
  hot.sample_temps_c[3] = 90.0;

  auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloatHold, hot, 1.0, ov);
  assert(duty[3] == 0.0);
  assert(ctrl.overtemp_latched());
  assert(ctrl.channel_latched()[3]);

  // Next tick, channel cools back down, but it must stay latched off.
  coatheal::SensorSnapshot cool = MakeSnapshot(10, 50.0);
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloatHold, cool, 1.0, ov);
  assert(duty[3] == 0.0);
  assert(ctrl.overtemp_latched());

  // Even a full-power override must not re-arm a latched channel.
  ov.all_heaters_override = 1.0;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloatHold, cool, 1.0, ov);
  assert(duty[3] == 0.0);
  ov.all_heaters_override.reset();

  // RESET_CONTROL (Reset()) clears the latch.
  ctrl.Reset();
  assert(!ctrl.overtemp_latched());
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloatHold, cool, 1.0, ov);
  assert(!ctrl.channel_latched()[3]);
}

void TestUniformityBit() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;

  // Within tolerance.
  coatheal::SensorSnapshot tight = MakeSnapshot(10, 70.0);
  tight.sample_temps_c[0] = 69.0;
  tight.sample_temps_c[1] = 70.5;
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kFloatHold, tight, 1.0, ov);
  assert(ctrl.uniformity_ok());

  // Spread > 2.0 C during float hold -> uniformity_ok == false.
  coatheal::SensorSnapshot spread = MakeSnapshot(10, 70.0);
  spread.sample_temps_c[0] = 68.0;
  spread.sample_temps_c[1] = 72.0;
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kFloatHold, spread, 1.0, ov);
  assert(!ctrl.uniformity_ok());

  // Outside float hold, uniformity bit should be OK regardless.
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kAscentHold, spread, 1.0, ov);
  assert(ctrl.uniformity_ok());
}

void TestAmbientRangeFlags() {
  auto cfg = MakeConfig();
  coatheal::SpiAdapter spi;
  coatheal::I2cAdapter i2c;
  coatheal::RtcAdapter rtc;
  coatheal::SensorManager sm(cfg, &spi, &i2c, &rtc);

  // Drive several ticks; the simulated model keeps ambient_temp at -40/-55 C
  // (in range) and pressure in [5, 1013.25] (in range).
  for (int i = 0; i < 5; ++i) {
    sm.ReadSnapshot(coatheal::MissionPhase::kFloatHold, {}, 1.0);
  }
  assert(sm.t_ambient_ok());
  assert(sm.p_ambient_ok());

  // Tighten the allowed bands so the simulated values fall outside; this
  // exercises the flag logic without touching the synthetic sensor model.
  cfg.sensor_range.ambient_temp_min_c = 100.0;
  cfg.sensor_range.ambient_temp_max_c = 200.0;
  cfg.sensor_range.ambient_pressure_min_mbar = 2000.0;
  cfg.sensor_range.ambient_pressure_max_mbar = 3000.0;
  coatheal::SensorManager sm2(cfg, &spi, &i2c, &rtc);
  sm2.ReadSnapshot(coatheal::MissionPhase::kFloatHold, {}, 1.0);
  assert(!sm2.t_ambient_ok());
  assert(!sm2.p_ambient_ok());
}

void TestStatusFlagsSerialize() {
  coatheal::StatusFlags flags;
  flags.overtemp_ok = false;
  flags.uniformity_ok = false;
  flags.t_ambient_ok = false;
  flags.p_ambient_ok = false;
  flags.energy_ok = false;
  const std::string s = coatheal::ToStatusBitfield(flags);
  assert(s.find("OVERTEMP_FAIL") != std::string::npos);
  assert(s.find("UNIFORMITY_FAIL") != std::string::npos);
  assert(s.find("T_AMBIENT_FAIL") != std::string::npos);
  assert(s.find("P_AMBIENT_FAIL") != std::string::npos);
  assert(s.find("ENERGY_FAIL") != std::string::npos);
  assert(s.find("SD_OK") != std::string::npos);
}

void TestStorageSafeModeWrites() {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "coatheal_safety_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  const std::string primary = (tmp / "primary.csv").string();
  const std::string secondary = (tmp / "secondary.csv").string();

  coatheal::StorageManager store(primary, secondary);
  std::string err;
  assert(store.Initialize(&err));
  store.SetSafeMode(true);
  assert(store.safe_mode());
  store.WriteLine("hello");
  store.WriteLine("world");
  store.FlushAndSync();

  std::ifstream in(primary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  assert(content.find("hello") != std::string::npos);
  assert(content.find("world") != std::string::npos);
  fs::remove_all(tmp);
}

}  // namespace

int main() {
  TestOvertempCutoffLatches();
  TestUniformityBit();
  TestAmbientRangeFlags();
  TestStatusFlagsSerialize();
  TestStorageSafeModeWrites();
  std::cout << "safety_test: OK\n";
  return 0;
}
