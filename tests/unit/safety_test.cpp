// Safety regression tests: per-channel over-temperature cutoff latch,
// uniformity monitor during FLOAT, ambient-range flagging, and
// StorageManager SAFE-mode fsync durability.
#include <algorithm>
#include <cassert>
#include <cmath>
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
#include "coatheal/pid_controller.hpp"
#include "coatheal/status_flags.hpp"
#include "coatheal/storage_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/thermal_controller.hpp"

namespace {

coatheal::OnboardConfig MakeConfig() {
  coatheal::OnboardConfig cfg;
  // Rev C: 6 sample heaters drive 6 of the 8 samples; no box heater.
  cfg.hardware.heater_count = 6;
  cfg.hardware.electronics_heater_index = static_cast<std::size_t>(-1);
  cfg.heater_safety.max_sample_temp_c = 85.0;
  cfg.phase.uniformity_tolerance_c = 2.0;
  // Rev C stores the fallback floor target in `sample_floor_c`.
  cfg.phase.sample_floor_c = 5.0;
  cfg.runtime.use_simulated_sensors = true;
  return cfg;
}

coatheal::SensorSnapshot MakeSnapshot(std::size_t n, double value) {
  coatheal::SensorSnapshot s;
  s.sample_temps_c.assign(n, value);
  s.ambient_temp_c = -40.0;
  s.ambient_pressure_mbar = 500.0;
  return s;
}

void TestOvertempCutoffLatches() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;

  // Channel 3 is over the 85 C cutoff. 8 samples are provided.
  coatheal::SensorSnapshot hot = MakeSnapshot(8, 50.0);
  hot.sample_temps_c[3] = 90.0;

  auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, hot, 1.0, ov);
  assert(duty[3] == 0.0);
  assert(ctrl.overtemp_latched());
  assert(ctrl.channel_latched()[3]);

  // Next tick, channel cools back down, but it must stay latched off.
  coatheal::SensorSnapshot cool = MakeSnapshot(8, 50.0);
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cool, 1.0, ov);
  assert(duty[3] == 0.0);
  assert(ctrl.overtemp_latched());

  // Even a full-power override must not re-arm a latched channel.
  ov.all_heaters_override = 1.0;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cool, 1.0, ov);
  assert(duty[3] == 0.0);
  ov.all_heaters_override.reset();

  // RESET_CONTROL (Reset()) clears the latch.
  ctrl.Reset();
  assert(!ctrl.overtemp_latched());
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cool, 1.0, ov);
  assert(!ctrl.channel_latched()[3]);
}

// heater.max_duty is the global power ceiling: it must bind the PID
// output, explicit duty overrides, and the bench open-loop path alike,
// while the over-temp latch keeps overriding it to zero. Added after the
// 2026-08-29 bench trip: a 5 W film at 100% duty ran its surface far
// ahead of the lagging PT100, so a 40 C target still crossed the 85 C
// latch — bounding delivered power is the fix, not lowering setpoints.
void TestMaxDutyCeilingBindsEveryPath() {
  auto cfg = MakeConfig();
  cfg.heaters.max_duty = 0.3;
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;

  // Closed-loop: 20 C error at kp=0.2 would ask for 1.0 — capped at 0.3.
  coatheal::SensorSnapshot cold = MakeSnapshot(8, 20.0);
  ov.temp_targets_c.assign(6, std::nullopt);
  ov.temp_targets_c[2] = 40.0;
  auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cold, 1.0, ov);
  assert(duty[2] > 0.0);
  assert(duty[2] <= 0.3 + 1e-9);

  // Explicit full-power override: still capped.
  ov.temp_targets_c[2].reset();
  ov.heater_duty_overrides.assign(6, std::nullopt);
  ov.heater_duty_overrides[1] = 1.0;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cold, 1.0, ov);
  assert(duty[1] == 0.3);

  // Bench open-loop (debug arm) path: capped too.
  ov.bench_open_loop_heaters = true;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, cold, 1.0, ov);
  assert(duty[1] == 0.3);
  ov.bench_open_loop_heaters = false;

  // The latch still wins over the ceiling: a tripped channel reads 0.
  coatheal::SensorSnapshot hot = MakeSnapshot(8, 20.0);
  hot.sample_temps_c[1] = 90.0;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, hot, 1.0, ov);
  assert(duty[1] == 0.0);
  assert(ctrl.channel_latched()[1]);
}

