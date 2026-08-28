// Link-loss failsafe plan (redesign spec §10, owner decisions D3–D6).
//
// The planner is pure, so every timeline below is a scripted sequence of
// FallbackTickInput values with a fake steady_clock -- no hardware, no
// SystemController. Each test states the rule it pins.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "coatheal/fallback_planner.hpp"

using namespace coatheal;

namespace {

using Clock = std::chrono::steady_clock;

FallbackPlannerConfig Cfg(double deadline_s = 1800.0) {
  FallbackPlannerConfig cfg;
  cfg.bend_min_c = -40.0;
  cfg.bend_max_c = 40.0;
  cfg.bend_deadline_s = deadline_s;
  return cfg;
}

FallbackMotorInput Ready(double temp_c = 5.0, bool moving = false) {
  FallbackMotorInput m;
  m.enabled = true;
  m.zeroed = true;
  m.healthy = true;
  m.moving_or_holding = moving;
  m.group_temp_c = temp_c;
  return m;
}

FallbackMotorInput NotReady(double temp_c = 5.0) {
  FallbackMotorInput m = Ready(temp_c);
  m.enabled = false;
  m.zeroed = false;
  return m;
}

FallbackTickInput In(bool fallback, MissionPhase phase, Clock::time_point now,
                     FallbackMotorInput m0, FallbackMotorInput m1) {
  FallbackTickInput in;
  in.fallback_active = fallback;
  in.phase = phase;
  in.now = now;
  in.motors = {m0, m1};
  return in;
}

FallbackPlanner ArmedPlanner(double deadline_s = 1800.0) {
  FallbackPlanner planner(Cfg(deadline_s), 2);
  std::string error;
  assert(planner.SetMotorPlan(0, 800, 5.0, 50.0, &error));
  assert(planner.SetMotorPlan(1, 600, 3.0, 0.0, &error));
  assert(planner.Arm(&error));
  assert(planner.state() == FallbackPlanState::kArmed);
  return planner;
}

// (a) Nothing happens while fallback is inactive or the phase is wrong,
// even with an armed plan and motors that are ready inside the window.
void TestNoActionOutsideFallbackOrWindowPhase() {
  FallbackPlanner planner = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  // Ready, in-window, but the link is up.
  assert(!planner.Tick(In(false, MissionPhase::kPreFloat, t0, Ready(), Ready())).has_value());
  // Fallback active but ASCENT: the bend belongs to PRE_FLOAT/FLOAT only.
  assert(!planner.Tick(In(true, MissionPhase::kAscent, t0, Ready(), Ready())).has_value());
  assert(!planner.Tick(In(true, MissionPhase::kDescent, t0, Ready(), Ready())).has_value());
  assert(!planner.deadline_started());
  assert(planner.state() == FallbackPlanState::kArmed);
  assert(planner.motor(0).state == FallbackMotorState::kPending);
}

// (b) M0 starts at PRE_FLOAT when ready and inside the window; M1 waits
// until M0 has finished, then starts with its own parameters.
void TestMotorsRunInOrderAtPreFloat() {
  FallbackPlanner planner = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  const std::optional<FallbackAction> a0 =
      planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(), Ready()));
  assert(a0.has_value());
  assert(a0->motor_id == 0);
  assert(a0->target_usteps == 800);
  assert(a0->hold_s == 5.0);
  assert(a0->speed_hz == 50.0);
  assert(planner.state() == FallbackPlanState::kRunning);
  assert(planner.motor(0).state == FallbackMotorState::kRunning);
  assert(planner.deadline_started());
  planner.ReportStartResult(0, true, false, "");

  // While M0 moves nothing else starts, M1 included.
  assert(!planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(1),
                          Ready(5.0, true), Ready())).has_value());
  assert(planner.motor(1).state == FallbackMotorState::kPending);

  // M0 stops moving -> done; the same tick may start M1.
  const std::optional<FallbackAction> a1 =
      planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(20),
                      Ready(5.0, false), Ready()));
  assert(planner.motor(0).state == FallbackMotorState::kDone);
  assert(a1.has_value());
  assert(a1->motor_id == 1);
  assert(a1->target_usteps == 600);
  assert(a1->speed_hz == 0.0);
  planner.ReportStartResult(1, true, false, "");
  assert(planner.state() == FallbackPlanState::kRunning);

  planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(21), Ready(), Ready(5.0, true)));
  assert(planner.motor(1).state == FallbackMotorState::kRunning);
  // The link comes back while M1 is still moving: the bend finishes and the
  // plan settles to done even though fallback is no longer active.
  planner.Tick(In(false, MissionPhase::kFloat, t0 + std::chrono::seconds(30), Ready(), Ready(5.0, true)));
  assert(planner.motor(1).state == FallbackMotorState::kRunning);
  planner.Tick(In(false, MissionPhase::kFloat, t0 + std::chrono::seconds(40), Ready(), Ready(5.0, false)));
  assert(planner.motor(1).state == FallbackMotorState::kDone);
  assert(planner.state() == FallbackPlanState::kDone);
  assert(!planner.armed());
}

