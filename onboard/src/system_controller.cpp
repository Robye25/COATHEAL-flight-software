#include "coatheal/system_controller.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>

#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/sd_notify.hpp"
#include "coatheal/stepper_channel.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/tmc2240_driver.hpp"

// Perf instrumentation. Off in flight builds (default). Enable with
// -DCOATHEAL_PERF_TRACE to emit a single "[perf]" line per 60 ticks with
// the per-stage microsecond breakdown of the tick loop. No extra work is
// done when undefined; everything folds away at compile time.
#ifdef COATHEAL_PERF_TRACE
#define COATHEAL_PERF_STAMP(target) target = std::chrono::steady_clock::now()
#define COATHEAL_PERF_DIFF_US(a, b) \
  std::chrono::duration_cast<std::chrono::microseconds>((b) - (a)).count()
#else
#define COATHEAL_PERF_STAMP(target) (void)0
#define COATHEAL_PERF_DIFF_US(a, b) 0
#endif

namespace coatheal {
namespace {

std::string Ack(const std::string& command, const std::string& message) {
  return "ACK," + command + "," + message;
}

std::string Nack(const std::string& command, const std::string& message) {
  return "NACK," + command + "," + message;
}

bool ParseDouble(const std::string& text, double* out) {
  try {
    const double value = std::stod(text);
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseIndex(const std::string& text, std::size_t* out) {
  try {
    const unsigned long value = std::stoul(text);
    *out = static_cast<std::size_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool IsLoopbackPeer(const std::string& ip) {
  return ip == "127.0.0.1" || ip == "::1";
}

}  // namespace

SystemController::SystemController(OnboardConfig config)
    : config_(std::move(config)),
      command_server_(config_.comms.command_port),
      sensor_manager_(config_, &spi_, &i2c_, &rtc_, &ina_),
      state_manager_(config_),
      thermal_controller_(config_),
      scheduler_(config_.power, config_.hardware.electronics_heater_index,
                 &motion_lock_),
      storage_manager_(config_.storage.primary_log_path, config_.storage.secondary_log_path),
      telemetry_queue_(config_.storage.queue_dir,
                       config_.storage.queue_retention_hours,
                       config_.storage.queue_max_bytes),
      telemetry_client_(config_.comms.telemetry_host,
                        config_.comms.telemetry_port,
                        config_.comms.command_port,
                        config_.comms.reconnect_ms,
                        config_.comms.discovery_enabled,
                        config_.comms.discovery_port,
                        config_.comms.static_ground_ip,
                        config_.comms.static_pi_ip,
                        config_.comms.discovery_period_ms,
                        config_.comms.rediscover_period_s,
                        config_.comms.failover_grace_s,
                        config_.comms.priority),
      last_heater_duty_(config_.hardware.heater_count, 0.0) {
  live_tick_hz_.store(std::max(0.1, config_.runtime.tick_hz));
  control_overrides_.heater_duty_overrides.resize(config_.hardware.heater_count);
  control_overrides_.temp_targets_c.resize(config_.hardware.heater_count);
  control_overrides_.pid_overrides.resize(config_.hardware.heater_count);
  bend_sequences_.resize(config_.motors.size());
  motor_zeroed_.assign(config_.motors.size(), false);
}

bool ParseInt64(const std::string& text, std::int64_t* out) {
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

bool IsSequenceNameValid(const std::string& name) {
  if (name.empty() || name.size() > 32) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
  });
}

bool SystemController::Initialize(std::string* error) {
  sensor_manager_.Start();
  if (config_.runtime.use_simulated_pwm) {
    pwm_ = std::make_unique<SimulatedPwmController>(config_.hardware.heater_count);
  } else {
    pwm_ = std::make_unique<LibgpiodPwmController>(
        config_.runtime.gpio_chip, config_.hardware.heater_count,
        config_.heaters.output_lines, config_.heaters.pwm_frequency_hz,
        config_.heaters.active_high);
  }

  if (config_.runtime.use_simulated_pwm || !config_.hal.status_led_enabled) {
    status_led_ = std::make_unique<SimulatedStatusLed>("heartbeat",
                                                       config_.hal.status_led_line);
  } else {
    status_led_ = std::make_unique<GpioStatusLed>(
        config_.runtime.gpio_chip, config_.hal.status_led_line, "heartbeat");
  }
  if (config_.runtime.use_simulated_pwm || !config_.hal.mode_led_enabled) {
    mode_led_ = std::make_unique<SimulatedStatusLed>("mode",
                                                     config_.hal.mode_led_line);
  } else {
    mode_led_ = std::make_unique<GpioStatusLed>(
        config_.runtime.gpio_chip, config_.hal.mode_led_line, "mode");
  }
  mode_led_->Set(StatusLed::Pattern::kSolid);

  // Final BOM: two TMC2240-driven NEMA 17 ball-screw actuators. Motor channel,
  // sample mapping, SPI device, current, and GPIO lines come from onboard.ini.
  std::vector<StepperChannelConfig> channel_cfgs;
  channel_cfgs.reserve(config_.motors.size());
  for (std::size_t i = 0; i < config_.motors.size(); ++i) {
    StepperChannelConfig cfg;
    cfg.channel_id = static_cast<int>(i);
    cfg.full_steps_per_rev = config_.stepper.steps_per_rev;
    cfg.max_step_hz = config_.pull.max_step_hz;
    cfg.default_step_hz = config_.stepper.default_step_hz;
    cfg.accel_steps_per_s2 = config_.pull.accel_steps_per_s2;
    cfg.microstep = config_.pull.microstep;
    cfg.max_position_steps = config_.stepper.max_position_steps;
    cfg.samples = config_.motors[i].samples;
    cfg.pull_travel_full_steps = config_.pull.travel_full_steps;
    cfg.pull_hold_s = config_.pull.hold_s;
    cfg.enable_on_boot = config_.stepper.enable_on_boot;
    cfg.use_pulse_thread = !config_.runtime.use_simulated_pwm;
    cfg.driver_retry_ms = config_.motors[i].retry_ms;
    channel_cfgs.push_back(std::move(cfg));
  }

  std::vector<std::unique_ptr<StepperDriver>> drivers;
  if (config_.runtime.use_simulated_pwm) {
    for (std::size_t i = 0; i < config_.motors.size(); ++i) {
      drivers.emplace_back(std::make_unique<SimulatedStepperDriver>());
    }
  } else {
    // Keep an unhealthy TMC backend present for diagnostics/retry, but never
    // fall back to unconfigured raw STEP/DIR motion.
    auto build_tmc =
        [&](const char* motor_label, const MotorConfig& motor)
            -> std::unique_ptr<StepperDriver> {
      Tmc2240Config tcfg;
      tcfg.gpio_chip = motor.gpio_chip;
      tcfg.spi_device = motor.spi_device;
      tcfg.cs_line = motor.cs_line;
      tcfg.step_line = motor.step_line;
      tcfg.dir_line = motor.dir_line;
      tcfg.enable_line = motor.enable_line;
      tcfg.invert_direction = motor.invert_direction;
      tcfg.enable_active_low = motor.enable_active_low;
      tcfg.microstep = config_.pull.microstep;
      tcfg.run_current_a_rms = motor.run_current_a_rms;
      tcfg.current_range_a_peak = motor.current_range_a_peak;
      tcfg.hold_current_frac = motor.hold_current_frac;
      tcfg.stealth_chop = motor.stealth_chop;
      tcfg.spi_speed_hz = motor.spi_speed_hz;
      tcfg.pulse_high_us = motor.pulse_high_us;
      auto tmc = std::make_unique<Tmc2240Driver>(tcfg);
      if (!tmc->healthy()) {
        tmc_spi_ok_ = false;
        std::cerr << "[system] " << motor_label
                  << ": TMC2240 bring-up on " << motor.spi_device
                  << " failed; motor remains unavailable until CHECK or restart"
                  << " completes SPI setup." << '\n';
      }
      return tmc;
    };
    drivers.emplace_back(build_tmc(
        "motor0", config_.motors[0]));
    drivers.emplace_back(build_tmc(
        "motor1", config_.motors[1]));
  }
  spi_.set_healthy(config_.runtime.use_simulated_pwm || tmc_spi_ok_);

  stepper_ = std::make_unique<StepperController>(
      std::move(channel_cfgs), std::move(drivers));

  // Routing fix (Agent C, 2026-04-17): StepperController owns its own
  // MotionLock, and that is the one every StepperChannel actually takes on
  // ArmPullCycle. The SystemController::motion_lock_ member was a parallel,
  // never-acquired lock — so both the HeaterScheduler interlock and the
  // EVT,PULL edge detector were silently no-ops in bench mode. Retarget
  // them to the authoritative lock now that stepper_ exists.
  MotionLock* const authoritative_lock = stepper_->motion_lock();
  scheduler_.SetMotionLock(authoritative_lock);
  active_motion_lock_ = authoritative_lock;

  // One PullState entry per channel for EVT,PULL edge detection in Run().
  pull_state_.resize(stepper_->channel_count());

  std::string degraded_error;
  if (!storage_manager_.Initialize(&degraded_error)) {
    std::cerr << "[system] storage degraded: " << degraded_error << '\n';
  }

  degraded_error.clear();
  if (!telemetry_queue_.Initialize(&degraded_error)) {
    std::cerr << "[system] telemetry queue is memory-only: "
              << degraded_error << '\n';
  }

  if (!command_server_.Start(
          [this](const std::string& line, const std::string& peer_ip) {
            return HandleCommandLine(line, peer_ip);
          }, error)) {
    return false;
  }

  // Tell systemd we are READY=1 so the WatchdogSec timer in the unit file
  // starts ticking from a known good state. No-op when not running under
  // systemd. (BEXUS User Manual §5.9 — durability and auto-recovery.)
  telemetry_client_.Start();

  SdNotify("READY=1");
  SdNotify("STATUS=initialised");

  return true;
}

bool SystemController::AnySequencePaused() const {
  std::lock_guard<std::mutex> lock(sequence_mu_);
  return std::any_of(
      bend_sequences_.begin(), bend_sequences_.end(),
      [](const BendSequenceRuntime& runtime) { return runtime.paused; });
}

std::string SystemController::SequenceStatus(int motor_id) const {
  std::lock_guard<std::mutex> lock(sequence_mu_);
  if (motor_id < 0 ||
      static_cast<std::size_t>(motor_id) >= bend_sequences_.size()) {
    return "invalid motor";
  }
  const BendSequenceRuntime& runtime =
      bend_sequences_[static_cast<std::size_t>(motor_id)];
  std::ostringstream oss;
  oss << "motor=" << motor_id
      << ";zeroed=" << (motor_zeroed_[static_cast<std::size_t>(motor_id)] ? "1" : "0")
      << ";running=" << (runtime.running ? "1" : "0")
      << ";paused=" << (runtime.paused ? "1" : "0")
      << ";name=" << runtime.active_name
      << ";step=" << runtime.step_index;
  if (!runtime.fault.empty()) oss << ";fault=" << runtime.fault;
  return oss.str();
}

void SystemController::InhibitHeatersForMotion() {
  if (pwm_ != nullptr) {
    for (std::size_t i = 0; i < config_.hardware.heater_count; ++i) {
      pwm_->SetDuty(i, 0.0);
    }
  }
  std::lock_guard<std::mutex> lock(overrides_mu_);
  last_heater_duty_.assign(config_.hardware.heater_count, 0.0);
}

void SystemController::TickBendSequences() {
  if (!stepper_) return;
  std::lock_guard<std::mutex> lock(sequence_mu_);
  for (std::size_t motor = 0; motor < bend_sequences_.size(); ++motor) {
    BendSequenceRuntime& runtime = bend_sequences_[motor];
    if (!runtime.running || runtime.paused) continue;
    const auto definition_it = runtime.definitions.find(runtime.active_name);
    if (definition_it == runtime.definitions.end() ||
        definition_it->second.steps.empty()) {
      runtime.running = false;
      runtime.paused = true;
      runtime.fault = "active sequence definition missing";
      continue;
    }
    const BendSequenceDefinition& definition = definition_it->second;
    if (runtime.step_index >= definition.steps.size()) {
      runtime.running = false;
      runtime.step_queued = false;
      runtime.active_name.clear();
      continue;
    }

    const BendSequenceStep& step = definition.steps[runtime.step_index];
    const StepperStatus status = stepper_->Snapshot(static_cast<int>(motor));
    if (runtime.step_queued) {
      if (status.moving || status.holding) continue;
      if (status.position_steps != step.target_usteps) {
        runtime.paused = true;
        runtime.fault = "motor stopped before target";
        runtime.step_queued = false;
        continue;
      }
      ++runtime.step_index;
      runtime.step_queued = false;
      if (runtime.step_index >= definition.steps.size()) {
        runtime.running = false;
        runtime.active_name.clear();
        continue;
      }
    }

    const BendSequenceStep& next_step =
        definition.steps[runtime.step_index];
    std::string error;
    if (next_step.speed_hz.has_value() &&
        !stepper_->SetSpeed(static_cast<int>(motor),
                            next_step.speed_hz.value(), &error)) {
      runtime.paused = true;
      runtime.fault = error;
      continue;
    }
    InhibitHeatersForMotion();
    if (!stepper_->MoveToSteps(static_cast<int>(motor),
                               next_step.target_usteps,
                               next_step.hold_s, &error)) {
      if (error == "motion lock held by another motor") {
        continue;
      }
      runtime.paused = true;
      runtime.fault = error;
      continue;
    }
    runtime.step_queued = true;
  }
}

void SystemController::StopNonSequenceMotionOnFallback() {
  if (!stepper_) return;
  std::vector<bool> sequence_running;
  {
    std::lock_guard<std::mutex> lock(sequence_mu_);
    sequence_running.reserve(bend_sequences_.size());
    for (const BendSequenceRuntime& runtime : bend_sequences_) {
      sequence_running.push_back(runtime.running);
    }
  }
  for (std::size_t motor = 0; motor < stepper_->channel_count(); ++motor) {
    if (motor < sequence_running.size() && sequence_running[motor]) continue;
    std::string error;
    stepper_->Stop(static_cast<int>(motor), &error);
  }
}

int SystemController::Run() {
  bool last_link_ok = false;

#ifdef COATHEAL_PERF_TRACE
  // Rolling accumulators reset every 60 ticks. Per-stage total µs and
  // max-observed µs are emitted on flush. p50/p99 are computed from a
  // ring buffer of 60 total-tick-us samples.
  constexpr int kPerfWindow = 60;
  constexpr int kNumStages = 10;
  std::array<std::uint64_t, kNumStages> perf_sum_us{};
  std::array<std::uint64_t, kNumStages> perf_max_us{};
  std::array<std::uint64_t, kPerfWindow> perf_tick_total_us{};
  std::array<std::uint64_t, kPerfWindow> perf_enqueue_us{};
  std::uint64_t perf_enqueue_sum = 0;
  std::uint64_t perf_enqueue_max = 0;
  int perf_ticks = 0;
  auto flush_perf = [&]() {
    if (perf_ticks == 0) return;
    std::array<std::uint64_t, kPerfWindow> sorted = perf_tick_total_us;
    std::sort(sorted.begin(), sorted.begin() + perf_ticks);
    const int p50_idx = (perf_ticks * 50) / 100;
    const int p99_idx = (perf_ticks >= 100) ? ((perf_ticks * 99) / 100)
                                             : (perf_ticks - 1);
    std::array<std::uint64_t, kPerfWindow> enq_sorted = perf_enqueue_us;
    std::sort(enq_sorted.begin(), enq_sorted.begin() + perf_ticks);
    const int enq_p99_idx = (perf_ticks >= 100) ? ((perf_ticks * 99) / 100)
                                                 : (perf_ticks - 1);
    std::fprintf(
        stderr,
        "[perf] n=%d avg_us=%llu max_us=%llu p50_us=%llu p99_us=%llu "
        "enqueue_avg_us=%llu enqueue_max_us=%llu enqueue_p99_us=%llu "
        "stages_avg_us=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] "
        "stages_max_us=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
        perf_ticks,
        (unsigned long long)(std::accumulate(
            perf_tick_total_us.begin(), perf_tick_total_us.begin() + perf_ticks,
            0ULL) / perf_ticks),
        (unsigned long long)sorted[perf_ticks - 1],
        (unsigned long long)sorted[p50_idx],
        (unsigned long long)sorted[p99_idx],
        (unsigned long long)(perf_enqueue_sum / perf_ticks),
        (unsigned long long)perf_enqueue_max,
        (unsigned long long)enq_sorted[enq_p99_idx],
        (unsigned long long)(perf_sum_us[0] / perf_ticks),
        (unsigned long long)(perf_sum_us[1] / perf_ticks),
        (unsigned long long)(perf_sum_us[2] / perf_ticks),
        (unsigned long long)(perf_sum_us[3] / perf_ticks),
        (unsigned long long)(perf_sum_us[4] / perf_ticks),
        (unsigned long long)(perf_sum_us[5] / perf_ticks),
        (unsigned long long)(perf_sum_us[6] / perf_ticks),
        (unsigned long long)(perf_sum_us[7] / perf_ticks),
        (unsigned long long)(perf_sum_us[8] / perf_ticks),
        (unsigned long long)(perf_sum_us[9] / perf_ticks),
        (unsigned long long)perf_max_us[0],
        (unsigned long long)perf_max_us[1],
        (unsigned long long)perf_max_us[2],
        (unsigned long long)perf_max_us[3],
        (unsigned long long)perf_max_us[4],
        (unsigned long long)perf_max_us[5],
        (unsigned long long)perf_max_us[6],
        (unsigned long long)perf_max_us[7],
        (unsigned long long)perf_max_us[8],
        (unsigned long long)perf_max_us[9]);
    perf_sum_us.fill(0);
    perf_max_us.fill(0);
    perf_enqueue_sum = 0;
    perf_enqueue_max = 0;
    perf_ticks = 0;
  };
#endif

  while (running_) {
    // Recompute tick duration every iteration so SET_TICK_HZ takes effect live.
    const double tick_hz = std::max(0.1, live_tick_hz_.load());
    const auto tick_duration = std::chrono::duration<double>(1.0 / tick_hz);
    const auto tick_start = std::chrono::steady_clock::now();

#ifdef COATHEAL_PERF_TRACE
    std::chrono::steady_clock::time_point perf_ts[kNumStages + 1];
    std::chrono::steady_clock::time_point perf_enqueue_end;
    perf_ts[0] = tick_start;
#endif

    StateOverrides state_overrides;
    ControlOverrides control_overrides;
    {
      std::lock_guard<std::mutex> lock(overrides_mu_);
      state_overrides = state_overrides_;
      control_overrides = control_overrides_;
      state_overrides_.force_start = false;
      state_overrides_.force_stop = false;
      state_overrides_.reset_control = false;
      state_overrides_.shutdown_safe = false;
    }

    if (state_overrides.reset_control) {
      thermal_controller_.Reset();
    }

    const SystemMode current_mode = mode_.load();
    if (last_link_ok) {
      link_seen_ = true;
      link_loss_s_ = 0.0;
    } else if (link_seen_) {
      link_loss_s_ += tick_duration.count();
    }
    const bool transmit_enabled = telemetry_client_.transmit_enabled();
    link_loss_fallback_active_ =
        config_.manual.manual_first &&
        config_.manual.link_loss_fallback_enabled &&
        current_mode == SystemMode::kRun &&
        transmit_enabled &&
        link_seen_ &&
        !last_link_ok &&
        link_loss_s_ >= config_.manual.link_loss_fallback_s;
    if (link_loss_fallback_active_ && !link_loss_fallback_was_active_) {
      StopNonSequenceMotionOnFallback();
    }
    link_loss_fallback_was_active_ = link_loss_fallback_active_;
    const bool fallback_floor_control =
        link_loss_fallback_active_;
    const bool automatic_phase_tracking =
        link_loss_fallback_active_;
    COATHEAL_PERF_STAMP(perf_ts[1]);  // stage 0: snapshot overrides + mode load

    SensorSnapshot snapshot = sensor_manager_.ReadSnapshot(
        state_manager_.phase(), last_heater_duty_, tick_duration.count());
    {
      std::lock_guard<std::mutex> lock(overrides_mu_);
      last_sensor_snapshot_ = snapshot;
    }
    COATHEAL_PERF_STAMP(perf_ts[2]);  // stage 1: sensor snapshot

    MissionPhase phase = state_manager_.phase();
    if (automatic_phase_tracking) {
      phase = state_manager_.Update(
          snapshot.ambient_pressure_mbar, snapshot.sample_temps_c, state_overrides,
          std::chrono::steady_clock::now());
    }

    const bool heaters_allowed = (current_mode == SystemMode::kRun);
    ControlOverrides effective_control = control_overrides;
    effective_control.floor_control_enabled =
        fallback_floor_control;
    if (!heaters_allowed) {
      effective_control.heaters_off = true;
    }
    COATHEAL_PERF_STAMP(perf_ts[3]);  // stage 2: phase update + effective ovr

    std::vector<double> requested_duty = thermal_controller_.ComputeRequestedDuty(
        phase, snapshot, tick_duration.count(), effective_control);
    bool debug_heat_requested = false;
    {
      std::lock_guard<std::mutex> lock(overrides_mu_);
      if (heater_test_.active) {
        const bool motion_active =
            active_motion_lock_ != nullptr && active_motion_lock_->holder() != -1;
        const bool still_allowed =
            config_.runtime.bench_mode &&
            debug_armed_.load() &&
            current_mode == SystemMode::kRun &&
            !motion_active &&
            std::chrono::steady_clock::now() < heater_test_.until;
        if (!still_allowed) {
          heater_test_.active = false;
        } else {
          std::fill(requested_duty.begin(), requested_duty.end(), 0.0);
          if (heater_test_.heater_index < requested_duty.size()) {
            requested_duty[heater_test_.heater_index] = heater_test_.duty;
            debug_heat_requested = heater_test_.duty > 0.0;
          }
        }
      }
    }
    COATHEAL_PERF_STAMP(perf_ts[4]);  // stage 3: thermal controller

    // The fallback floor follows flight phases. Explicit operator duty and
    // temperature commands remain available in any RUN phase.
    const bool any_flying_phase =
        (phase == MissionPhase::kAscent || phase == MissionPhase::kPreFloat ||
         phase == MissionPhase::kFloat || phase == MissionPhase::kDescent);
    const bool manual_heat_requested =
        control_overrides.all_heaters_override.has_value() ||
        control_overrides.single_heater_override.has_value() ||
        std::any_of(control_overrides.heater_duty_overrides.begin(),
                    control_overrides.heater_duty_overrides.end(),
                    [](const std::optional<double>& value) {
                      return value.has_value();
                    }) ||
        std::any_of(control_overrides.temp_targets_c.begin(),
                    control_overrides.temp_targets_c.end(),
                    [](const std::optional<double>& value) {
                      return value.has_value();
                    }) ||
        debug_heat_requested;
    const std::vector<double> scheduled_duty = scheduler_.Schedule(
        requested_duty,
        heaters_allowed && (any_flying_phase || manual_heat_requested),
        tick_duration.count());
    COATHEAL_PERF_STAMP(perf_ts[5]);  // stage 4: heater scheduler

    for (std::size_t i = 0; i < scheduled_duty.size(); ++i) {
      pwm_->SetDuty(i, scheduled_duty[i]);
    }
    {
      std::lock_guard<std::mutex> lock(overrides_mu_);
      last_heater_duty_ = scheduled_duty;
    }

    if (stepper_) {
      TickBendSequences();
      stepper_->Tick(phase, tick_duration.count());
      const bool sequence_fault =
          !stepper_->AllHealthy() || thermal_controller_.overtemp_latched();
      if (sequence_fault) {
        std::lock_guard<std::mutex> lock(sequence_mu_);
        for (std::size_t motor = 0; motor < bend_sequences_.size(); ++motor) {
          BendSequenceRuntime& runtime = bend_sequences_[motor];
          if (!runtime.running || runtime.paused) continue;
          runtime.paused = true;
          runtime.step_queued = false;
          runtime.fault = !stepper_->AllHealthy()
                              ? "stepper backend unhealthy"
                              : "overtemperature latch";
          std::string stop_error;
          stepper_->Stop(static_cast<int>(motor), &stop_error);
        }
      }
    }
    COATHEAL_PERF_STAMP(perf_ts[6]);  // stage 5: pwm set + stepper tick

    TelemetryRecord record;
    record.seq = seq_++;
    record.phase = phase;
    record.mode = current_mode;
    record.sensors = snapshot;
    record.heater_duty = scheduled_duty;
    record.status = storage_manager_.status();
    record.status.spi_ok = spi_.healthy();
    record.status.i2c_ok = sensor_manager_.i2c_ok();
    record.status.link_ok = last_link_ok;
    record.status.t_ambient_ok = sensor_manager_.t_ambient_ok();
    record.status.p_ambient_ok = sensor_manager_.p_ambient_ok();
    record.status.overtemp_ok = !thermal_controller_.overtemp_latched();
    record.status.uniformity_ok = thermal_controller_.uniformity_ok();
    record.status.energy_ok = !scheduler_.is_budget_exhausted();
    record.status.rs485_ok = sensor_manager_.rs485_ok();
    record.status.pwm_ok = pwm_ != nullptr && pwm_->healthy();
    if (pwm_ != nullptr && pwm_->channel_count() > 0) {
      const std::size_t healthy_channels = pwm_->healthy_channel_count();
      record.pwm_state =
          healthy_channels == pwm_->channel_count()
              ? ComponentState::kOk
              : (healthy_channels > 0 ? ComponentState::kDegraded
                                      : ComponentState::kFailed);
    }
    record.status.stepper_ok = stepper_ != nullptr && stepper_->AllHealthy();
    record.status.sample_temp_ok = sensor_manager_.sample_temp_ok();
    record.status.simulated = sensor_manager_.simulated();
    record.status.sequence_paused = AnySequencePaused();
    record.status.heater_inhibited = scheduler_.heater_inhibited();
    record.status.resistance_ok = sensor_manager_.resistance_ok();
    if (stepper_) {
      // One snapshot per channel drives the STEPPER0=/STEPPER1= telemetry
      // segments. Reserve up-front so the two push_back calls avoid reallocs.
      const std::size_t n_channels = stepper_->channel_count();
      record.steppers.clear();
      record.steppers.reserve(n_channels);
      for (std::size_t i = 0; i < n_channels; ++i) {
        record.steppers.push_back(stepper_->Snapshot(static_cast<int>(i)));
      }
    }

    const std::string line = SerializeTelemetryDataFrame(record, telemetry_client_.session_id());
    COATHEAL_PERF_STAMP(perf_ts[7]);  // stage 6: build+serialize telemetry

    storage_manager_.WriteLine(line);
    COATHEAL_PERF_STAMP(perf_ts[8]);  // stage 7: storage write

    std::string queue_error;
    QueuedTelemetryFrame queued_frame;
    queued_frame.queued_epoch_s = CurrentUnixEpochSeconds();
    queued_frame.session_id = telemetry_client_.session_id();
    queued_frame.seq = record.seq;
    queued_frame.frame = line;

    if (!telemetry_queue_.Enqueue(queued_frame, &queue_error)) {
      last_link_ok = false;
    }
    COATHEAL_PERF_STAMP(perf_enqueue_end);  // sub-stage: enqueue-only latency

    std::string drain_error;
    if (!DrainTelemetryQueue(&last_link_ok, &drain_error)) {
      last_link_ok = false;
      std::cerr << "[telemetry] drain error: " << drain_error << '\n';
    }
    COATHEAL_PERF_STAMP(perf_ts[9]);  // stage 8: queue enqueue + drain

    // Emit EVT,PULL after each motor finishes a pull cycle.
    //
    // A pull starts when the MotionLock transitions to this motor (rising
    // edge of the lock holder) and completes when the channel releases it
    // again (kIdle at retract end). The earlier implementation gated on
    // `moving==true`, which misses pulls whose outgoing-leg motion completes
    // between two telemetry ticks — common at tick_hz=2 + 100 full-steps/s.
    // Using the lock as the authoritative "pull in progress" signal is
    // also tick-rate-independent and matches the heater-interlock contract
    // (HeaterScheduler zeros duty while the lock is held; the pull-event
    // emitter must bracket exactly the same window). (Agent C, 2026-04-17.)
    if (stepper_ && !record.steppers.empty()) {
      if (pull_state_.size() != record.steppers.size()) {
        pull_state_.resize(record.steppers.size());
      }
      for (std::size_t i = 0; i < record.steppers.size(); ++i) {
        PullState& ps = pull_state_[i];
        const StepperStatus& s = record.steppers[i];
        // Use the authoritative lock exposed by the StepperController (see
        // Initialize() for the routing fix). Fall back to the local member
        // so this still works in unit harnesses that do not construct the
        // stepper.
        MotionLock* const lock =
            (active_motion_lock_ != nullptr) ? active_motion_lock_
                                             : &motion_lock_;
        const bool lock_held_now =
            (lock->holder() == static_cast<int>(i));

        if (!ps.lock_held && lock_held_now) {
          // Rising edge: pull just started. Snapshot the reference position
          // so `steps_moved` reports net travel during the whole cycle.
          ps.lock_held = true;
          ps.was_moving = true;  // maintained for back-compat
          ps.start_ts = snapshot.timestamp_utc;
          ps.start_pos = s.position_steps;
        } else if (ps.lock_held && !lock_held_now) {
          // Falling edge: pull completed; motor released the lock in
          // StepperChannel::Tick once the retract leg finished.
          HeatingPullEvent pev;
          pev.pull_id = next_pull_id_++;
          pev.motor_id = static_cast<int>(i);
          pev.start_ts = ps.start_ts;
          pev.steps_moved = s.position_steps - ps.start_pos;
          pev.hold_s = s.hold_remaining_s;
          pev.samples = stepper_->SamplesForMotor(static_cast<int>(i));

          const std::string evt_line =
              SerializeTelemetryPullEventFrame(pev, telemetry_client_.session_id());
          storage_manager_.WriteLine(evt_line);

          QueuedTelemetryFrame evt_frame;
          evt_frame.queued_epoch_s = CurrentUnixEpochSeconds();
          evt_frame.session_id = telemetry_client_.session_id();
          evt_frame.seq = seq_++;
          evt_frame.frame = evt_line;
          std::string evt_error;
          telemetry_queue_.Enqueue(evt_frame, &evt_error);

          // Feed the optional resistance simulator after each completed pull.
          sensor_manager_.NotePullCompleted(static_cast<int>(i));

          ps.lock_held = false;
          ps.was_moving = false;
        }
      }
    }

    if (phase == MissionPhase::kStopped) {
      running_ = false;
    }

    // Heartbeat: toggle the status LED every tick so a human can see the
    // main loop is alive. Mode-indicator LED: Group A owns SystemMode — at
    // merge time, replace the kSolid default with the mapped pattern from
    // the current SystemMode (e.g. kHeartbeat=NOMINAL, kFastBlink=WARN,
    // kSOS=FAULT). TODO(group-a-merge): wire SystemMode -> StatusLed::Pattern.
    if (status_led_) {
      status_led_->Toggle();
    }

    // Pet the systemd watchdog every tick. If the main loop hangs (e.g. an
    // adapter blocks indefinitely) systemd will SIGKILL us after WatchdogSec
    // and Restart=always brings us back. Silent no-op outside systemd.
    SdNotifyWatchdog();
    COATHEAL_PERF_STAMP(perf_ts[10]);  // stage 9: pull-evt + LED + sd_notify

#ifdef COATHEAL_PERF_TRACE
    {
      std::uint64_t tick_total_us = 0;
      for (int s = 0; s < kNumStages; ++s) {
        const std::uint64_t dt = static_cast<std::uint64_t>(
            COATHEAL_PERF_DIFF_US(perf_ts[s], perf_ts[s + 1]));
        perf_sum_us[s] += dt;
        if (dt > perf_max_us[s]) perf_max_us[s] = dt;
        tick_total_us += dt;
      }
      perf_tick_total_us[perf_ticks] = tick_total_us;
      // Enqueue-only latency (pre-storage-write is stage 7; enqueue lives
      // between that and perf_ts[9]). We measure from stage 7 end to
      // perf_enqueue_end, which isolates the fsync cost of Enqueue() alone.
      const std::uint64_t enq_us = static_cast<std::uint64_t>(
          COATHEAL_PERF_DIFF_US(perf_ts[8], perf_enqueue_end));
      perf_enqueue_us[perf_ticks] = enq_us;
      perf_enqueue_sum += enq_us;
      if (enq_us > perf_enqueue_max) perf_enqueue_max = enq_us;
      ++perf_ticks;
      if (perf_ticks >= kPerfWindow) {
        flush_perf();
      }
    }
#endif

    const auto elapsed = std::chrono::steady_clock::now() - tick_start;
    if (elapsed < tick_duration) {
      std::this_thread::sleep_for(tick_duration - elapsed);
    }
  }

#ifdef COATHEAL_PERF_TRACE
  flush_perf();
#endif

  SdNotify("STOPPING=1");
  {
    std::lock_guard<std::mutex> lock(overrides_mu_);
    heater_test_.active = false;
  }
  command_server_.Stop();
  telemetry_client_.Stop();
  return 0;
}

bool SystemController::DrainTelemetryQueue(bool* link_ok, std::string* error) {
  if (link_ok != nullptr) {
    *link_ok = false;
  }

  std::vector<QueuedTelemetryFrame> pending = telemetry_queue_.PendingFrames();
  if (pending.empty()) {
    if (link_ok != nullptr) {
      *link_ok = telemetry_client_.is_connected();
    }
    return true;
  }

  // Rev C: limit drain to a small batch per tick so the control loop is not
  // blocked by a large backlog (the Pi was accumulating 12k+ frames).
  constexpr std::size_t kMaxDrainPerTick = 10;
  std::size_t drained = 0;

  for (const QueuedTelemetryFrame& frame : pending) {
    if (drained >= kMaxDrainPerTick) break;
    TelemetryAck ack;
    if (!telemetry_client_.SendFrameAwaitAck(frame.frame, &ack)) {
      if (error != nullptr) {
        *error = "failed to send telemetry frame";
      }
      return false;
    }

    const bool event_ack =
        frame.frame.rfind("EVT,", 0) == 0 &&
        ack.session_id == frame.session_id &&
        ack.seq == 0U;
    if (event_ack) {
      if (!telemetry_queue_.AcknowledgeExact(frame, error)) {
        return false;
      }
      if (link_ok != nullptr) {
        *link_ok = true;
      }
      ++drained;
      continue;
    }

    if (ack.session_id != frame.session_id || ack.seq < frame.seq) {
      if (error != nullptr) {
        *error = "received mismatched telemetry ACK";
      }
      return false;
    }

    if (!telemetry_queue_.Acknowledge(ack.session_id, ack.seq, error)) {
      return false;
    }

    if (link_ok != nullptr) {
      *link_ok = true;
    }
    ++drained;
  }

  return true;
}

std::string SystemController::HandleCommandLine(const std::string& line,
                                                const std::string& peer_ip) {
  if (!peer_ip.empty() && (config_.runtime.bench_mode || !IsLoopbackPeer(peer_ip))) {
    telemetry_client_.ObserveGroundStation(peer_ip,
                                           config_.comms.telemetry_port,
                                           config_.comms.command_port,
                                           1000);
  }

  const CommandParseResult parsed = parser_.ParseLine(line);
  if (!parsed.ok) {
    return Nack("UNKNOWN", parsed.error);
  }

  const Command& command = parsed.command;
  const std::string cmd_name = command.name;

  auto require_debug_arm = [&]() -> bool {
    return config_.runtime.bench_mode && debug_armed_.load();
  };

  auto set_state_override = [&](auto fn) {
    std::lock_guard<std::mutex> lock(overrides_mu_);
    fn();
  };

  auto valid_motor = [&](int motor_id) {
    return motor_id >= 0 && stepper_ != nullptr &&
           static_cast<std::size_t>(motor_id) < stepper_->channel_count();
  };

  auto motor_zeroed = [&](int motor_id) {
    std::lock_guard<std::mutex> lock(sequence_mu_);
    return motor_id >= 0 &&
           static_cast<std::size_t>(motor_id) < motor_zeroed_.size() &&
           motor_zeroed_[static_cast<std::size_t>(motor_id)];
  };

  auto stop_all_sequences = [&]() {
    std::lock_guard<std::mutex> lock(sequence_mu_);
    for (BendSequenceRuntime& runtime : bend_sequences_) {
      runtime.running = false;
      runtime.paused = false;
      runtime.step_queued = false;
      runtime.active_name.clear();
      runtime.step_index = 0;
      runtime.fault.clear();
    }
  };

  switch (command.type) {
    case CommandType::kPing:
      return Ack(cmd_name, "pong");

    case CommandType::kStatus: {
      std::ostringstream status;
      status << "phase=" << ToString(state_manager_.phase())
             << ";mode=" << ToString(mode_.load())
             << ";manual_first=" << (config_.manual.manual_first ? "1" : "0")
             << ";link_seen=" << (link_seen_ ? "1" : "0")
             << ";link_loss_s=" << link_loss_s_
             << ";fallback_active=" << (link_loss_fallback_active_ ? "1" : "0")
             << ";bench_mode=" << (config_.runtime.bench_mode ? "1" : "0")
             << ";debug_armed=" << (debug_armed_.load() ? "1" : "0")
             << ";telemetry_target=" << telemetry_client_.current_host()
             << ";queue_depth=" << telemetry_queue_.size()
             << ";tick_hz=" << live_tick_hz_.load()
             << ";simulated=" << (sensor_manager_.simulated() ? "1" : "0")
             << ";i2c_ok=" << (sensor_manager_.i2c_ok() ? "1" : "0")
             << ";rs485_ok=" << (sensor_manager_.rs485_ok() ? "1" : "0")
             << ";sample_temp_ok=" << (sensor_manager_.sample_temp_ok() ? "1" : "0")
             << ";pwm_ok=" << (pwm_ && pwm_->healthy() ? "1" : "0")
             << ";stepper_ok=" << (stepper_ && stepper_->AllHealthy() ? "1" : "0")
             << ";energy_wh=" << scheduler_.energy_consumed_wh()
             << ";energy_budget_wh=" << config_.power.energy_budget_wh
             << ";budget_exhausted=" << (scheduler_.is_budget_exhausted() ? "1" : "0");
      for (std::size_t motor = 0; motor < bend_sequences_.size(); ++motor) {
        status << ";seq" << motor << "={" << SequenceStatus(static_cast<int>(motor))
               << '}';
      }
      return Ack(cmd_name, status.str());
    }

    case CommandType::kComponents: {
      std::ostringstream result;
      result << sensor_manager_.ComponentSummary();
      if (pwm_ == nullptr || pwm_->channel_count() == 0) {
        result << ";pwm=FAILED";
      } else {
        const std::size_t healthy_channels = pwm_->healthy_channel_count();
        result << ";pwm="
               << (healthy_channels == pwm_->channel_count()
                       ? "OK"
                       : (healthy_channels > 0 ? "DEGRADED" : "FAILED"));
        for (std::size_t channel = 0; channel < pwm_->channel_count();
             ++channel) {
          result << ";heater" << channel << '='
                 << (pwm_->channel_healthy(channel) ? "OK" : "FAILED");
        }
      }
      for (std::size_t motor = 0;
           motor < (stepper_ ? stepper_->channel_count() : 0); ++motor) {
        result << ";motor" << motor << '='
               << (stepper_->Healthy(static_cast<int>(motor))
                       ? "OK"
                       : "FAILED");
      }
      result << ";comms="
             << (telemetry_client_.is_connected() ? "OK" : "DEGRADED");
      return Ack(cmd_name, result.str());
    }

    case CommandType::kCheck: {
      const std::string selected =
          command.args.empty() ? "ALL" : command.args[0];
      std::string storage_details;
      const bool check_storage = selected == "ALL" || selected == "STORAGE";
      const bool check_sensors =
          selected == "ALL" || selected == "DPS310" ||
          selected == "ADS1115" || selected == "DAQ132M" ||
          selected == "RTD_CLICK";
      const bool check_pwm = selected == "ALL" || selected == "PWM";
      const bool check_motor0 = selected == "ALL" || selected == "MOTOR0";
      const bool check_motor1 = selected == "ALL" || selected == "MOTOR1";
      const bool check_comms = selected == "ALL" || selected == "COMMS";
      const bool known = check_storage || check_sensors || check_pwm ||
                         check_motor0 || check_motor1 || check_comms;
      if (!known) return Nack(cmd_name, "unknown component");

      const bool storage_ok =
          !check_storage || storage_manager_.ActiveCheck(&storage_details);
      std::string sensor_details = "sensors=SKIPPED";
      const bool sensor_probe_ok =
          !check_sensors ||
          sensor_manager_.ActiveCheck(selected, &sensor_details);
      const bool pwm_ok =
          !check_pwm || (pwm_ != nullptr && pwm_->healthy());
      const bool motor0_ok =
          !check_motor0 ||
          (stepper_ != nullptr && stepper_->ActiveCheck(0));
      const bool motor1_ok =
          !check_motor1 ||
          (stepper_ != nullptr && stepper_->ActiveCheck(1));
      const bool stepper_ok = motor0_ok && motor1_ok;
      if (check_motor0 || check_motor1) {
        tmc_spi_ok_ = config_.runtime.use_simulated_pwm || stepper_ok;
        spi_.set_healthy(tmc_spi_ok_);
      }
      const bool spi_ok =
          !(check_motor0 || check_motor1) || tmc_spi_ok_;
      const bool sensor_ok = sensor_probe_ok;
      const bool comms_ok =
          !check_comms || telemetry_client_.is_connected();
      const bool overall = storage_ok && sensor_ok && pwm_ok &&
                           stepper_ok && spi_ok && comms_ok;
      std::ostringstream result;
      result << "overall=" << (overall ? "OK" : "FAIL")
             << ";selected=" << selected
             << ';' << (check_storage ? storage_details : "storage=SKIPPED")
             << ';' << sensor_details
             << ";pwm=" << (pwm_ok ? "OK" : "FAIL")
             << ";motor0=" << (motor0_ok ? "OK" : "FAIL")
             << ";motor1=" << (motor1_ok ? "OK" : "FAIL")
             << ";spi=" << (spi_ok ? "OK" : "FAIL")
             << ";comms=" << (comms_ok ? "OK" : "FAIL");
      return Ack(cmd_name, result.str());
    }

    case CommandType::kForceStart:
      state_manager_.SetPhase(MissionPhase::kAscent);
      return Ack(cmd_name, "phase=ASCENT");

    case CommandType::kForceStop:
      state_manager_.SetPhase(MissionPhase::kDescent);
      stop_all_sequences();
      if (stepper_) {
        std::string err;
        for (std::size_t i = 0; i < stepper_->channel_count(); ++i) {
          stepper_->Stop(static_cast<int>(i), &err);
        }
      }
      return Ack(cmd_name, "phase=DESCENT;steppers=stopped");

    case CommandType::kHeatersOff:
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        heater_test_.active = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      return Ack(cmd_name, "all heaters disabled");

    case CommandType::kResetCtrl:
      set_state_override([&]() { state_overrides_.reset_control = true; });
      return Ack(cmd_name, "control loop reset queued");

    case CommandType::kShutdownSafe:
      // Graceful shutdown: heaters off, flush data, then stop the process.
      // Unlike ENTER_SAFE, this actually triggers the STOPPED transition via
      // state_overrides_.shutdown_safe, which StateManager handles.
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        heater_test_.active = false;
        state_overrides_.shutdown_safe = true;
      });
      stop_all_sequences();
      storage_manager_.FlushAndSync();
      return Ack(cmd_name, "shutdown initiated");

    case CommandType::kEnterSafe:
      mode_.store(SystemMode::kSafe);
      stop_all_sequences();
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        heater_test_.active = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      if (stepper_) {
        std::string err;
        for (std::size_t i = 0; i < stepper_->channel_count(); ++i) {
          stepper_->Stop(static_cast<int>(i), &err);
        }
      }
      storage_manager_.SetSafeMode(true);
      storage_manager_.FlushAndSync();
      return Ack(cmd_name, "entered SAFE mode");

    case CommandType::kExitSafe: {
      SystemMode expected = SystemMode::kSafe;
      if (!mode_.compare_exchange_strong(expected, SystemMode::kStandby)) {
        return Nack(cmd_name, "not in SAFE mode");
      }
      set_state_override([&]() { control_overrides_.heaters_off = false; });
      storage_manager_.SetSafeMode(false);
      return Ack(cmd_name, "exited SAFE mode to STANDBY");
    }

    case CommandType::kArm: {
      SystemMode expected = SystemMode::kStandby;
      if (!mode_.compare_exchange_strong(expected, SystemMode::kRun)) {
        return Nack(cmd_name, "ARM requires STANDBY mode");
      }
      return Ack(cmd_name, "mode=RUN;manual_control=1");
    }

    case CommandType::kDisarm: {
      SystemMode expected = SystemMode::kRun;
      if (!mode_.compare_exchange_strong(expected, SystemMode::kStandby)) {
        return Nack(cmd_name, "DISARM requires RUN mode");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        heater_test_.active = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      if (stepper_) {
        std::string err;
        for (std::size_t i = 0; i < stepper_->channel_count(); ++i) {
          stepper_->Stop(static_cast<int>(i), &err);
        }
      }
      return Ack(cmd_name, "mode=STANDBY");
    }

    case CommandType::kArmDebug:
      if (!config_.runtime.bench_mode) {
        return Nack(cmd_name, "bench mode required");
      }
      if (command.args.front() != config_.runtime.debug_arm_code) {
        return Nack(cmd_name, "invalid arm token");
      }
      debug_armed_ = true;
      return Ack(cmd_name, "debug armed");

    case CommandType::kDisarmDebug:
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug not armed");
      }
      debug_armed_ = false;
      set_state_override([&]() { heater_test_.active = false; });
      return Ack(cmd_name, "debug disarmed");

    case CommandType::kSetHeaterDuty: {
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      std::size_t index = 0;
      double duty = 0.0;
      if (!ParseIndex(command.args[0], &index) || !ParseDouble(command.args[1], &duty)) {
        return Nack(cmd_name, "invalid args");
      }
      if (index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "heater index out of range");
      }
      if (duty < 0.0 || duty > 1.0) {
        return Nack(cmd_name, "duty out of range [0,1]");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        control_overrides_.heater_duty_overrides[index] = duty;
        control_overrides_.temp_targets_c[index].reset();
      });
      return Ack(cmd_name, "override applied");
    }

