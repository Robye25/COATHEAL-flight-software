#include <cassert>
#include <memory>
#include <string>

#include "coatheal/config.hpp"
#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/stepper_controller.hpp"

using namespace coatheal;

namespace {

StepperConfig MakeCfg() {
  StepperConfig c;
  c.steps_per_rev = 200;
  c.microstep = 1;  // keep rotate math simple in tests
  c.default_step_hz = 1000.0;
  c.max_step_hz = 4000.0;
  c.max_position_steps = 100000;
  c.enable_on_boot = true;
  return c;
}

BendScheduleConfig MakeSchedule() {
  BendScheduleConfig s;
  s.float_steps = 500;
  s.float_hold_s = 10.0;
  s.descent_steps = 0;
  return s;
}

std::unique_ptr<StepperController> MakeCtl(StepperConfig cfg = MakeCfg(),
                                           BendScheduleConfig sch = MakeSchedule()) {
  auto drv = std::make_unique<SimulatedStepperDriver>();
  return std::make_unique<StepperController>(cfg, sch, std::move(drv));
}

void TestMoveStepsIssuesPulses() {
  auto c = MakeCtl();
  std::string err;
  assert(c->MoveSteps(100, &err));
  // 1000 Hz × 1 s = 1000 pulses available; we only need 100.
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 100);
  assert(!s.moving);
  assert(s.pulses_total >= 100);
}

void TestSpeedLimitsPulsesPerTick() {
  auto c = MakeCtl();
  c->SetSpeed(50.0, nullptr);  // 50 Hz
  c->MoveSteps(1000, nullptr);
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s = c->Snapshot();
  // 50 pulses/s — should be around 50 after 1 s, definitely < 1000.
  assert(s.position_steps > 40 && s.position_steps < 60);
  assert(s.moving);
}

void TestPhaseEntryAppliesBend() {
  auto c = MakeCtl();
  // Give enough time to reach the 500-step float bend.
  c->Tick(MissionPhase::kFloatHold, 1.0);
  auto s = c->Snapshot();
  assert(s.target_steps == 500);
  // 1000 Hz × 1 s ≥ 500 — should already be at target.
  assert(s.position_steps == 500);
  assert(!s.moving);
  assert(s.holding);
  assert(s.hold_remaining_s <= 10.0 && s.hold_remaining_s > 8.0);
}

void TestHoldCountdown() {
  auto c = MakeCtl();
  c->Tick(MissionPhase::kFloatHold, 1.0);
  const double before = c->Snapshot().hold_remaining_s;
  c->Tick(MissionPhase::kFloatHold, 2.0);
  const double after = c->Snapshot().hold_remaining_s;
  assert(after < before);
  assert(before - after >= 1.9);
}

void TestStopAbortsMotion() {
  auto c = MakeCtl();
  c->MoveSteps(10000, nullptr);
  c->Tick(MissionPhase::kAscentHold, 0.1);  // partial
  c->Stop();
  auto s1 = c->Snapshot();
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s2 = c->Snapshot();
  assert(s2.position_steps == s1.position_steps);
  assert(!s2.moving);
}

void TestRotateUsesStepsPerRev() {
  StepperConfig cfg = MakeCfg();
  cfg.steps_per_rev = 200;
  cfg.microstep = 1;
  auto c = MakeCtl(cfg);
  c->Rotate(2.0, nullptr);  // 2 full revs × 200 = 400 steps
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 400);
}

void TestHomeReturnsToZero() {
  auto c = MakeCtl();
  c->MoveSteps(250, nullptr);
  c->Tick(MissionPhase::kAscentHold, 1.0);
  c->Home(nullptr);
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 0);
}

void TestMaxPositionRejected() {
  StepperConfig cfg = MakeCfg();
  cfg.max_position_steps = 100;
  auto c = MakeCtl(cfg);
  std::string err;
  assert(!c->MoveSteps(500, &err));
  assert(!err.empty());
}

void TestDisabledDriverDoesNotPulse() {
  auto c = MakeCtl();
  c->SetEnabled(false);
  c->MoveSteps(100, nullptr);
  c->Tick(MissionPhase::kAscentHold, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 0);
  assert(s.pulses_total == 0);
}

}  // namespace

int main() {
  TestMoveStepsIssuesPulses();
  TestSpeedLimitsPulsesPerTick();
  TestPhaseEntryAppliesBend();
  TestHoldCountdown();
  TestStopAbortsMotion();
  TestRotateUsesStepsPerRev();
  TestHomeReturnsToZero();
  TestMaxPositionRejected();
  TestDisabledDriverDoesNotPulse();
  return 0;
}