// (c) Outside the temperature window the motor waits; once the deadline
// (counted from the first fallback tick at PRE_FLOAT) has passed it bends
// regardless of temperature.
void TestWindowNotMetWaitsUntilDeadline() {
  FallbackPlanner planner = ArmedPlanner(/*deadline_s=*/60.0);
  const Clock::time_point t0 = Clock::now();
  // -55 C is below bend_min_c; no valid temperature at all also counts as
  // "window not met".
  assert(!planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(-55.0), Ready(-55.0))).has_value());
  assert(planner.deadline_started());
  FallbackMotorInput blind = Ready();
  blind.group_temp_c.reset();
  assert(!planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(30), blind, Ready(-55.0))).has_value());
  assert(!planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(59), Ready(-55.0), Ready(-55.0))).has_value());
  assert(planner.motor(0).state == FallbackMotorState::kPending);
  // Deadline passed: the bend goes ahead at -55 C.
  const std::optional<FallbackAction> a0 =
      planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(60), Ready(-55.0), Ready(-55.0)));
  assert(a0.has_value());
  assert(a0->motor_id == 0);
}

// The window is inclusive at both ends and a temperature just outside it
// does not count.
void TestWindowBoundsAreInclusive() {
  FallbackPlanner planner = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  assert(!planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(40.01), Ready())).has_value());
  assert(planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(40.0), Ready())).has_value());
  FallbackPlanner low = ArmedPlanner();
  assert(!low.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(-40.01), Ready())).has_value());
  assert(low.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(-40.0), Ready())).has_value());
}

// (d) A motor that is still not enabled/zeroed when the deadline passes is
// skipped; the other motor runs and the plan still reaches done.
void TestNotReadyAtDeadlineIsSkipped() {
  FallbackPlanner planner = ArmedPlanner(/*deadline_s=*/10.0);
  const Clock::time_point t0 = Clock::now();
  // M0 not ready blocks M1 (strict id order) while the deadline has not
  // passed, even though M1 is ready and in-window.
  assert(!planner.Tick(In(true, MissionPhase::kPreFloat, t0, NotReady(), Ready())).has_value());
  assert(planner.motor(0).state == FallbackMotorState::kPending);
  assert(planner.motor(1).state == FallbackMotorState::kPending);
  const std::optional<FallbackAction> a =
      planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(10), NotReady(), Ready()));
  assert(planner.motor(0).state == FallbackMotorState::kSkipped);
  assert(a.has_value());
  assert(a->motor_id == 1);
  planner.ReportStartResult(1, true, false, "");
  planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(11), NotReady(), Ready(5.0, true)));
  planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(30), NotReady(), Ready(5.0, false)));
  assert(planner.motor(1).state == FallbackMotorState::kDone);
  assert(planner.state() == FallbackPlanState::kDone);
}

