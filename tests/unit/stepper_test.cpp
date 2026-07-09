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

std::unique_ptr<StepperController> MakeCtl(StepperConfig cfg = MakeCfg()) {
  auto drv = std::make_unique<SimulatedStepperDriver>();
  return std::make_unique<StepperController>(cfg, std::move(drv));
}

void TestMoveStepsIssuesPulses() {
  auto c = MakeCtl();
  std::string err;
  assert(c->MoveSteps(100, &err));
  // 1000 Hz × 1 s = 1000 pulses available; we only need 100.
  c->Tick(MissionPhase::kAscent, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 100);
  assert(!s.moving);
  assert(s.pulses_total >= 100);
}

void TestSpeedLimitsPulsesPerTick() {
  auto c = MakeCtl();
  c->SetSpeed(50.0, nullptr);  // 50 Hz
  c->MoveSteps(1000, nullptr);
  c->Tick(MissionPhase::kAscent, 1.0);
  auto s = c->Snapshot();
  // 50 pulses/s — should be around 50 after 1 s, definitely < 1000.
  assert(s.position_steps > 40 && s.position_steps < 60);
  assert(s.moving);
}

void TestPhaseEntryDoesNotStartMotion() {
  auto c = MakeCtl();
  // Phase changes alone must never create a target or emit a pulse.
  c->Tick(MissionPhase::kFloat, 1.0);
  auto s = c->Snapshot();
  assert(s.target_steps == 0);
  assert(s.position_steps == 0);
  assert(!s.moving);
  assert(!s.holding);
}

void TestStopAbortsMotion() {
  auto c = MakeCtl();
  c->MoveSteps(10000, nullptr);
  c->Tick(MissionPhase::kAscent, 0.1);  // partial
  c->Stop();
  auto s1 = c->Snapshot();
  c->Tick(MissionPhase::kAscent, 1.0);
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
  c->Tick(MissionPhase::kAscent, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 400);
}

void TestHomeReturnsToZero() {
  auto c = MakeCtl();
  c->MoveSteps(250, nullptr);
  c->Tick(MissionPhase::kAscent, 1.0);
  c->Home(nullptr);
  c->Tick(MissionPhase::kAscent, 1.0);
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
  c->Tick(MissionPhase::kAscent, 1.0);
  auto s = c->Snapshot();
  assert(s.position_steps == 0);
  assert(s.pulses_total == 0);
}

}  // namespace

int main() {
  TestMoveStepsIssuesPulses();
  TestSpeedLimitsPulsesPerTick();
  TestPhaseEntryDoesNotStartMotion();
  TestStopAbortsMotion();
  TestRotateUsesStepsPerRev();
  TestHomeReturnsToZero();
  TestMaxPositionRejected();
  TestDisabledDriverDoesNotPulse();
  return 0;
}
