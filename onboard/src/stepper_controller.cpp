#include "coatheal/stepper_controller.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "coatheal/stepper_channel.hpp"

namespace coatheal {

namespace {

StepperChannelConfig BuildLegacyChannelConfig(const StepperConfig& cfg) {
  StepperChannelConfig c;
  c.channel_id = 0;
  c.full_steps_per_rev = cfg.steps_per_rev;
  c.microstep = std::max(1, cfg.microstep);
  // Legacy default_step_hz/max_step_hz are in *microstep* Hz (REV-A
  // convention); convert to full-step Hz for the channel by dividing by the
  // microstep divisor. This keeps REV-A behavior bit-for-bit under the legacy
  // ctor and doesn't penalise operators who've calibrated the old numbers.
  const double div = static_cast<double>(c.microstep);
  c.default_step_hz = cfg.default_step_hz / div;
  c.max_step_hz = cfg.max_step_hz / div;
  c.accel_steps_per_s2 = 1e12;  // "instant" — REV-A had no ramp; keep behavior
                                // step-for-step identical under legacy ctor.
  c.allow_extended_microstep = true;  // REV-A accepted any divisor [1,32]
  c.max_position_steps = cfg.max_position_steps;
  c.samples = {0, 1, 2, 3};
  c.enable_on_boot = cfg.enable_on_boot;
  c.use_pulse_thread = false;
  return c;
}

}  // namespace

StepperController::StepperController(const StepperConfig& cfg,
                                     const BendScheduleConfig& schedule,
                                     std::unique_ptr<StepperDriver> driver)
    : schedule_(schedule) {
  StepperChannelConfig ccfg = BuildLegacyChannelConfig(cfg);
  channels_.emplace_back(std::make_unique<StepperChannel>(
      std::move(ccfg), std::move(driver), &lock_));
}

StepperController::StepperController(
    std::vector<StepperChannelConfig> channel_cfgs,
    std::vector<std::unique_ptr<StepperDriver>> drivers,
    const BendScheduleConfig& schedule)
    : schedule_(schedule) {
  if (channel_cfgs.size() != drivers.size()) {
    throw std::invalid_argument(
        "StepperController: channel config / driver count mismatch");
  }
  channels_.reserve(channel_cfgs.size());
  for (std::size_t i = 0; i < channel_cfgs.size(); ++i) {
    channels_.emplace_back(std::make_unique<StepperChannel>(
        std::move(channel_cfgs[i]), std::move(drivers[i]), &lock_));
  }
}

StepperController::~StepperController() = default;

StepperChannel* StepperController::ChannelById(int motor_id) {
  for (auto& ch : channels_) {
    if (ch && ch->channel_id() == motor_id) return ch.get();
  }
  return nullptr;
}

const StepperChannel* StepperController::ChannelById(int motor_id) const {
  for (const auto& ch : channels_) {
    if (ch && ch->channel_id() == motor_id) return ch.get();
  }
  return nullptr;
}

std::size_t StepperController::channel_count() const {
  return channels_.size();
}

std::vector<std::size_t> StepperController::SamplesForMotor(int motor_id) const {
  const StepperChannel* ch = ChannelById(motor_id);
  if (!ch) return {};
  return ch->samples();
}

bool StepperController::ResolvePhaseBend(MissionPhase phase,
                                         std::int64_t* steps,
                                         double* hold_s) const {
  switch (phase) {
    case MissionPhase::kAscentHold:
      *steps = schedule_.ascent_steps;
      *hold_s = schedule_.ascent_hold_s;
      return true;
    case MissionPhase::kActivationRamp:
      *steps = schedule_.activation_steps;
      *hold_s = schedule_.activation_hold_s;
      return true;
    case MissionPhase::kFloatHold:
      *steps = schedule_.float_steps;
      *hold_s = schedule_.float_hold_s;
      return true;
    case MissionPhase::kDescentFloor:
      *steps = schedule_.descent_steps;
      *hold_s = schedule_.descent_hold_s;
      return true;
    case MissionPhase::kStopped:
      return false;
  }
  return false;
}

void StepperController::ApplyPhaseSetpoint(MissionPhase phase) {
  std::int64_t steps = 0;
  double hold_s = 0.0;
  if (!ResolvePhaseBend(phase, &steps, &hold_s)) return;
  // Phase setpoints target channel 0 only (single bending actuator); the
  // second pulling motor is commanded explicitly via PULL_* rather than the
  // phase schedule.
  StepperChannel* ch = ChannelById(0);
  if (!ch) return;
  std::string err;
  ch->MoveToSteps(steps, hold_s, &err);
}

void StepperController::Tick(MissionPhase phase, double dt_s) {
  bool need_phase_apply = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!last_phase_valid_ || phase != last_phase_) {
      last_phase_ = phase;
      last_phase_valid_ = true;
      need_phase_apply = true;
    }
  }
  // Apply phase setpoint outside mu_ to avoid nested locks with channel
  // mutexes (MoveToSteps takes the channel's own mutex).
  if (need_phase_apply) {
    ApplyPhaseSetpoint(phase);
  }

  for (auto& ch : channels_) {
    if (ch) ch->Tick(dt_s);
  }
}