void TestUniformityBit() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;

  // Within tolerance. 8 sample channels in Rev C.
  coatheal::SensorSnapshot tight = MakeSnapshot(8, 5.0);
  tight.sample_temps_c[0] = 4.5;
  tight.sample_temps_c[1] = 5.5;
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kFloat, tight, 1.0, ov);
  assert(ctrl.uniformity_ok());

  // Spread > 2.0 C during FLOAT -> uniformity_ok == false.
  coatheal::SensorSnapshot spread = MakeSnapshot(8, 5.0);
  spread.sample_temps_c[0] = 3.0;
  spread.sample_temps_c[1] = 7.0;
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kFloat, spread, 1.0, ov);
  assert(!ctrl.uniformity_ok());

  // Outside any flying phase (BOOT), uniformity bit should be OK regardless.
  ctrl.ComputeRequestedDuty(coatheal::MissionPhase::kBoot, spread, 1.0, ov);
  assert(ctrl.uniformity_ok());
}

void TestInvalidSampleForcesHeaterOff() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;
  ov.floor_control_enabled = false;
  ov.heater_duty_overrides.resize(cfg.hardware.heater_count);
  ov.heater_duty_overrides[2] = 1.0;

  coatheal::SensorSnapshot snapshot = MakeSnapshot(8, 10.0);
  snapshot.sample_temp_valid.assign(8, true);
  snapshot.sample_temp_valid[2] = false;
  const auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, snapshot, 1.0, ov);
  assert(duty[2] == 0.0);

  ov.bench_open_loop_heaters = true;
  const auto bench_duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, snapshot, 1.0, ov);
  assert(bench_duty[2] == 1.0);
}

void TestManualTemperatureTargetAndPid() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;
  ov.floor_control_enabled = false;
  ov.temp_targets_c.resize(cfg.hardware.heater_count);
  ov.pid_overrides.resize(cfg.hardware.heater_count);
  ov.temp_targets_c[0] = 40.0;
  ov.pid_overrides[0] = coatheal::PidGains{0.1, 0.0, 0.0};

  const auto cold = MakeSnapshot(8, 20.0);
  auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kBoot, cold, 1.0, ov);
  assert(duty[0] > 0.0);
  for (std::size_t i = 1; i < duty.size(); ++i) assert(duty[i] == 0.0);

  auto hot = cold;
  hot.sample_temps_c[0] = 40.0;
  duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kBoot, hot, 1.0, ov);
  assert(duty[0] == 0.0);
}

// Owner report 2026-08-30: reaching the target snapped the duty to zero
// and wiped the PID, so the loop sawed between full reheat and coast-down
// forever. Closed-loop check on a first-order plant: the controller must
// SETTLE — temperature holding at the target with the duty resting at the
// plant's equilibrium (~0.5 here), never chopping to zero.
void TestTargetHoldsEquilibriumDutyWithoutChopping() {
  const auto cfg = MakeConfig();  // default gains 0.2/0.02/0.03, max_duty 1.0
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;
  ov.floor_control_enabled = false;
  ov.temp_targets_c.resize(cfg.hardware.heater_count);
  ov.pid_overrides.resize(cfg.hardware.heater_count);
  ov.temp_targets_c[0] = 40.0;

  // Plant: dT/dt = k_heat·duty − k_loss·(T − T_amb). Equilibrium duty at
  // 40 °C: k_loss·20/k_heat = 0.5.
  constexpr double kHeat = 2.0;   // °C/s at full duty
  constexpr double kLoss = 0.05;  // 1/s
  constexpr double kAmbient = 20.0;
  double temp = kAmbient;
  double min_duty_late = 1.0;
  double max_temp = temp;
  auto snapshot = MakeSnapshot(8, kAmbient);
  for (int t = 0; t < 600; ++t) {
    snapshot.sample_temps_c[0] = temp;
    const auto duty = ctrl.ComputeRequestedDuty(
        coatheal::MissionPhase::kBoot, snapshot, 1.0, ov);
    temp += kHeat * duty[0] - kLoss * (temp - kAmbient);
    max_temp = std::max(max_temp, temp);
    if (t >= 500) {
      min_duty_late = std::min(min_duty_late, duty[0]);
      assert(std::fabs(temp - 40.0) < 1.0);  // holding the target
    }
  }
  // The regression: the old cut-off forced duty to 0 on every target
  // crossing, so the late-window minimum was 0. A settled loop rests at
  // the equilibrium duty instead.
  assert(min_duty_late > 0.2);
  // Conditional anti-windup: the saturated ramp must not bank an integral
  // that discharges as a large overshoot.
  assert(max_temp < 44.0);
}

