#include "coatheal/system_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "coatheal/telemetry.hpp"

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

}  // namespace

SystemController::SystemController(OnboardConfig config)
    : config_(std::move(config)),
      command_server_(config_.comms.command_port),
      sensor_manager_(config_, &spi_, &i2c_, &rtc_),
      state_manager_(config_),
      thermal_controller_(config_),
      scheduler_(config_.power, config_.hardware.electronics_heater_index),
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
                        config_.comms.static_pi_ip),
      last_heater_duty_(config_.hardware.heater_count, 0.0) {}

bool SystemController::Initialize(std::string* error) {
  if (config_.runtime.use_simulated_pwm) {
    pwm_ = std::make_unique<SimulatedPwmController>(config_.hardware.heater_count);
  } else {
    pwm_ = std::make_unique<LibgpiodPwmController>(
        config_.runtime.gpio_chip, config_.hardware.heater_count);
  }

  if (!storage_manager_.Initialize(error)) {
    return false;
  }

  if (!telemetry_queue_.Initialize(error)) {
    return false;
  }

  if (!command_server_.Start(
          [this](const std::string& line) { return HandleCommandLine(line); }, error)) {
    return false;
  }

  return true;
}

int SystemController::Run() {
  const double tick_hz = std::max(0.1, config_.runtime.tick_hz);
  const auto tick_duration = std::chrono::duration<double>(1.0 / tick_hz);
  bool last_link_ok = false;

  while (running_) {
    const auto tick_start = std::chrono::steady_clock::now();

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

    SensorSnapshot snapshot = sensor_manager_.ReadSnapshot(
        state_manager_.phase(), last_heater_duty_, tick_duration.count());

    const MissionPhase phase = state_manager_.Update(
        snapshot.ambient_pressure_mbar, snapshot.sample_temps_c, state_overrides,
        std::chrono::steady_clock::now());

    std::vector<double> requested_duty = thermal_controller_.ComputeRequestedDuty(
        phase, snapshot, tick_duration.count(), control_overrides);

    const std::vector<double> scheduled_duty = scheduler_.Schedule(
        requested_duty, phase == MissionPhase::kActivationRamp);

    for (std::size_t i = 0; i < scheduled_duty.size(); ++i) {
      pwm_->SetDuty(i, scheduled_duty[i]);
    }
    last_heater_duty_ = scheduled_duty;

    TelemetryRecord record;
    record.seq = seq_++;
    record.phase = phase;
    record.sensors = snapshot;
    record.heater_duty = scheduled_duty;
    record.status = storage_manager_.status();
    record.status.spi_ok = spi_.healthy();
    record.status.i2c_ok = i2c_.healthy();
    record.status.link_ok = last_link_ok;

    const std::string line = SerializeTelemetryDataFrame(record, telemetry_client_.session_id());
    storage_manager_.WriteLine(line);

    std::string queue_error;
    QueuedTelemetryFrame queued_frame;
    queued_frame.queued_epoch_s = CurrentUnixEpochSeconds();
    queued_frame.session_id = telemetry_client_.session_id();
    queued_frame.seq = record.seq;
    queued_frame.frame = line;

    if (!telemetry_queue_.Enqueue(queued_frame, &queue_error)) {
      last_link_ok = false;
    }

    std::string drain_error;
    if (!DrainTelemetryQueue(&last_link_ok, &drain_error)) {
      last_link_ok = false;
      std::cerr << "[telemetry] drain error: " << drain_error << '\n';
    }

    if (phase == MissionPhase::kStopped) {
      running_ = false;
    }

    const auto elapsed = std::chrono::steady_clock::now() - tick_start;
    if (elapsed < tick_duration) {
      std::this_thread::sleep_for(tick_duration - elapsed);
    }
  }

  command_server_.Stop();
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

  for (const QueuedTelemetryFrame& frame : pending) {
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
  }

  return true;
}

std::string SystemController::HandleCommandLine(const std::string& line) {
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

    case CommandType::kStatus:
      return Ack(cmd_name,
                 "phase=" + ToString(state_manager_.phase()) +
                     ";bench_mode=" + (config_.runtime.bench_mode ? "1" : "0") +
                     ";debug_armed=" + (debug_armed_.load() ? "1" : "0") +
                     ";telemetry_target=" + telemetry_client_.current_host() +
                     ";queue_depth=" + std::to_string(telemetry_queue_.size()));

    case CommandType::kForceStart:
      set_state_override([&]() { state_overrides_.force_start = true; });
      return Ack(cmd_name, "override accepted");

    case CommandType::kForceStop:
      set_state_override([&]() { state_overrides_.force_stop = true; });
      return Ack(cmd_name, "override accepted");

    case CommandType::kHeatersOff:
      set_state_override([&]() { control_overrides_.heaters_off = true; });
      return Ack(cmd_name, "all heaters disabled");

    case CommandType::kResetCtrl:
      set_state_override([&]() { state_overrides_.reset_control = true; });
      return Ack(cmd_name, "control loop reset queued");

    case CommandType::kShutdownSafe:
      set_state_override([&]() { state_overrides_.shutdown_safe = true; });
      running_ = false;
      return Ack(cmd_name, "safe shutdown queued");

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
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug arm required");
      }
      std::size_t index = 0;
      double duty = 0.0;
      if (!ParseIndex(command.args[0], &index) || !ParseDouble(command.args[1], &duty)) {
        return Nack(cmd_name, "invalid args");
      }
      if (index >= config_.hardware.heater_count) {
        return Nack(cmd_name, "heater index out of range");
      }
      set_state_override([&]() {
        control_overrides_.single_heater_override = {index, std::clamp(duty, 0.0, 1.0)};
      });
      return Ack(cmd_name, "override applied");
    }

    case CommandType::kSetAllDuty: {
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug arm required");
      }
      double duty = 0.0;
      if (!ParseDouble(command.args[0], &duty)) {
        return Nack(cmd_name, "invalid duty");
      }
      set_state_override([&]() {
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
      if (!require_debug_arm()) {
        return Nack(cmd_name, "debug arm required");
      }
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

    case CommandType::kUnknown:
      return Nack(cmd_name, "unknown command");
  }

  return Nack(cmd_name, "unhandled command");
}

}  // namespace coatheal
