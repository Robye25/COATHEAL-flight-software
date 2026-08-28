#pragma once

// Operator-armed link-loss bend plan (redesign spec §10, owner decisions
// D3–D6). The planner is pure: no sockets, no hardware, no clock of its own
// -- SystemController feeds it one FallbackTickInput per tick and applies
// the action it returns through the same stepper path the operator's
// STEPPER_MOVETO uses. Everything here is therefore unit-testable with a
// scripted timeline (tests/unit/test_fallback_planner.cpp).
//
// Rules (all of them must hold for a motor to start):
//   fallback active, phase PRE_FLOAT or FLOAT, plan armed, motor configured
//   and pending, enabled + zeroed + healthy, no other plan motor running,
//   and (group temperature inside the window OR the deadline has passed).
// The deadline clock starts on the first tick fallback is active in
// PRE_FLOAT/FLOAT and is never persisted (a restart restarts it). A motor
// still not ready when the deadline has passed is skipped. Motors run in id
// order. A refused start fails the plan; a completed/failed plan never runs
// again, across restarts included, until the operator disarms it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coatheal/phase.hpp"

namespace coatheal {

enum class FallbackPlanState { kNone, kArmed, kRunning, kDone, kFailed };
enum class FallbackMotorState {
  kUnconfigured,
  kPending,
  kRunning,
  kDone,
  kSkipped,
  kFailed,
};

const char* ToString(FallbackPlanState state);
const char* ToString(FallbackMotorState state);
bool ParseFallbackPlanState(const std::string& text, FallbackPlanState* out);
bool ParseFallbackMotorState(const std::string& text, FallbackMotorState* out);

struct FallbackPlannerConfig {
  double bend_min_c = -40.0;
  double bend_max_c = 40.0;
  double bend_deadline_s = 1800.0;
};

struct FallbackMotorPlan {
  bool configured = false;
  std::int64_t target_usteps = 0;
  double hold_s = 0.0;
  double speed_hz = 0.0;  // 0 = keep the motor's current speed
  FallbackMotorState state = FallbackMotorState::kUnconfigured;
};

struct FallbackMotorInput {
  bool enabled = false;
  bool zeroed = false;
  bool healthy = false;
  bool moving_or_holding = false;
  // Mean of the motor's VALID sample temperatures; empty when none is valid.
  std::optional<double> group_temp_c;
};

struct FallbackTickInput {
  bool fallback_active = false;
  MissionPhase phase = MissionPhase::kBoot;
  std::chrono::steady_clock::time_point now{};
  std::vector<FallbackMotorInput> motors;
};

struct FallbackAction {
  int motor_id = 0;
  std::int64_t target_usteps = 0;
  double hold_s = 0.0;
  double speed_hz = 0.0;
};

class FallbackPlanner {
 public:
  FallbackPlanner(FallbackPlannerConfig cfg, std::size_t motor_count);

  // ---- operator surface (FALLBACK_PLAN / FALLBACK_ARM / FALLBACK_DISARM) ----
  // Loading a motor plan while the plan runs is refused. Loading one after
  // the plan completed or failed starts a fresh, unarmed plan with every
  // configured motor pending again.
  bool SetMotorPlan(int motor_id, std::int64_t target_usteps, double hold_s,
                    double speed_hz, std::string* error);
  bool Arm(std::string* error);
  void Disarm();

  // ---- per-tick decision ----
  // Returns the bend to start now, if any. The caller must answer with
  // ReportStartResult(): `accepted` marks the motion as running,
  // `retry_later` (e.g. the motion lock is held by another motor) puts the
  // motor back to pending without failing anything, anything else fails
  // the motor and the plan.
  std::optional<FallbackAction> Tick(const FallbackTickInput& in);
  void ReportStartResult(int motor_id, bool accepted, bool retry_later,
                         const std::string& error);

  // ---- queries ----
  FallbackPlanState state() const { return state_; }
  bool armed() const {
    return state_ == FallbackPlanState::kArmed ||
           state_ == FallbackPlanState::kRunning;
  }
  std::size_t motor_count() const { return motors_.size(); }
  const FallbackMotorPlan& motor(int motor_id) const;
  bool deadline_started() const { return deadline_started_; }
  double deadline_s() const { return cfg_.bend_deadline_s; }
  const std::string& last_error() const { return last_error_; }
  // `state=..;armed=..;deadline_s=..;deadline_started=..;m0=t/h/hz/state;m1=-`
  std::string StatusBody() const;

  // ---- persistence (plain key=value text, no JSON) ----
  // True once after any state change; SystemController persists on it.
  bool TakeDirty();
  std::string Serialize() const;
  // A corrupt or empty text leaves the planner with no plan and returns false.
  bool Deserialize(const std::string& text);
  bool SaveTo(const std::string& path) const;
  bool LoadFrom(const std::string& path);

 private:
  bool AllSettled() const;
  void ResetToNone();
  void ClearMotors();
  void MarkDirty() { dirty_ = true; }

  FallbackPlannerConfig cfg_;
  std::vector<FallbackMotorPlan> motors_;
  std::vector<int> ticks_since_start_;
  FallbackPlanState state_ = FallbackPlanState::kNone;
  bool deadline_started_ = false;
  std::chrono::steady_clock::time_point deadline_start_{};
  std::string last_error_;
  bool dirty_ = false;
};

}  // namespace coatheal