// (e) A refused start fails the motor and the whole plan; M1 is never
// attempted. A transient refusal (motion lock held) is retried instead.
void TestRefusedStartFailsPlan() {
  FallbackPlanner planner = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  assert(planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(), Ready())).has_value());
  planner.ReportStartResult(0, false, false, "channel disabled");
  assert(planner.state() == FallbackPlanState::kFailed);
  assert(planner.motor(0).state == FallbackMotorState::kFailed);
  assert(planner.last_error() == "channel disabled");
  for (int s = 1; s < 5; ++s) {
    assert(!planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(s), Ready(), Ready())).has_value());
  }
  assert(planner.motor(1).state == FallbackMotorState::kPending);
  // Arming a failed plan is refused until it is disarmed.
  std::string error;
  assert(!planner.Arm(&error));
  assert(error.find("failed") != std::string::npos);

  FallbackPlanner retry = ArmedPlanner();
  assert(retry.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(), Ready())).has_value());
  retry.ReportStartResult(0, false, true, "motion lock held by another motor");
  assert(retry.state() == FallbackPlanState::kArmed);
  assert(retry.motor(0).state == FallbackMotorState::kPending);
  const std::optional<FallbackAction> again =
      retry.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(1), Ready(), Ready()));
  assert(again.has_value());
  assert(again->motor_id == 0);
}

// (f) A finished plan never re-runs, however fallback toggles afterwards,
// and (g) DISARM resets it to none (motor plans kept, ready to re-arm).
void TestDonePlanNeverRerunsAndDisarmResets() {
  FallbackPlanner planner = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  assert(planner.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(), Ready())).has_value());
  planner.ReportStartResult(0, true, false, "");
  assert(planner.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(9), Ready(5.0, false), Ready())).has_value());
  planner.ReportStartResult(1, true, false, "");
  planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(20), Ready(), Ready(5.0, false)));
  assert(planner.state() == FallbackPlanState::kDone);

  // Link back, link lost again, still nothing.
  planner.Tick(In(false, MissionPhase::kFloat, t0 + std::chrono::seconds(30), Ready(), Ready()));
  for (int s = 40; s < 45; ++s) {
    assert(!planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(s), Ready(), Ready())).has_value());
  }
  assert(planner.state() == FallbackPlanState::kDone);

  planner.Disarm();
  assert(planner.state() == FallbackPlanState::kNone);
  assert(!planner.armed());
  assert(planner.motor(0).configured);
  assert(planner.motor(0).state == FallbackMotorState::kPending);
  assert(!planner.deadline_started());
  assert(!planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(50), Ready(), Ready())).has_value());
}

// Operator-surface rules: no plan -> cannot arm; loading while running ->
// refused; loading after done -> a fresh plan that must be armed again.
void TestOperatorSurfaceRules() {
  FallbackPlanner planner(Cfg(), 2);
  std::string error;
  assert(!planner.Arm(&error));
  assert(error == "no plan loaded");
  assert(!planner.SetMotorPlan(2, 1, 1.0, 1.0, &error));
  assert(error == "invalid motor id");
  assert(planner.SetMotorPlan(1, 600, 3.0, 25.0, &error));
  assert(planner.Arm(&error));
  const Clock::time_point t0 = Clock::now();
  assert(planner.Tick(In(true, MissionPhase::kFloat, t0, Ready(), Ready())).has_value());
  planner.ReportStartResult(1, true, false, "");
  assert(planner.state() == FallbackPlanState::kRunning);
  assert(!planner.SetMotorPlan(0, 800, 5.0, 50.0, &error));
  assert(error == "plan running");
  assert(!planner.Arm(&error));
  assert(error == "plan running");
  planner.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(30), Ready(), Ready(5.0, false)));
  assert(planner.state() == FallbackPlanState::kDone);
  assert(planner.SetMotorPlan(0, 800, 5.0, 50.0, &error));
  assert(planner.state() == FallbackPlanState::kNone);
  assert(planner.motor(1).state == FallbackMotorState::kPending);
  assert(planner.Arm(&error));
}