    case CommandType::kSetAllDuty: {
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      double duty = 0.0;
      if (!ParseDouble(command.args[0], &duty)) {
        return Nack(cmd_name, "invalid duty");
      }
      if (duty < 0.0 || duty > 1.0) {
        return Nack(cmd_name, "duty out of range [0,1]");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.all_heaters_override = duty;
        control_overrides_.single_heater_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      return Ack(cmd_name, "global override applied");
    }

    case CommandType::kHeaterTest: {
      if (!require_debug_arm()) {
        return Nack(cmd_name, "bench debug arm required");
      }
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (active_motion_lock_ != nullptr && active_motion_lock_->holder() != -1) {
        return Nack(cmd_name, "blocked during motor motion");
      }
      std::size_t index = 0;
      double duty = 0.0;
      double seconds = 0.0;
      if (!ParseIndex(command.args[0], &index) ||
          !ParseDouble(command.args[1], &duty) ||
          !ParseDouble(command.args[2], &seconds) ||
          index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "invalid heater test args");
      }
      if (duty < 0.0 || duty > config_.heaters.debug_max_duty) {
        return Nack(cmd_name, "duty exceeds heater.debug_max_duty");
      }
      if (seconds <= 0.0 || seconds > config_.heaters.debug_max_seconds) {
        return Nack(cmd_name, "duration exceeds heater.debug_max_seconds");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        heater_test_.active = true;
        heater_test_.heater_index = index;
        heater_test_.duty = duty;
        heater_test_.until = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(seconds));
      });
      std::ostringstream msg;
      msg << "heater=" << index << ";duty=" << duty << ";seconds=" << seconds;
      return Ack(cmd_name, msg.str());
    }

    case CommandType::kSetPid: {
      double kp = 0.0;
      double ki = 0.0;
      double kd = 0.0;
      if (!ParseDouble(command.args[1], &kp) ||
          !ParseDouble(command.args[2], &ki) ||
          !ParseDouble(command.args[3], &kd) ||
          kp < 0.0 || ki < 0.0 || kd < 0.0) {
        return Nack(cmd_name, "invalid pid args");
      }
      const PidGains gains{kp, ki, kd};
      if (command.args[0] == "ALL" || command.args[0] == "all") {
        set_state_override([&]() {
          control_overrides_.pid_override = gains;
          std::fill(control_overrides_.pid_overrides.begin(),
                    control_overrides_.pid_overrides.end(), gains);
        });
      } else {
        std::size_t index = 0;
        if (!ParseIndex(command.args[0], &index) ||
            index >= config_.hardware.heater_count) {
          return Nack(cmd_name, "heater index out of range");
        }
        set_state_override([&]() {
          control_overrides_.pid_overrides[index] = gains;
        });
      }
      return Ack(cmd_name, "pid override applied");
    }

    case CommandType::kSetTempTarget: {
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      std::size_t index = 0;
      double target = 0.0;
      if (!ParseIndex(command.args[0], &index) ||
          !ParseDouble(command.args[1], &target) ||
          index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "invalid target args");
      }
      if (target < config_.heater_safety.target_min_c ||
          target > config_.heater_safety.target_max_c) {
        return Nack(cmd_name, "target outside configured limits");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.all_heaters_override.reset();
        control_overrides_.heater_duty_overrides[index].reset();
        control_overrides_.temp_targets_c[index] = target;
      });
      return Ack(cmd_name, "temperature target applied");
    }

    case CommandType::kSetAllTempTargets: {
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      double target = 0.0;
      if (!ParseDouble(command.args[0], &target) ||
          target < config_.heater_safety.target_min_c ||
          target > config_.heater_safety.target_max_c) {
        return Nack(cmd_name, "target outside configured limits");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.all_heaters_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), target);
      });
      return Ack(cmd_name, "all temperature targets applied");
    }

    case CommandType::kClearTempTarget: {
      std::size_t index = 0;
      if (!ParseIndex(command.args[0], &index) ||
          index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "heater index out of range");
      }
      set_state_override([&]() {
        control_overrides_.temp_targets_c[index].reset();
      });
      return Ack(cmd_name, "temperature target cleared");
    }

    case CommandType::kClearTempTargets:
      set_state_override([&]() {
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      return Ack(cmd_name, "temperature targets cleared");

    case CommandType::kGetThermal: {
      std::lock_guard<std::mutex> lock(overrides_mu_);
      std::ostringstream result;
      result << "target_min_c=" << config_.heater_safety.target_min_c
             << ";target_max_c=" << config_.heater_safety.target_max_c;
      for (std::size_t i = 0; i < config_.hardware.heater_count; ++i) {
        result << ";h" << i << "_target=";
        if (i < control_overrides_.temp_targets_c.size() &&
            control_overrides_.temp_targets_c[i].has_value()) {
          result << control_overrides_.temp_targets_c[i].value();
        } else {
          result << '-';
        }
        result << ";h" << i << "_temp=";
        const std::size_t sample =
            i < config_.heaters.temperature_channels.size()
                ? config_.heaters.temperature_channels[i]
                : i;
        if (sample < last_sensor_snapshot_.sample_temps_c.size() &&
            (last_sensor_snapshot_.sample_temp_valid.empty() ||
             (sample < last_sensor_snapshot_.sample_temp_valid.size() &&
              last_sensor_snapshot_.sample_temp_valid[sample]))) {
          result << last_sensor_snapshot_.sample_temps_c[sample];
        } else {
          result << '-';
        }
        result << ";h" << i << "_duty="
               << (i < last_heater_duty_.size() ? last_heater_duty_[i] : 0.0);
      }
      return Ack(cmd_name, result.str());
    }

    case CommandType::kClearOverrides:
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        std::fill(control_overrides_.heater_duty_overrides.begin(),
                  control_overrides_.heater_duty_overrides.end(), std::nullopt);
        control_overrides_.pid_override.reset();
        std::fill(control_overrides_.pid_overrides.begin(),
                  control_overrides_.pid_overrides.end(), std::nullopt);
        std::fill(control_overrides_.temp_targets_c.begin(),
                  control_overrides_.temp_targets_c.end(), std::nullopt);
      });
      return Ack(cmd_name, "overrides cleared");

    case CommandType::kSetBenchMode: {
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug arm required");
      }
      const std::string mode = command.args[0];
      if (mode == "ON") {
        config_.runtime.bench_mode = true;
      } else if (mode == "OFF") {
        config_.runtime.bench_mode = false;
      } else {
        return Nack(cmd_name, "expected ON or OFF");
      }
      return Ack(cmd_name, "bench mode updated");
    }

    case CommandType::kSetTickHz: {
      // Flight-safe: BEXUS User Manual §5.4 requires the operator to be able
      // to tune the downlink data rate during flight. No debug arm required.
      double hz = 0.0;
      if (!ParseDouble(command.args[0], &hz)) {
        return Nack(cmd_name, "invalid hz value");
      }
      // Clamp to a safe band: 0.1 Hz floor (one frame / 10 s) and 5 Hz ceiling
      // (well below the 2 Mbps E-Link budget at our ~600 B frame size).
      constexpr double kMinHz = 0.1;
      constexpr double kMaxHz = 5.0;
      if (hz < kMinHz || hz > kMaxHz) {
        return Nack(cmd_name, "hz out of range [0.1,5.0]");
      }
      live_tick_hz_.store(hz);
      config_.runtime.tick_hz = hz;
      std::ostringstream msg;
      msg << "tick_hz=" << hz;
      return Ack(cmd_name, msg.str());
    }

    case CommandType::kRadioSilence:
      telemetry_client_.SetTransmitEnabled(false);
      return Ack(cmd_name, "radio silent");

    case CommandType::kRadioResume:
      telemetry_client_.SetTransmitEnabled(true);
      return Ack(cmd_name, "radio resumed");

    case CommandType::kSetPhase: {
      MissionPhase requested = MissionPhase::kBoot;
      if (!ParseMissionPhase(command.args[0], &requested)) {
        return Nack(cmd_name, "invalid phase");
      }
      state_manager_.SetPhase(requested);
      std::ostringstream msg;
      msg << "phase=" << ToString(requested);
      return Ack(cmd_name, msg.str());
    }

    case CommandType::kSetPositionZero: {
      std::size_t motor = 0;
      if (!ParseIndex(command.args[0], &motor) ||
          !valid_motor(static_cast<int>(motor))) {
        return Nack(cmd_name, "invalid motor id");
      }
      std::string error;
      if (!stepper_->SetPositionZero(static_cast<int>(motor), &error)) {
        return Nack(cmd_name, error);
      }
      {
        std::lock_guard<std::mutex> lock(sequence_mu_);
        motor_zeroed_[motor] = true;
      }
      return Ack(cmd_name, "motor=" + std::to_string(motor) + ";zeroed=1");
    }

    case CommandType::kBendSeqLoad: {
      std::size_t motor = 0;
      if (!ParseIndex(command.args[0], &motor) ||
          !valid_motor(static_cast<int>(motor))) {
        return Nack(cmd_name, "invalid motor id");
      }
      const std::string name = command.args[1];
      if (!IsSequenceNameValid(name)) {
        return Nack(cmd_name, "invalid sequence name");
      }
      BendSequenceDefinition definition;
      definition.name = name;
      for (std::size_t i = 2; i < command.args.size(); ++i) {
        std::istringstream spec(command.args[i]);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(spec, field, ':')) fields.push_back(field);
        if (fields.size() < 2 || fields.size() > 3) {
          return Nack(cmd_name, "invalid sequence step");
        }
        BendSequenceStep step;
        if (!ParseInt64(fields[0], &step.target_usteps) ||
            !ParseDouble(fields[1], &step.hold_s) ||
            std::abs(step.target_usteps) > config_.stepper.max_position_steps ||
            step.hold_s < 0.0 || step.hold_s > 86400.0) {
          return Nack(cmd_name, "invalid sequence target/hold");
        }
        if (fields.size() == 3) {
          double speed = 0.0;
          if (!ParseDouble(fields[2], &speed) || speed <= 0.0 ||
              speed > config_.pull.max_step_hz) {
            return Nack(cmd_name, "invalid sequence speed");
          }
          step.speed_hz = speed;
        }
        definition.steps.push_back(step);
      }
      {
        std::lock_guard<std::mutex> lock(sequence_mu_);
        BendSequenceRuntime& runtime = bend_sequences_[motor];
        if (runtime.running && runtime.active_name == name) {
          return Nack(cmd_name, "cannot replace active sequence");
        }
        runtime.definitions[name] = std::move(definition);
      }
      return Ack(cmd_name, "sequence loaded");
    }

    case CommandType::kBendSeqRun: {
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "cannot start sequence during link-loss fallback");
      }
      std::size_t motor = 0;
      if (!ParseIndex(command.args[0], &motor) ||
          !valid_motor(static_cast<int>(motor))) {
        return Nack(cmd_name, "invalid motor id");
      }
      if (!motor_zeroed(static_cast<int>(motor))) {
        return Nack(cmd_name, "motor must be zeroed first");
      }
      if (!stepper_->Snapshot(static_cast<int>(motor)).enabled) {
        return Nack(cmd_name, "motor must be enabled first");
      }
      std::lock_guard<std::mutex> lock(sequence_mu_);
      BendSequenceRuntime& runtime = bend_sequences_[motor];
      const auto it = runtime.definitions.find(command.args[1]);
      if (it == runtime.definitions.end()) {
        return Nack(cmd_name, "unknown sequence");
      }
      if (runtime.running) {
        return Nack(cmd_name, "sequence already running");
      }
      runtime.active_name = it->first;
      runtime.step_index = 0;
      runtime.running = true;
      runtime.paused = false;
      runtime.step_queued = false;
      runtime.fault.clear();
      return Ack(cmd_name, "sequence started");
    }

    case CommandType::kBendSeqPause:
    case CommandType::kBendSeqResume:
    case CommandType::kBendSeqStop:
    case CommandType::kBendSeqStatus:
    case CommandType::kBendSeqClear: {
      std::size_t motor = 0;
      if (!ParseIndex(command.args[0], &motor) ||
          !valid_motor(static_cast<int>(motor))) {
        return Nack(cmd_name, "invalid motor id");
      }
      if (command.type == CommandType::kBendSeqStatus) {
        return Ack(cmd_name, SequenceStatus(static_cast<int>(motor)));
      }
      if (command.type == CommandType::kBendSeqPause) {
        std::lock_guard<std::mutex> lock(sequence_mu_);
        BendSequenceRuntime& runtime = bend_sequences_[motor];
        if (!runtime.running) return Nack(cmd_name, "no sequence running");
        runtime.paused = true;
        runtime.step_queued = false;
        runtime.fault = "operator pause";
        std::string error;
        stepper_->Stop(static_cast<int>(motor), &error);
        return Ack(cmd_name, "sequence paused");
      }
      if (command.type == CommandType::kBendSeqResume) {
        if (mode_.load() != SystemMode::kRun) {
          return Nack(cmd_name, "RUN mode required");
        }
        if (!motor_zeroed(static_cast<int>(motor))) {
          return Nack(cmd_name, "motor must be zeroed first");
        }
        std::lock_guard<std::mutex> lock(sequence_mu_);
        BendSequenceRuntime& runtime = bend_sequences_[motor];
        if (!runtime.running || !runtime.paused) {
          return Nack(cmd_name, "sequence is not paused");
        }
        runtime.paused = false;
        runtime.step_queued = false;
        runtime.fault.clear();
        return Ack(cmd_name, "sequence resumed");
      }
      if (command.type == CommandType::kBendSeqStop) {
        {
          std::lock_guard<std::mutex> lock(sequence_mu_);
          BendSequenceRuntime& runtime = bend_sequences_[motor];
          runtime.running = false;
          runtime.paused = false;
          runtime.step_queued = false;
          runtime.active_name.clear();
          runtime.step_index = 0;
          runtime.fault.clear();
        }
        std::string error;
        stepper_->Stop(static_cast<int>(motor), &error);
        return Ack(cmd_name, "sequence stopped");
      }
      std::lock_guard<std::mutex> lock(sequence_mu_);
      BendSequenceRuntime& runtime = bend_sequences_[motor];
      if (command.args.size() == 1) {
        if (runtime.running) return Nack(cmd_name, "sequence running");
        runtime.definitions.clear();
      } else {
        if (runtime.running && runtime.active_name == command.args[1]) {
          return Nack(cmd_name, "cannot clear active sequence");
        }
        if (runtime.definitions.erase(command.args[1]) == 0U) {
          return Nack(cmd_name, "unknown sequence");
        }
      }
      return Ack(cmd_name, "sequence definitions cleared");
    }

    case CommandType::kStepperMove: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      std::int64_t steps = 0;
      try {
        steps = std::stoll(command.args[0]);
      } catch (...) {
        return Nack(cmd_name, "invalid steps");
      }
      std::string err;
      InhibitHeatersForMotion();
      if (!stepper_->MoveSteps(command.motor_id, steps, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "move queued");
    }

    case CommandType::kStepperMoveTo:
    case CommandType::kStepperBend: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (!motor_zeroed(command.motor_id)) {
        return Nack(cmd_name, "motor must be zeroed first");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      std::int64_t steps = 0;
      double hold_s = 0.0;
      try {
        steps = std::stoll(command.args[0]);
        if (command.args.size() == 2) {
          hold_s = std::stod(command.args[1]);
        }
      } catch (...) {
        return Nack(cmd_name, "invalid args");
      }
      std::string err;
      InhibitHeatersForMotion();
      if (!stepper_->MoveToSteps(command.motor_id, steps, hold_s, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "bend queued");
    }

    case CommandType::kStepperRotate: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      double revs = 0.0;
      if (!ParseDouble(command.args[0], &revs)) return Nack(cmd_name, "invalid revs");
      std::string err;
      InhibitHeatersForMotion();
      if (!stepper_->Rotate(command.motor_id, revs, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "rotate queued");
    }

    case CommandType::kStepperHome: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (!motor_zeroed(command.motor_id)) {
        return Nack(cmd_name, "motor must be zeroed first");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      std::string err;
      InhibitHeatersForMotion();
      if (!stepper_->Home(command.motor_id, &err)) return Nack(cmd_name, err);
      return Ack(cmd_name, "homing");
    }

    case CommandType::kStepperStop: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::string err;
      stepper_->Stop(command.motor_id, &err);
      return Ack(cmd_name, "stopped");
    }

    case CommandType::kStepperSetSpeed: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      double hz = 0.0;
      if (!ParseDouble(command.args[0], &hz)) return Nack(cmd_name, "invalid hz");
      std::string err;
      if (!stepper_->SetSpeed(command.motor_id, hz, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "speed updated");
    }

    case CommandType::kStepperSetMicrostep: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::size_t idx = 0;
      if (!ParseIndex(command.args[0], &idx)) return Nack(cmd_name, "invalid divisor");
      std::string err;
      if (!stepper_->SetMicrostep(command.motor_id, static_cast<int>(idx), &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "microstep updated");
    }

    case CommandType::kStepperEnable: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      std::string err;
      if (!stepper_->SetEnabled(command.motor_id, true, &err)) {
        return Nack(cmd_name, err.empty() ? "enable failed" : err);
      }
      return Ack(cmd_name, "stepper enabled");
    }

    case CommandType::kStepperDisable: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::string err;
      if (!stepper_->SetEnabled(command.motor_id, false, &err)) {
        return Nack(cmd_name, err.empty() ? "disable failed" : err);
      }
      return Ack(cmd_name, "stepper disabled");
    }

    case CommandType::kPullArm: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (!motor_zeroed(command.motor_id)) {
        return Nack(cmd_name, "motor must be zeroed first");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      std::string err;
      InhibitHeatersForMotion();
      if (!stepper_->ArmPull(command.motor_id, &err)) return Nack(cmd_name, err);
      return Ack(cmd_name, "pull armed");
    }

    case CommandType::kPullExecute: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      if (mode_.load() != SystemMode::kRun) {
        return Nack(cmd_name, "RUN mode required");
      }
      if (!motor_zeroed(command.motor_id)) {
        return Nack(cmd_name, "motor must be zeroed first");
      }
      if (link_loss_fallback_active_) {
        return Nack(cmd_name, "manual motion blocked during link-loss fallback");
      }
      std::string err;
      // Routing fix (Agent C, 2026-04-17): use the non-blocking ArmPull
      // path and let the main loop's Tick() drive the pull to completion.
      // The previous `ExecutePull` helper pumped Tick() synchronously here,
      // which meant that by the time we ACK'd, the channel was already
      // back to `moving=false` — so the `was_moving && !moving` edge
      // detector in the telemetry emitter (see further down in this file)
      // never fired and EVT,PULL frames were never emitted. The MotionLock
      // + heater interlock also barely had time to engage. With arm-only
      // dispatch, the next tick sees `moving=true`, subsequent ticks hold
      // HEATER_INHIBITED, and the falling edge publishes EVT,PULL.
      InhibitHeatersForMotion();
      if (!stepper_->ArmPull(command.motor_id, &err)) return Nack(cmd_name, err);
      return Ack(cmd_name, "pull executed");
    }

    case CommandType::kUnknown:
      return Nack(cmd_name, "unknown command");
  }

  return Nack(cmd_name, "unhandled command");
}

}  // namespace coatheal