// The integrator must (a) not wind up while the output is saturated and
// (b) hold the steady-state output at zero error — (b) is what the old
// reset-at-target threw away.
void TestPidConditionalAntiWindupAndHold() {
  coatheal::PidController pid({0.0, 1.0, 0.0}, 0.0, 1.0, -1000.0, 1000.0);
  // 50 s hard against the ceiling: the integral must stay frozen...
  for (int i = 0; i < 50; ++i) {
    assert(pid.Update(10.0, 0.0, 1.0) <= 1.0 + 1e-9);
  }
  // ...so the moment the error vanishes, the output lets go at once
  // (an unconditional integrator would hold the ceiling for ~1000 s).
  assert(pid.Update(0.0, 0.0, 1.0) < 0.1);

  // Hold: integrate a small error inside the linear band, then sit at
  // zero error — the built-up integral keeps the output where it was.
  pid.Reset();
  for (int i = 0; i < 3; ++i) {
    pid.Update(0.2, 0.0, 1.0);
  }
  const double held = pid.Update(0.0, 0.0, 1.0);
  assert(held > 0.4);  // ki=1: three 0.2 error-seconds banked
}

void TestPerChannelPidOverridesGlobalPid() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController ctrl(cfg);
  coatheal::ControlOverrides ov;
  ov.floor_control_enabled = false;
  ov.temp_targets_c.resize(cfg.hardware.heater_count);
  ov.pid_overrides.resize(cfg.hardware.heater_count);
  ov.temp_targets_c[0] = 21.0;
  ov.temp_targets_c[1] = 21.0;
  ov.pid_override = coatheal::PidGains{0.1, 0.0, 0.0};
  ov.pid_overrides[0] = coatheal::PidGains{0.2, 0.0, 0.0};

  const auto snapshot = MakeSnapshot(8, 20.0);
  const auto duty = ctrl.ComputeRequestedDuty(
      coatheal::MissionPhase::kFloat, snapshot, 1.0, ov);
  assert(std::fabs(duty[0] - 0.2) < 1e-9);
  assert(std::fabs(duty[1] - 0.1) < 1e-9);
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
    sm.ReadSnapshot(coatheal::MissionPhase::kFloat, {}, 1.0);
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
  sm2.ReadSnapshot(coatheal::MissionPhase::kFloat, {}, 1.0);
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

  std::string content;
  {
    // Scoped so the ifstream's handle on primary.csv is released before
    // remove_all below — Windows refuses to unlink an open file (Linux
    // silently allows it, which is why this only surfaced here).
    std::ifstream in(primary);
    content.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
  }
  assert(content.find("hello") != std::string::npos);
  assert(content.find("world") != std::string::npos);
  fs::remove_all(tmp);
}

}  // namespace

int main() {
  TestOvertempCutoffLatches();
  TestMaxDutyCeilingBindsEveryPath();
  TestUniformityBit();
  TestInvalidSampleForcesHeaterOff();
  TestManualTemperatureTargetAndPid();
  TestTargetHoldsEquilibriumDutyWithoutChopping();
  TestPidConditionalAntiWindupAndHold();
  TestPerChannelPidOverridesGlobalPid();
  TestAmbientRangeFlags();
  TestStatusFlagsSerialize();
  TestStorageSafeModeWrites();
  std::cout << "safety_test: OK\n";
  return 0;
}
