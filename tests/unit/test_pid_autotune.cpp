// Relay PID auto-tuner coverage: convergence on a first-order plant,
// every abort path, and the loop-closing proof — the gains the tuner
// suggests must actually hold that same plant at the setpoint.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>

#include "coatheal/config.hpp"
#include "coatheal/pid_autotuner.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/thermal_controller.hpp"

using namespace coatheal;

namespace {

// The same first-order plant safety_test uses:
// dT/dt = kHeat·duty − kLoss·(T − ambient). Equilibrium duty at 40 °C
// is kLoss·20/kHeat = 0.5.
constexpr double kHeat = 2.0;
constexpr double kLoss = 0.05;
constexpr double kAmbient = 20.0;

struct Plant {
  double temp = kAmbient;
  void Step(double duty, double dt) {
    temp += dt * (kHeat * duty - kLoss * (temp - kAmbient));
  }
};

PidAutoTuner::Config TuneConfig() {
  PidAutoTuner::Config cfg;
  cfg.setpoint_c = 40.0;
  cfg.relay_duty = 0.6;
  cfg.hysteresis_c = 1.0;
  cfg.cycles = 4;
  cfg.timeout_s = 1200.0;
  cfg.abort_ceiling_c = 55.0;
  return cfg;
}

// Runs the relay to completion against the plant; returns the result.
PidAutoTuner::Result RunTune(PidAutoTuner* tuner, Plant* plant) {
  tuner->Start(TuneConfig(), 0.0);
  double now = 0.0;
  for (int i = 0; i < 3000 && tuner->active(); ++i) {
    const double duty = tuner->Tick(true, plant->temp, now);
    plant->Step(duty, 1.0);
    now += 1.0;
  }
  return tuner->result();
}

void TestRelayConvergesOnFirstOrderPlant() {
  PidAutoTuner tuner;
  Plant plant;
  const auto r = RunTune(&tuner, &plant);
  assert(tuner.state() == PidAutoTuner::State::kDone);
  assert(r.cycles_used == 4);
  // The limit cycle must at least span the hysteresis band.
  assert(r.amplitude_c >= 0.9);
  assert(r.amplitude_c < 5.0);
  assert(r.tu_s >= 5.0);
  assert(r.tu_s < 120.0);
  assert(r.ku > 0.0);
  // Both rule sets produce strictly positive parallel-form gains.
  assert(r.kp > 0.0 && r.ki > 0.0 && r.kd > 0.0);
  assert(r.zn_kp > r.kp);  // Z-N is the more aggressive rule
}

// The loop-closing proof: hand the tuner's Tyreus–Luyben gains to the real
// ThermalController and the same plant must settle at the setpoint with
// the duty resting at equilibrium (never chopping to zero).
void TestTunedGainsHoldThePlant() {
  PidAutoTuner tuner;
  Plant tune_plant;
  const auto r = RunTune(&tuner, &tune_plant);
  assert(tuner.state() == PidAutoTuner::State::kDone);

  OnboardConfig cfg;
  cfg.hardware.heater_count = 6;
  cfg.runtime.use_simulated_sensors = true;
  ThermalController ctrl(cfg);
  ControlOverrides ov;
  ov.floor_control_enabled = false;
  ov.temp_targets_c.resize(cfg.hardware.heater_count);
  ov.pid_overrides.resize(cfg.hardware.heater_count);
  ov.temp_targets_c[0] = 40.0;
  ov.pid_overrides[0] = PidGains{r.kp, r.ki, r.kd};

  Plant plant;  // fresh, from ambient
  SensorSnapshot snapshot;
  snapshot.sample_temps_c.assign(8, kAmbient);
  double min_duty_late = 1.0;
  for (int t = 0; t < 900; ++t) {
    snapshot.sample_temps_c[0] = plant.temp;
    const auto duty = ctrl.ComputeRequestedDuty(
        MissionPhase::kBoot, snapshot, 1.0, ov);
    plant.Step(duty[0], 1.0);
    if (t >= 700) {
      min_duty_late = std::min(min_duty_late, duty[0]);
      assert(std::fabs(plant.temp - 40.0) < 2.0);
    }
  }
  assert(min_duty_late > 0.1);  // holding at equilibrium, not chopping
}

void TestInvalidTemperatureFailsTheTune() {
  PidAutoTuner tuner;
  tuner.Start(TuneConfig(), 0.0);
  assert(tuner.Tick(true, 20.0, 1.0) > 0.0);
  tuner.Tick(false, 0.0, 2.0);
  assert(tuner.state() == PidAutoTuner::State::kFailed);
  assert(tuner.error().find("invalid") != std::string::npos);
  assert(tuner.Tick(true, 20.0, 3.0) == 0.0);  // stays off after failure
}

void TestCeilingFailsTheTune() {
  PidAutoTuner tuner;
  tuner.Start(TuneConfig(), 0.0);
  tuner.Tick(true, 56.0, 1.0);  // above the 55 C ceiling
  assert(tuner.state() == PidAutoTuner::State::kFailed);
  assert(tuner.error().find("ceiling") != std::string::npos);
}

void TestTimeoutFailsTheTune() {
  PidAutoTuner tuner;
  tuner.Start(TuneConfig(), 0.0);
  // Plant never reaches the band: time runs out.
  tuner.Tick(true, 25.0, 1201.0);
  assert(tuner.state() == PidAutoTuner::State::kFailed);
  assert(tuner.error().find("timed out") != std::string::npos);
}

void TestOperatorAbort() {
  PidAutoTuner tuner;
  tuner.Start(TuneConfig(), 0.0);
  assert(tuner.active());
  tuner.Abort("aborted by operator");
  assert(tuner.state() == PidAutoTuner::State::kFailed);
  assert(tuner.error() == "aborted by operator");
  assert(!tuner.active());
}

}  // namespace

int main() {
  TestRelayConvergesOnFirstOrderPlant();
  TestTunedGainsHoldThePlant();
  TestInvalidTemperatureFailsTheTune();
  TestCeilingFailsTheTune();
  TestTimeoutFailsTheTune();
  TestOperatorAbort();
  std::cout << "pid autotune tests passed" << std::endl;
  return 0;
}