// (h) Persistence: an armed plan round-trips through the text file, a done
// plan comes back done (and stays inert), and a corrupt file yields no plan.
void TestPersistenceRoundTrip() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "coatheal_fallback_planner_test";
  std::filesystem::remove_all(dir);
  const std::string path = (dir / "fallback_plan.txt").string();

  FallbackPlanner planner = ArmedPlanner();
  assert(planner.TakeDirty());
  assert(!planner.TakeDirty());
  assert(planner.SaveTo(path));

  FallbackPlanner loaded(Cfg(), 2);
  assert(loaded.LoadFrom(path));
  assert(loaded.state() == FallbackPlanState::kArmed);
  assert(loaded.armed());
  assert(loaded.motor(0).configured);
  assert(loaded.motor(0).target_usteps == 800);
  assert(loaded.motor(0).hold_s == 5.0);
  assert(loaded.motor(0).speed_hz == 50.0);
  assert(loaded.motor(0).state == FallbackMotorState::kPending);
  assert(loaded.motor(1).target_usteps == 600);
  assert(loaded.StatusBody() ==
         "state=armed;armed=1;deadline_s=1800;deadline_started=0;"
         "m0=800/5/50/pending;m1=600/3/0/pending");
  // The restored plan is live: it runs on the next window tick.
  assert(loaded.Tick(In(true, MissionPhase::kPreFloat, Clock::now(), Ready(), Ready())).has_value());

  // A done plan stays done across the file.
  FallbackPlanner done = ArmedPlanner();
  const Clock::time_point t0 = Clock::now();
  assert(done.Tick(In(true, MissionPhase::kPreFloat, t0, Ready(), Ready())).has_value());
  done.ReportStartResult(0, true, false, "");
  assert(done.Tick(In(true, MissionPhase::kPreFloat, t0 + std::chrono::seconds(9), Ready(5.0, false), Ready())).has_value());
  done.ReportStartResult(1, true, false, "");
  done.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(20), Ready(), Ready(5.0, false)));
  assert(done.state() == FallbackPlanState::kDone);
  assert(done.SaveTo(path));
  FallbackPlanner reloaded(Cfg(), 2);
  assert(reloaded.LoadFrom(path));
  assert(reloaded.state() == FallbackPlanState::kDone);
  assert(reloaded.motor(0).state == FallbackMotorState::kDone);
  assert(!reloaded.Tick(In(true, MissionPhase::kFloat, t0 + std::chrono::seconds(30), Ready(), Ready())).has_value());

  // Corrupt file: no plan, and the planner is inert.
  {
    std::ofstream out(path, std::ios::trunc);
    out << "state=armed\nm0=800,5,fifty,pending\n";
  }
  FallbackPlanner corrupt(Cfg(), 2);
  assert(!corrupt.LoadFrom(path));
  assert(corrupt.state() == FallbackPlanState::kNone);
  assert(!corrupt.motor(0).configured);
  assert(!corrupt.Tick(In(true, MissionPhase::kFloat, t0, Ready(), Ready())).has_value());

  // Armed without any motor line is corrupt too.
  {
    std::ofstream out(path, std::ios::trunc);
    out << "armed=1\nstate=armed\n";
  }
  FallbackPlanner empty(Cfg(), 2);
  assert(!empty.LoadFrom(path));
  assert(empty.state() == FallbackPlanState::kNone);

  // Missing file: no plan, returns false, nothing thrown.
  std::filesystem::remove_all(dir);
  FallbackPlanner missing(Cfg(), 2);
  assert(!missing.LoadFrom(path));
  assert(missing.state() == FallbackPlanState::kNone);
}

// The serialized form is the documented plain key=value text.
void TestSerializedShape() {
  FallbackPlanner planner(Cfg(), 2);
  std::string error;
  assert(planner.SetMotorPlan(0, 800, 5.0, 50.0, &error));
  assert(planner.Serialize() == "armed=0\nstate=none\ndeadline_s=1800\nm0=800,5,50,pending\n");
  assert(planner.StatusBody() ==
         "state=none;armed=0;deadline_s=1800;deadline_started=0;m0=800/5/50/pending;m1=-");
}

}  // namespace

int main() {
  TestNoActionOutsideFallbackOrWindowPhase();
  TestMotorsRunInOrderAtPreFloat();
  TestWindowNotMetWaitsUntilDeadline();
  TestWindowBoundsAreInclusive();
  TestNotReadyAtDeadlineIsSkipped();
  TestRefusedStartFailsPlan();
  TestDonePlanNeverRerunsAndDisarmResets();
  TestOperatorSurfaceRules();
  TestPersistenceRoundTrip();
  TestSerializedShape();
  std::cout << "All fallback planner tests passed.\n";
  return 0;
}
