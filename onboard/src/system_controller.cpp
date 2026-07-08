#include "coatheal/system_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>

#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/fatigue_sequencer.hpp"
#include "coatheal/sd_notify.hpp"
#include "coatheal/stepper_channel.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/tmc5160_driver.hpp"

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
}

bool SystemController::Initialize(std::string* error) {
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

  // Final BOM: two TMC5160-driven NEMA 17 ball-screw actuators. Motor channel,
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
    channel_cfgs.push_back(std::move(cfg));
  }

  std::vector<std::unique_ptr<StepperDriver>> drivers;
  if (config_.runtime.use_simulated_pwm) {
    for (std::size_t i = 0; i < config_.motors.size(); ++i) {
      drivers.emplace_back(std::make_unique<SimulatedStepperDriver>());
    }
  } else {
    // If SPI bring-up fails, keep the process alive with a bare STEP/DIR/EN
    // fallback so operators still get diagnostics.
    auto build_tmc_or_fallback =
        [&](const char* motor_label, const MotorConfig& motor)
            -> std::unique_ptr<StepperDriver> {
      Tmc5160Config tcfg;
      tcfg.spi_device = motor.spi_device;
      tcfg.cs_line = motor.cs_line;
      tcfg.step_line = motor.step_line;
      tcfg.dir_line = motor.dir_line;
      tcfg.enable_line = motor.enable_line;
      tcfg.invert_direction = motor.invert_direction;
      tcfg.enable_active_low = motor.enable_active_low;
      tcfg.microstep = config_.pull.microstep;
      tcfg.run_current_a_rms = motor.run_current_a_rms;
      tcfg.hold_current_frac = motor.hold_current_frac;
      tcfg.stealth_chop = motor.stealth_chop;
      tcfg.spi_speed_hz = motor.spi_speed_hz;
      auto tmc = std::make_unique<Tmc5160Driver>(tcfg);
      if (tmc->healthy()) {
        return tmc;
      }
      std::cerr << "[system] " << motor_label
                << ": TMC5160 SPI bring-up on " << motor.spi_device
                << " failed; falling back to bare STEP/DIR/EN driver."
                << " Motor will run without SPI current control." << '\n';
      return std::make_unique<GpioStepDirStepperDriver>(
          config_.runtime.gpio_chip, motor.step_line, motor.dir_line,
          motor.enable_line, motor.invert_direction, motor.enable_active_low);
    };
    drivers.emplace_back(build_tmc_or_fallback(
        "motor0", config_.motors[0]));
    drivers.emplace_back(build_tmc_or_fallback(
        "motor1", config_.motors[1]));
  }

  stepper_ = std::make_unique<StepperController>(
      std::move(channel_cfgs), std::move(drivers), config_.bend);

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

  // Rev C: create the fatigue sequencer for PRE_FLOAT phase.
  fatigue_sequencer_ = std::make_unique<FatigueSequencer>(
      config_.fatigue, stepper_.get());

  if (!storage_manager_.Initialize(error)) {
    return false;
  }

  if (!telemetry_queue_.Initialize(error)) {
    return false;
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
      state_overrides_.secondary_cycle = false;
      state_overrides_.fatigue_complete = false;
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
    const bool legacy_autonomous_run =
        !config_.manual.manual_first && current_mode == SystemMode::kRun;
    const bool fallback_floor_control =
        link_loss_fallback_active_;
    const bool automatic_phase_tracking =
        legacy_autonomous_run || link_loss_fallback_active_;
    const bool automatic_experiment_actions =
        legacy_autonomous_run;
    COATHEAL_PERF_STAMP(perf_ts[1]);  // stage 0: snapshot overrides + mode load

    SensorSnapshot snapshot = sensor_manager_.ReadSnapshot(
        state_manager_.phase(), last_heater_duty_, tick_duration.count());
    COATHEAL_PERF_STAMP(perf_ts[2]);  // stage 1: sensor snapshot

    MissionPhase phase = state_manager_.phase();
    if (automatic_phase_tracking) {
      phase = state_manager_.Update(
          snapshot.ambient_pressure_mbar, snapshot.sample_temps_c, state_overrides,
          std::chrono::steady_clock::now());
    }

    // Manual-first Rev C: connected operation must not start the irreversible
    // fatigue sequence automatically. It remains available only for legacy
    // autonomous builds (`manual.manual_first=false`); flight operators use
    // PULL_* commands explicitly.
    if (automatic_experiment_actions &&
        phase == MissionPhase::kPreFloat && fatigue_sequencer_) {
      if (fatigue_sequencer_->Tick(tick_duration.count())) {
        std::lock_guard<std::mutex> lock(overrides_mu_);
        state_overrides_.fatigue_complete = true;
      }
    }

    const bool heaters_allowed = (current_mode == SystemMode::kRun);
    ControlOverrides effective_control = control_overrides;
    effective_control.floor_control_enabled =
        legacy_autonomous_run || fallback_floor_control;
    if (!heaters_allowed) {
      effective_control.heaters_off = true;
    }
    COATHEAL_PERF_STAMP(perf_ts[3]);  // stage 2: phase update + effective ovr

    std::vector<double> requested_duty = thermal_controller_.ComputeRequestedDuty(
        phase, snapshot, tick_duration.count(), effective_control);
    COATHEAL_PERF_STAMP(perf_ts[4]);  // stage 3: thermal controller

    // Heaters are only scheduled during active flight phases. Manual duty
    // overrides can request heat, but safety limits still gate output.
    const bool any_flying_phase =
        (phase == MissionPhase::kAscent || phase == MissionPhase::kPreFloat ||
         phase == MissionPhase::kFloat || phase == MissionPhase::kDescent);
    const std::vector<double> scheduled_duty = scheduler_.Schedule(
        requested_duty, heaters_allowed && any_flying_phase,
        tick_duration.count());
    COATHEAL_PERF_STAMP(perf_ts[5]);  // stage 4: heater scheduler

    for (std::size_t i = 0; i < scheduled_duty.size(); ++i) {
      pwm_->SetDuty(i, scheduled_duty[i]);
    }
    last_heater_duty_ = scheduled_duty;

    if (stepper_) {
      stepper_->Tick(phase, tick_duration.count(),
                     automatic_experiment_actions);
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
    record.status.i2c_ok = i2c_.healthy();
    record.status.link_ok = last_link_ok;
    record.status.t_ambient_ok = sensor_manager_.t_ambient_ok();
    record.status.p_ambient_ok = sensor_manager_.p_ambient_ok();
    record.status.overtemp_ok = !thermal_controller_.overtemp_latched();
    record.status.uniformity_ok = thermal_controller_.uniformity_ok();
    record.status.energy_ok = !scheduler_.is_budget_exhausted();
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
             << ";energy_wh=" << scheduler_.energy_consumed_wh()
             << ";energy_budget_wh=" << config_.power.energy_budget_wh
             << ";budget_exhausted=" << (scheduler_.is_budget_exhausted() ? "1" : "0");
      return Ack(cmd_name, status.str());
    }

    case CommandType::kForceStart:
      if (config_.manual.manual_first) {
        state_manager_.SetPhase(MissionPhase::kAscent);
        if (fatigue_sequencer_) fatigue_sequencer_->Reset();
        return Ack(cmd_name, "phase=ASCENT");
      }
      set_state_override([&]() { state_overrides_.force_start = true; });
      return Ack(cmd_name, "override accepted");

    case CommandType::kForceStop:
      if (config_.manual.manual_first) {
        state_manager_.SetPhase(MissionPhase::kDescent);
        if (stepper_) {
          std::string err;
          for (std::size_t i = 0; i < stepper_->channel_count(); ++i) {
            stepper_->Stop(static_cast<int>(i), &err);
          }
        }
        return Ack(cmd_name, "phase=DESCENT;steppers=stopped");
      }
      set_state_override([&]() { state_overrides_.force_stop = true; });
      return Ack(cmd_name, "override accepted");

    case CommandType::kHeatersOff:
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
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
        state_overrides_.shutdown_safe = true;
      });
      storage_manager_.FlushAndSync();
      return Ack(cmd_name, "shutdown initiated");

    case CommandType::kEnterSafe:
      mode_.store(SystemMode::kSafe);
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
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
      return Ack(cmd_name,
                 config_.manual.manual_first ? "mode=RUN;manual_control=1"
                                             : "mode=RUN;autonomous=1");
    }

    case CommandType::kDisarm: {
      SystemMode expected = SystemMode::kRun;
      if (!mode_.compare_exchange_strong(expected, SystemMode::kStandby)) {
        return Nack(cmd_name, "DISARM requires RUN mode");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = true;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
      });
      if (stepper_) {
        std::string err;
        for (std::size_t i = 0; i < stepper_->channel_count(); ++i) {
          stepper_->Stop(static_cast<int>(i), &err);
        }
      }
      return Ack(cmd_name, "mode=STANDBY");
    }

    case CommandType::kSecondaryCycle:
      set_state_override([&]() { state_overrides_.secondary_cycle = true; });
      return Ack(cmd_name, "secondary heating cycle requested");

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
      return Ack(cmd_name, "debug disarmed");

    case CommandType::kSetHeaterDuty: {
      std::size_t index = 0;
      double duty = 0.0;
      if (!ParseIndex(command.args[0], &index) || !ParseDouble(command.args[1], &duty)) {
        return Nack(cmd_name, "invalid args");
      }
      if (index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "heater index out of range");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.single_heater_override = {index, std::clamp(duty, 0.0, 1.0)};
      });
      return Ack(cmd_name, "override applied");
    }

    case CommandType::kSetAllDuty: {
      double duty = 0.0;
      if (!ParseDouble(command.args[0], &duty)) {
        return Nack(cmd_name, "invalid duty");
      }
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.all_heaters_override = std::clamp(duty, 0.0, 1.0);
      });
      return Ack(cmd_name, "global override applied");
    }

    case CommandType::kSetPid: {
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug arm required");
      }
      double kp = 0.0;
      double ki = 0.0;
      double kd = 0.0;
      if (!ParseDouble(command.args[0], &kp) || !ParseDouble(command.args[1], &ki) ||
          !ParseDouble(command.args[2], &kd)) {
        return Nack(cmd_name, "invalid pid args");
      }
      set_state_override([&]() { control_overrides_.pid_override = PidGains{kp, ki, kd}; });
      return Ack(cmd_name, "pid override applied");
    }

    case CommandType::kClearOverrides:
      set_state_override([&]() {
        control_overrides_.heaters_off = false;
        control_overrides_.single_heater_override.reset();
        control_overrides_.all_heaters_override.reset();
        control_overrides_.pid_override.reset();
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
      if (fatigue_sequencer_) fatigue_sequencer_->Reset();
      std::ostringstream msg;
      msg << "phase=" << ToString(requested);
      return Ack(cmd_name, msg.str());
    }

    case CommandType::kStepperMove: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::int64_t steps = 0;
      try {
        steps = std::stoll(command.args[0]);
      } catch (...) {
        return Nack(cmd_name, "invalid steps");
      }
      std::string err;
      if (!stepper_->MoveSteps(command.motor_id, steps, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "move queued");
    }

    case CommandType::kStepperMoveTo:
    case CommandType::kStepperBend: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
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
      if (!stepper_->MoveToSteps(command.motor_id, steps, hold_s, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "bend queued");
    }

    case CommandType::kStepperRotate: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      double revs = 0.0;
      if (!ParseDouble(command.args[0], &revs)) return Nack(cmd_name, "invalid revs");
      std::string err;
      if (!stepper_->Rotate(command.motor_id, revs, &err))
        return Nack(cmd_name, err);
      return Ack(cmd_name, "rotate queued");
    }

    case CommandType::kStepperHome: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::string err;
      stepper_->Home(command.motor_id, &err);
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
      std::string err;
      stepper_->SetEnabled(command.motor_id, true, &err);
      return Ack(cmd_name, "stepper enabled");
    }

    case CommandType::kStepperDisable: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::string err;
      stepper_->SetEnabled(command.motor_id, false, &err);
      return Ack(cmd_name, "stepper disabled");
    }

    case CommandType::kPullArm: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
      std::string err;
      if (!stepper_->ArmPull(command.motor_id, &err)) return Nack(cmd_name, err);
      return Ack(cmd_name, "pull armed");
    }

    case CommandType::kPullExecute: {
      if (!stepper_) return Nack(cmd_name, "stepper unavailable");
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
      if (!stepper_->ArmPull(command.motor_id, &err)) return Nack(cmd_name, err);
      return Ack(cmd_name, "pull executed");
    }

    case CommandType::kUnknown:
      return Nack(cmd_name, "unknown command");
  }

  return Nack(cmd_name, "unhandled command");
}

}  // namespace coatheal