// ---- Legacy single-motor surface (motor 0) ----
bool StepperController::MoveSteps(std::int64_t delta_steps, std::string* error) {
  return MoveSteps(0, delta_steps, error);
}
bool StepperController::MoveToSteps(std::int64_t absolute_steps, double hold_s,
                                    std::string* error) {
  return MoveToSteps(0, absolute_steps, hold_s, error);
}
bool StepperController::Rotate(double revolutions, std::string* error) {
  return Rotate(0, revolutions, error);
}
bool StepperController::Home(std::string* error) {
  return Home(0, error);
}
void StepperController::Stop() {
  std::string err;
  Stop(0, &err);
}
bool StepperController::SetSpeed(double step_hz, std::string* error) {
  return SetSpeed(0, step_hz, error);
}
bool StepperController::SetMicrostep(int divisor, std::string* error) {
  return SetMicrostep(0, divisor, error);
}
bool StepperController::SetEnabled(bool enable) {
  std::string err;
  return SetEnabled(0, enable, &err);
}

// ---- REV-B multi-motor surface ----
bool StepperController::MoveSteps(int motor_id, std::int64_t delta_steps,
                                  std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->MoveSteps(delta_steps, error);
}

bool StepperController::MoveToSteps(int motor_id, std::int64_t absolute_steps,
                                    double hold_s, std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->MoveToSteps(absolute_steps, hold_s, error);
}

bool StepperController::Rotate(int motor_id, double revolutions,
                               std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->Rotate(revolutions, error);
}

bool StepperController::Home(int motor_id, std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->Home(error);
}

bool StepperController::Stop(int motor_id, std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  ch->Stop();
  return true;
}

bool StepperController::SetSpeed(int motor_id, double step_hz,
                                 std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->SetSpeed(step_hz, error);
}

bool StepperController::SetMicrostep(int motor_id, int divisor,
                                     std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->SetMicrostep(divisor, error);
}

bool StepperController::SetEnabled(int motor_id, bool enable,
                                   std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->SetEnabled(enable);
}

bool StepperController::ArmPull(int motor_id, std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->ArmPullCycle(error);
}

bool StepperController::ExecutePull(int motor_id, std::string* error) {
  StepperChannel* ch = ChannelById(motor_id);
  if (!ch) { if (error) *error = "unknown motor id"; return false; }
  return ch->ExecutePullCycle(error);
}

StepperStatus StepperController::Snapshot() const {
  return Snapshot(0);
}

StepperStatus StepperController::Snapshot(int motor_id) const {
  const StepperChannel* ch = ChannelById(motor_id);
  if (!ch) return StepperStatus{};
  return ch->Snapshot();
}

}  // namespace coatheal
