// Rev B.1 refactor tests: floor-only thermal policy, simplified phase FSM,
// and duty vector sizing (6 heaters drive 6 of the 8 samples one-to-one;
// samples 6 and 7 are pulled but unheated).
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/state_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/thermal_controller.hpp"

namespace {

coatheal::OnboardConfig MakeConfig() {
  coatheal::OnboardConfig cfg;
  // Rev B.1 defaults should already be set; pin them here for readability.
  cfg.hardware.heater_count = 6;
  cfg.hardware.electronics_heater_index = static_cast<std::size_t>(-1);
  cfg.phase.sample_floor_c = 5.0;
  cfg.phase.uniformity_tolerance_c = 2.0;
  cfg.transition.ascent_to_float_mbar = 100.0;
  cfg.transition.float_to_descent_mbar = 300.0;
  cfg.transition.descent_to_landed_mbar = 800.0;
  // Healthy overtemp ceilings so nothing latches in these tests.
  cfg.heater_safety.max_sample_temp_c = 85.0;
  return cfg;
}

coatheal::SensorSnapshot SnapshotWithSamples(double sample_c,
                                             std::size_t sample_count) {
  coatheal::SensorSnapshot s;
  s.sample_temps_c.assign(sample_count, sample_c);
  return s;
}

void TestRevBDefaults() {
  coatheal::OnboardConfig cfg;
  assert(cfg.hardware.heater_count == 6);
  assert(cfg.hardware.electronics_heater_index == static_cast<std::size_t>(-1));
  assert(cfg.phase.sample_floor_c == 5.0);
  assert(cfg.transition.ascent_to_float_mbar == 100.0);
  assert(cfg.transition.float_to_descent_mbar == 300.0);
  assert(cfg.transition.descent_to_landed_mbar == 800.0);
  // Rev B.1 power defaults: 5 W @ 24 V, 20 W combined ceiling, 4 active.
  assert(cfg.power.heater_nominal_w == 5.0);
  assert(cfg.power.max_thermal_w == 20.0);
  assert(cfg.power.max_active_heaters == 4);
}

void TestDutyVectorSizeIsSix() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController tc(cfg);
  const auto snap = SnapshotWithSamples(-10.0, 8);
  coatheal::ControlOverrides ov;
  const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                            snap, 1.0, ov);
  assert(duty.size() == 6);
}

void TestFloorHysteresisCycle() {
  // Heater only turns on when sample < 4.5 C (= 5 - 0.5 hysteresis),
  // stays on until sample >= 5 C, then turns off and stays off.
  const auto cfg = MakeConfig();
  coatheal::ThermalController tc(cfg);
  coatheal::ControlOverrides ov;

  // Case A: at 4.6 C (above on_threshold but below floor) and not already
  // heating -> stays off (pure hysteresis guard).
  {
    const auto snap = SnapshotWithSamples(4.6, 8);
    const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                              snap, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] == 0.0);
    }
    assert(!tc.sample_heating()[0]);
  }

  // Case B: sample drops below on_threshold -> heater turns on, duty > 0.
  {
    const auto snap = SnapshotWithSamples(3.0, 8);
    const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                              snap, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] > 0.0);
    }
    assert(tc.sample_heating()[0]);
  }

  // Case C: still below floor (4.6 C) but already heating -> remains on.
  {
    const auto snap = SnapshotWithSamples(4.6, 8);
    const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                              snap, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] > 0.0);
    }
    assert(tc.sample_heating()[0]);
  }

  // Case D: reaches floor -> heater turns off and stays off at 4.7 C (within
  // hysteresis band above on_threshold).
  {
    const auto snap = SnapshotWithSamples(5.0, 8);
    const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                              snap, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] == 0.0);
    }
    assert(!tc.sample_heating()[0]);
  }
  {
    const auto snap = SnapshotWithSamples(4.7, 8);
    const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kAscent,
                                              snap, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] == 0.0);  // still off: above on_threshold (4.5)
    }
    assert(!tc.sample_heating()[0]);
  }
}

void TestNoHeatInBootLandedStopped() {
  const auto cfg = MakeConfig();
  coatheal::ThermalController tc(cfg);
  coatheal::ControlOverrides ov;
  const auto cold = SnapshotWithSamples(-30.0, 8);

  for (auto phase : {coatheal::MissionPhase::kBoot,
                     coatheal::MissionPhase::kLanded,
                     coatheal::MissionPhase::kStopped}) {
    const auto duty = tc.ComputeRequestedDuty(phase, cold, 1.0, ov);
    for (double d : duty) {
      assert(d == 0.0);
    }
  }
}

void TestAllFlyingPhasesShareFloor() {
  // ASCENT/FLOAT/DESCENT all demand heat when sample is below floor.
  const auto cfg = MakeConfig();
  coatheal::ThermalController tc(cfg);
  coatheal::ControlOverrides ov;
  const auto cold = SnapshotWithSamples(-5.0, 8);

  for (auto phase : {coatheal::MissionPhase::kAscent,
                     coatheal::MissionPhase::kFloat,
                     coatheal::MissionPhase::kDescent}) {
    tc.Reset();
    const auto duty = tc.ComputeRequestedDuty(phase, cold, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] > 0.0);
    }
  }
}

void TestPhaseTransitionsViaPressure() {
  auto cfg = MakeConfig();
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  // First Update leaves BOOT immediately.
  auto p = sm.Update(900.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // 150 mbar: still in ascent (above 100 mbar threshold).
  p = sm.Update(150.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // 100 mbar: reaches ascent->float threshold (<=).
  p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kFloat);

  // Still at float altitude — no timed expiry, stays in FLOAT.
  p = sm.Update(80.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kFloat);

  // Re-pressurises past 300 mbar -> DESCENT.
  p = sm.Update(300.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kDescent);

  // Surface pressure recovered -> LANDED.
  p = sm.Update(800.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kLanded);

  // LANDED is sticky.
  p = sm.Update(1013.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kLanded);
}

void TestShutdownSafeGoesToStopped() {
  auto cfg = MakeConfig();
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  coatheal::StateOverrides ov;
  ov.shutdown_safe = true;
  auto p = sm.Update(500.0, samples, ov, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kStopped);
}

}  // namespace

int main() {
  TestRevBDefaults();
  TestDutyVectorSizeIsSix();
  TestFloorHysteresisCycle();
  TestNoHeatInBootLandedStopped();
  TestAllFlyingPhasesShareFloor();
  TestPhaseTransitionsViaPressure();
  TestShutdownSafeGoesToStopped();
  std::cout << "Rev B.1 phase/thermal tests passed.\n";
  return 0;
}
