#include "coatheal/fallback_planner.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace coatheal {

namespace {

bool ParseInt64Text(const std::string& text, std::int64_t* out) {
  try {
    std::size_t consumed = 0;
    const std::int64_t value = std::stoll(text, &consumed);
    if (consumed != text.size()) return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDoubleText(const std::string& text, double* out) {
  try {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size()) return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

std::string TrimLine(const std::string& in) {
  const auto begin = in.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = in.find_last_not_of(" \t\r\n");
  return in.substr(begin, end - begin + 1);
}

}  // namespace

const char* ToString(FallbackPlanState state) {
  switch (state) {
    case FallbackPlanState::kNone: return "none";
    case FallbackPlanState::kArmed: return "armed";
    case FallbackPlanState::kRunning: return "running";
    case FallbackPlanState::kDone: return "done";
    case FallbackPlanState::kFailed: return "failed";
  }
  return "none";
}

const char* ToString(FallbackMotorState state) {
  switch (state) {
    case FallbackMotorState::kUnconfigured: return "unconfigured";
    case FallbackMotorState::kPending: return "pending";
    case FallbackMotorState::kRunning: return "running";
    case FallbackMotorState::kDone: return "done";
    case FallbackMotorState::kSkipped: return "skipped";
    case FallbackMotorState::kFailed: return "failed";
  }
  return "unconfigured";
}

bool ParseFallbackPlanState(const std::string& text, FallbackPlanState* out) {
  static const FallbackPlanState kAll[] = {
      FallbackPlanState::kNone, FallbackPlanState::kArmed,
      FallbackPlanState::kRunning, FallbackPlanState::kDone,
      FallbackPlanState::kFailed};
  for (FallbackPlanState state : kAll) {
    if (text == ToString(state)) {
      if (out != nullptr) *out = state;
      return true;
    }
  }
  return false;
}

bool ParseFallbackMotorState(const std::string& text, FallbackMotorState* out) {
  static const FallbackMotorState kAll[] = {
      FallbackMotorState::kUnconfigured, FallbackMotorState::kPending,
      FallbackMotorState::kRunning,      FallbackMotorState::kDone,
      FallbackMotorState::kSkipped,      FallbackMotorState::kFailed};
  for (FallbackMotorState state : kAll) {
    if (text == ToString(state)) {
      if (out != nullptr) *out = state;
      return true;
    }
  }
  return false;
}

FallbackPlanner::FallbackPlanner(FallbackPlannerConfig cfg, std::size_t motor_count)
    : cfg_(cfg), motors_(motor_count), ticks_since_start_(motor_count, 0) {}

const FallbackMotorPlan& FallbackPlanner::motor(int motor_id) const {
  static const FallbackMotorPlan kEmpty;
  if (motor_id < 0 || static_cast<std::size_t>(motor_id) >= motors_.size()) {
    return kEmpty;
  }
  return motors_[static_cast<std::size_t>(motor_id)];
}

bool FallbackPlanner::SetMotorPlan(int motor_id, std::int64_t target_usteps,
                                   double hold_s, double speed_hz,
                                   std::string* error) {
  if (motor_id < 0 || static_cast<std::size_t>(motor_id) >= motors_.size()) {
    if (error) *error = "invalid motor id";
    return false;
  }
  if (state_ == FallbackPlanState::kRunning) {
    if (error) *error = "plan running";
    return false;
  }
  if (state_ == FallbackPlanState::kDone || state_ == FallbackPlanState::kFailed) {
    // A new plan after a finished one: every motor is pending again and
    // the plan must be armed again on purpose.
    ResetToNone();
  }
  FallbackMotorPlan& plan = motors_[static_cast<std::size_t>(motor_id)];
  plan.configured = true;
  plan.target_usteps = target_usteps;
  plan.hold_s = hold_s;
  plan.speed_hz = speed_hz;
  plan.state = FallbackMotorState::kPending;
  ticks_since_start_[static_cast<std::size_t>(motor_id)] = 0;
  MarkDirty();
  return true;
}

bool FallbackPlanner::Arm(std::string* error) {
  bool any_configured = false;
  for (const FallbackMotorPlan& plan : motors_) {
    any_configured = any_configured || plan.configured;
  }
  if (!any_configured) {
    if (error) *error = "no plan loaded";
    return false;
  }
  if (state_ == FallbackPlanState::kRunning) {
    if (error) *error = "plan running";
    return false;
  }
  if (state_ == FallbackPlanState::kDone || state_ == FallbackPlanState::kFailed) {
    if (error) {
      *error = std::string("plan already ") + ToString(state_) +
               " (FALLBACK_DISARM to clear)";
    }
    return false;
  }
  state_ = FallbackPlanState::kArmed;
  last_error_.clear();
  MarkDirty();
  return true;
}

void FallbackPlanner::ResetToNone() {
  state_ = FallbackPlanState::kNone;
  deadline_started_ = false;
  last_error_.clear();
  for (std::size_t i = 0; i < motors_.size(); ++i) {
    motors_[i].state = motors_[i].configured ? FallbackMotorState::kPending
                                              : FallbackMotorState::kUnconfigured;
    ticks_since_start_[i] = 0;
  }
}

void FallbackPlanner::ClearMotors() {
  for (FallbackMotorPlan& plan : motors_) {
    plan = FallbackMotorPlan{};
  }
}

void FallbackPlanner::Disarm() {
  ResetToNone();
  MarkDirty();
}

bool FallbackPlanner::AllSettled() const {
  for (const FallbackMotorPlan& plan : motors_) {
    if (!plan.configured) continue;
    if (plan.state != FallbackMotorState::kDone &&
        plan.state != FallbackMotorState::kSkipped) {
      return false;
    }
  }
  return true;
}

std::optional<FallbackAction> FallbackPlanner::Tick(const FallbackTickInput& in) {
  // Completion tracking runs regardless of the gate: a bend that started
  // while the link was down finishes even if the link comes back.
  for (std::size_t i = 0; i < motors_.size(); ++i) {
    FallbackMotorPlan& plan = motors_[i];
    if (plan.state != FallbackMotorState::kRunning) continue;
    ++ticks_since_start_[i];
    const bool still_moving =
        i < in.motors.size() && in.motors[i].moving_or_holding;
    if (ticks_since_start_[i] >= 1 && !still_moving) {
      plan.state = FallbackMotorState::kDone;
      MarkDirty();
    }
  }
  if (state_ == FallbackPlanState::kRunning && AllSettled()) {
    state_ = FallbackPlanState::kDone;
    MarkDirty();
  }
  if (state_ != FallbackPlanState::kArmed && state_ != FallbackPlanState::kRunning) {
    return std::nullopt;
  }

  const bool window_phase = in.fallback_active &&
                            (in.phase == MissionPhase::kPreFloat ||
                             in.phase == MissionPhase::kFloat);
  if (window_phase && !deadline_started_) {
    deadline_started_ = true;
    deadline_start_ = in.now;
  }
  if (!window_phase) return std::nullopt;

  for (const FallbackMotorPlan& plan : motors_) {
    if (plan.state == FallbackMotorState::kRunning) return std::nullopt;
  }

  const bool deadline_passed =
      deadline_started_ &&
      std::chrono::duration<double>(in.now - deadline_start_).count() >=
          cfg_.bend_deadline_s;

  for (std::size_t i = 0; i < motors_.size(); ++i) {
    FallbackMotorPlan& plan = motors_[i];
    if (!plan.configured || plan.state != FallbackMotorState::kPending) continue;
    const FallbackMotorInput input =
        i < in.motors.size() ? in.motors[i] : FallbackMotorInput{};
    const bool ready = input.enabled && input.zeroed && input.healthy;
    const bool window_ok = input.group_temp_c.has_value() &&
                           *input.group_temp_c >= cfg_.bend_min_c &&
                           *input.group_temp_c <= cfg_.bend_max_c;
    if (ready && (window_ok || deadline_passed)) {
      plan.state = FallbackMotorState::kRunning;
      state_ = FallbackPlanState::kRunning;
      ticks_since_start_[i] = 0;
      MarkDirty();
      FallbackAction action;
      action.motor_id = static_cast<int>(i);
      action.target_usteps = plan.target_usteps;
      action.hold_s = plan.hold_s;
      action.speed_hz = plan.speed_hz;
      return action;
    }
    if (!ready && deadline_passed) {
      plan.state = FallbackMotorState::kSkipped;
      MarkDirty();
      continue;
    }
    // Strict id order: this motor is waiting (window or readiness), so the
    // next one waits too.
    return std::nullopt;
  }

  if (AllSettled()) {
    state_ = FallbackPlanState::kDone;
    MarkDirty();
  }
  return std::nullopt;
}

void FallbackPlanner::ReportStartResult(int motor_id, bool accepted,
                                        bool retry_later,
                                        const std::string& error) {
  if (motor_id < 0 || static_cast<std::size_t>(motor_id) >= motors_.size()) return;
  FallbackMotorPlan& plan = motors_[static_cast<std::size_t>(motor_id)];
  if (accepted) return;
  if (retry_later) {
    plan.state = FallbackMotorState::kPending;
    ticks_since_start_[static_cast<std::size_t>(motor_id)] = 0;
    bool any_done = false;
    for (const FallbackMotorPlan& other : motors_) {
      any_done = any_done || other.state == FallbackMotorState::kDone;
    }
    state_ = any_done ? FallbackPlanState::kRunning : FallbackPlanState::kArmed;
    MarkDirty();
    return;
  }
  plan.state = FallbackMotorState::kFailed;
  state_ = FallbackPlanState::kFailed;
  last_error_ = error.empty() ? "start refused" : error;
  MarkDirty();
}

std::string FallbackPlanner::StatusBody() const {
  std::ostringstream oss;
  oss << "state=" << ToString(state_)
      << ";armed=" << (armed() ? "1" : "0")
      << ";deadline_s=" << cfg_.bend_deadline_s
      << ";deadline_started=" << (deadline_started_ ? "1" : "0");
  for (std::size_t i = 0; i < motors_.size(); ++i) {
    const FallbackMotorPlan& plan = motors_[i];
    oss << ";m" << i << '=';
    if (!plan.configured) {
      oss << '-';
    } else {
      oss << plan.target_usteps << '/' << plan.hold_s << '/' << plan.speed_hz
          << '/' << ToString(plan.state);
    }
  }
  if (!last_error_.empty()) oss << ";error=" << last_error_;
  return oss.str();
}

bool FallbackPlanner::TakeDirty() {
  const bool was = dirty_;
  dirty_ = false;
  return was;
}

std::string FallbackPlanner::Serialize() const {
  std::ostringstream oss;
  oss << "armed=" << (armed() ? "1" : "0") << '\n'
      << "state=" << ToString(state_) << '\n'
      << "deadline_s=" << cfg_.bend_deadline_s << '\n';
  for (std::size_t i = 0; i < motors_.size(); ++i) {
    const FallbackMotorPlan& plan = motors_[i];
    if (!plan.configured) continue;
    oss << 'm' << i << '=' << plan.target_usteps << ',' << plan.hold_s << ','
        << plan.speed_hz << ',' << ToString(plan.state) << '\n';
  }
  return oss.str();
}

bool FallbackPlanner::Deserialize(const std::string& text) {
  ClearMotors();
  ResetToNone();
  bool saw_state = false;
  FallbackPlanState state = FallbackPlanState::kNone;
  std::vector<FallbackMotorPlan> motors(motors_.size());
  std::istringstream lines(text);
  std::string raw;
  while (std::getline(lines, raw)) {
    const std::string line = TrimLine(raw);
    if (line.empty() || line[0] == '#') continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "state") {
      if (!ParseFallbackPlanState(value, &state)) return false;
      saw_state = true;
    } else if (key.size() >= 2 && key[0] == 'm') {
      std::int64_t index = 0;
      if (!ParseInt64Text(key.substr(1), &index) || index < 0 ||
          static_cast<std::size_t>(index) >= motors.size()) {
        return false;
      }
      std::vector<std::string> fields;
      std::istringstream spec(value);
      std::string field;
      while (std::getline(spec, field, ',')) fields.push_back(field);
      if (fields.size() != 4) return false;
      FallbackMotorPlan plan;
      plan.configured = true;
      if (!ParseInt64Text(fields[0], &plan.target_usteps) ||
          !ParseDoubleText(fields[1], &plan.hold_s) ||
          !ParseDoubleText(fields[2], &plan.speed_hz) ||
          !ParseFallbackMotorState(fields[3], &plan.state) ||
          plan.state == FallbackMotorState::kUnconfigured) {
        return false;
      }
      motors[static_cast<std::size_t>(index)] = plan;
    }
    // armed= and deadline_s= are informational; unknown keys are ignored.
  }
  if (!saw_state) return false;
  bool any_configured = false;
  for (const FallbackMotorPlan& plan : motors) {
    any_configured = any_configured || plan.configured;
  }
  if (state != FallbackPlanState::kNone && !any_configured) return false;
  motors_ = motors;
  state_ = state;
  return true;
}

bool FallbackPlanner::SaveTo(const std::string& path) const {
  std::error_code ec;
  const std::filesystem::path p(path);
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::out | std::ios::trunc);
  if (!out) return false;
  out << Serialize();
  return static_cast<bool>(out);
}

bool FallbackPlanner::LoadFrom(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    ClearMotors();
    ResetToNone();
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const bool ok = Deserialize(buffer.str());
  if (!ok) {
    ClearMotors();
    ResetToNone();
  }
  return ok;
}

}  // namespace coatheal
