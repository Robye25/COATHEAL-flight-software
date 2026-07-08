// Rev C refactor tests: floor-only thermal policy, simplified phase FSM with
// PRE_FLOAT and pressure debounce, and duty vector sizing (6 heaters drive
// 6 of the 8 samples one-to-one; samples 6 and 7 are pulled but unheated).
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
  // Rev C defaults should already be set; pin them here for readability.
  cfg.hardware.heater_count = 6;
  cfg.hardware.electronics_heater_index = static_cast<std::size_t>(-1);
  cfg.phase.sample_floor_c = 5.0;
  cfg.phase.uniformity_tolerance_c = 2.0;
  cfg.transition.pre_float_mbar = 150.0;
  cfg.transition.ascent_to_float_mbar = 100.0;
  cfg.transition.float_to_descent_mbar = 300.0;
  cfg.transition.descent_to_landed_mbar = 800.0;
  cfg.transition.debounce_samples = 5;
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

void TestRevCDefaults() {
  coatheal::OnboardConfig cfg;
  assert(cfg.hardware.heater_count == 6);
  assert(cfg.hardware.electronics_heater_index == static_cast<std::size_t>(-1));
  assert(cfg.phase.sample_floor_c == 5.0);
  assert(cfg.transition.pre_float_mbar == 150.0);
  assert(cfg.transition.ascent_to_float_mbar == 100.0);
  assert(cfg.transition.float_to_descent_mbar == 300.0);
  assert(cfg.transition.descent_to_landed_mbar == 800.0);
  assert(cfg.transition.debounce_samples == 5);
  // Rev C power defaults: 5 W heaters, 20 W combined ceiling, 4 active.
  assert(cfg.power.heater_nominal_w == 5.0);
  assert(cfg.power.max_thermal_w == 20.0);
  assert(cfg.power.max_active_heaters == 4);
  // Rev C fatigue defaults.
  assert(cfg.fatigue.fatigue_cycles == 30);
  assert(cfg.fatigue.fatigue_travel_full_steps == 200);
  assert(cfg.fatigue.fatigue_pull_hold_s == 2.0);
  assert(cfg.fatigue.soak_hold_s == 900.0);
  assert(cfg.fatigue.soak_travel_full_steps == 200);
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
  // ASCENT/PRE_FLOAT/FLOAT/DESCENT all demand heat when sample is below floor.
  const auto cfg = MakeConfig();
  coatheal::ThermalController tc(cfg);
  coatheal::ControlOverrides ov;
  const auto cold = SnapshotWithSamples(-5.0, 8);

  for (auto phase : {coatheal::MissionPhase::kAscent,
                     coatheal::MissionPhase::kPreFloat,
                     coatheal::MissionPhase::kFloat,
                     coatheal::MissionPhase::kDescent}) {
    tc.Reset();
    const auto duty = tc.ComputeRequestedDuty(phase, cold, 1.0, ov);
    for (std::size_t i = 0; i < cfg.hardware.heater_count; ++i) {
      assert(duty[i] > 0.0);
    }
  }
}

void TestPhaseTransitionsWithDebounce() {
  // Rev C: transitions require `debounce_samples` consecutive qualifying
  // pressure readings. Single readings do NOT trigger transition.
  auto cfg = MakeConfig();
  cfg.transition.debounce_samples = 3;  // smaller for test speed
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  // First Update leaves BOOT immediately -> ASCENT.
  auto p = sm.Update(900.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // 1 tick at 100 mbar: still ASCENT (need 3 consecutive).
  p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // 2nd tick at 100 mbar: still ASCENT.
  p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // 3rd tick at 100 mbar: NOW transitions to PRE_FLOAT.
  p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // PRE_FLOAT doesn't transition to FLOAT on pressure alone.
  p = sm.Update(50.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // PRE_FLOAT -> FLOAT requires fatigue_complete signal.
  coatheal::StateOverrides ov;
  ov.fatigue_complete = true;
  p = sm.Update(50.0, samples, ov, t0);
  assert(p == coatheal::MissionPhase::kFloat);
}

void TestDebounceResetOnBounce() {
  // If pressure bounces above threshold mid-debounce, counter resets.
  auto cfg = MakeConfig();
  cfg.transition.debounce_samples = 3;
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  // Enter ASCENT.
  sm.Update(900.0, samples, {}, t0);

  // 2 ticks below threshold.
  sm.Update(100.0, samples, {}, t0);
  sm.Update(100.0, samples, {}, t0);

  // Bounce above threshold — counter should reset.
  auto p = sm.Update(200.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // Need 3 NEW consecutive ticks below threshold.
  sm.Update(100.0, samples, {}, t0);
  sm.Update(100.0, samples, {}, t0);
  p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);
}

void TestPreFloatAbortOnDescentPressure() {
  // If pressure rises rapidly during PRE_FLOAT (balloon burst), abort to DESCENT.
  auto cfg = MakeConfig();
  cfg.transition.debounce_samples = 2;
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  // Enter ASCENT, then PRE_FLOAT.
  sm.Update(900.0, samples, {}, t0);
  sm.Update(100.0, samples, {}, t0);
  auto p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // 1 tick at descent pressure — not enough.
  p = sm.Update(350.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // 2nd tick at descent pressure — abort to DESCENT.
  p = sm.Update(350.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kDescent);
}

void TestFullPhaseTransitionSequence() {
  // Complete flight sequence: BOOT -> ASCENT -> PRE_FLOAT -> FLOAT -> DESCENT -> LANDED.
  auto cfg = MakeConfig();
  cfg.transition.debounce_samples = 1;  // instant transitions for this test
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  // BOOT -> ASCENT
  auto p = sm.Update(900.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);

  // ASCENT -> PRE_FLOAT at 150 mbar
  p = sm.Update(150.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // Still PRE_FLOAT without fatigue_complete.
  p = sm.Update(80.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kPreFloat);

  // PRE_FLOAT -> FLOAT on fatigue_complete.
  coatheal::StateOverrides ov;
  ov.fatigue_complete = true;
  p = sm.Update(80.0, samples, ov, t0);
  assert(p == coatheal::MissionPhase::kFloat);

  // Still at float altitude — stays in FLOAT.
  p = sm.Update(60.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kFloat);

  // FLOAT -> DESCENT at 300 mbar.
  p = sm.Update(300.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kDescent);

  // DESCENT -> LANDED at 800 mbar.
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

void TestManualSetPhaseResetsDebounce() {
  auto cfg = MakeConfig();
  cfg.transition.debounce_samples = 3;
  coatheal::StateManager sm(cfg);
  const std::vector<double> samples(8, 5.0);
  const auto t0 = std::chrono::steady_clock::now();

  sm.Update(900.0, samples, {}, t0);
  sm.Update(100.0, samples, {}, t0);
  sm.Update(100.0, samples, {}, t0);

  sm.SetPhase(coatheal::MissionPhase::kAscent);
  auto p = sm.Update(100.0, samples, {}, t0);
  assert(p == coatheal::MissionPhase::kAscent);
}

}  // namespace

int main() {
  TestRevCDefaults();
  TestDutyVectorSizeIsSix();
  TestFloorHysteresisCycle();
  TestNoHeatInBootLandedStopped();
  TestAllFlyingPhasesShareFloor();
  TestPhaseTransitionsWithDebounce();
  TestDebounceResetOnBounce();
  TestPreFloatAbortOnDescentPressure();
  TestFullPhaseTransitionSequence();
  TestShutdownSafeGoesToStopped();
  TestManualSetPhaseResetsDebounce();
  std::cout << "Rev C phase/thermal tests passed.\n";
  return 0;
}
