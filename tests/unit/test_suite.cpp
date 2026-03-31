#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "coatheal/command_parser.hpp"
#include "coatheal/config.hpp"
#include "coatheal/heater_scheduler.hpp"
#include "coatheal/pid_controller.hpp"
#include "coatheal/state_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/telemetry_queue.hpp"

namespace {

void TestPidBoundsAndAntiWindup() {
  coatheal::PidController pid({0.4, 0.2, 0.0}, 0.0, 1.0, -0.5, 0.5);

  for (int i = 0; i < 100; ++i) {
    const double out = pid.Update(100.0, -100.0, 0.1);
    assert(out <= 1.0 + 1e-9);
    assert(out >= -1e-9);
  }

  pid.Reset();
  const double settle = pid.Update(0.0, 0.0, 0.1);
  assert(std::fabs(settle) < 1e-6);
}

void TestHeaterSchedulerCap() {
  coatheal::PowerConfig power;
  power.max_active_heaters = 4;
  power.heater_nominal_w = 10.0;
  power.max_thermal_w = 40.0;

  coatheal::HeaterScheduler scheduler(power, 9);
  std::vector<double> requested(10, 1.0);

  const std::vector<double> scheduled = scheduler.Schedule(requested, true);
  int active = 0;
  double power_sum = 0.0;
  for (double duty : scheduled) {
    if (duty > 1e-6) {
      ++active;
    }
    power_sum += duty * power.heater_nominal_w;
  }

  assert(active <= 4);
  assert(power_sum <= 40.0 + 1e-6);
}

void TestCommandParser() {
  coatheal::CommandParser parser;

  auto ping = parser.ParseLine("PING");
  assert(ping.ok);
  assert(ping.command.type == coatheal::CommandType::kPing);

  auto alias = parser.ParseLine("ON");
  assert(alias.ok);
  assert(alias.command.type == coatheal::CommandType::kForceStart);

  auto invalid = parser.ParseLine("SET_PID 1 2");
  assert(!invalid.ok);
}

void TestTelemetrySerializer() {
  coatheal::TelemetryRecord record;
  record.seq = 42;
  record.phase = coatheal::MissionPhase::kFloatHold;
  record.sensors.timestamp_utc = "2026-03-31T12:00:00Z";
  record.sensors.rtc_valid = true;
  record.sensors.ambient_temp_c = -40.0;
  record.sensors.ambient_pressure_mbar = 120.0;
  record.sensors.ambient_humidity_pct = 15.0;
  record.sensors.uv = 1.2;
  record.sensors.box_temp_c = 5.5;
  record.sensors.sample_temps_c = {70.0, 69.5, 70.2};
  record.heater_duty = {0.1, 0.2, 0.3};

  const std::string frame = coatheal::SerializeTelemetryDataFrame(record, "session-abc");
  assert(frame.rfind("DATA,session-abc,42,", 0) == 0);
  assert(frame.find("HEATER_DUTY=") != std::string::npos);
  assert(frame.find("STATUS=") != std::string::npos);
}

void TestTelemetryQueuePersistenceAndAck() {
  const std::filesystem::path queue_dir =
      std::filesystem::temp_directory_path() /
      ("coatheal_queue_test_" + std::to_string(coatheal::CurrentUnixEpochSeconds()));

  std::string error;
  {
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));

    coatheal::QueuedTelemetryFrame f1;
    f1.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
    f1.session_id = "s1";
    f1.seq = 1;
    f1.frame = "DATA,s1,1,2026-01-01T00:00:01Z,1,0,0,0,0,0,HEATER_DUTY=0.0,PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK";

    coatheal::QueuedTelemetryFrame f2 = f1;
    f2.seq = 2;
    f2.frame = "DATA,s1,2,2026-01-01T00:00:02Z,1,0,0,0,0,0,HEATER_DUTY=0.0,PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK";

    assert(queue.Enqueue(f1, &error));
    assert(queue.Enqueue(f2, &error));
    assert(queue.size() == 2);
    assert(queue.Acknowledge("s1", 1, &error));
    assert(queue.size() == 1);
  }

  {
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));
    const auto pending = queue.PendingFrames();
    assert(pending.size() == 1);
    assert(pending.front().seq == 2);
  }

  std::error_code ec;
  std::filesystem::remove_all(queue_dir, ec);
}

void TestConfigParsesReliabilityFields() {
  const std::filesystem::path cfg_path =
      std::filesystem::temp_directory_path() /
      ("coatheal_cfg_test_" + std::to_string(coatheal::CurrentUnixEpochSeconds()) + ".ini");

  std::ofstream out(cfg_path);
  out << "runtime.tick_hz=1.0\n";
  out << "runtime.bench_mode=false\n";
  out << "runtime.debug_arm_code=COATHEAL_DEBUG\n";
  out << "runtime.use_simulated_pwm=false\n";
  out << "runtime.gpio_chip=/dev/gpiochip0\n";
  out << "comms.telemetry_host=192.168.50.1\n";
  out << "comms.static_ground_ip=192.168.50.1\n";
  out << "comms.static_pi_ip=192.168.50.2\n";
  out << "comms.telemetry_port=4000\n";
  out << "comms.command_port=5000\n";
  out << "comms.reconnect_ms=2000\n";
  out << "comms.discovery_enabled=true\n";
  out << "comms.discovery_port=4100\n";
  out << "storage.primary_log_path=logs/a.csv\n";
  out << "storage.secondary_log_path=logs/b.csv\n";
  out << "storage.queue_dir=logs/q\n";
  out << "storage.queue_retention_hours=72\n";
  out << "storage.queue_max_bytes=1024\n";
  out << "phase.ascent_target_c=-30\n";
  out << "phase.activation_target_c=70\n";
  out << "phase.float_target_c=70\n";
  out << "phase.descent_floor_c=-20\n";
  out << "phase.activation_ramp_c_per_s=0.85\n";
  out << "phase.float_hold_minutes=90\n";
  out << "transition.ascent_to_activation_mbar=140\n";
  out << "transition.float_to_descent_mbar=300\n";
  out << "power.max_active_heaters=4\n";
  out << "power.max_thermal_w=40\n";
  out << "power.max_system_w=48.23\n";
  out << "power.heater_nominal_w=10\n";
  out << "pid.kp=0.2\n";
  out << "pid.ki=0.02\n";
  out << "pid.kd=0.03\n";
  out << "pid.box_kp=0.15\n";
  out << "pid.box_ki=0.01\n";
  out << "pid.box_kd=0.02\n";
  out << "hardware.heater_count=10\n";
  out << "hardware.electronics_heater_index=9\n";
  out.close();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(cfg_path.string(), &cfg, &error));
  assert(cfg.comms.discovery_enabled);
  assert(cfg.storage.queue_max_bytes == 1024U);
  assert(!cfg.runtime.use_simulated_pwm);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestStateTransitions() {
  coatheal::OnboardConfig config;
  config.transition.ascent_to_activation_mbar = 200.0;
  config.transition.float_to_descent_mbar = 350.0;
  config.phase.float_hold_minutes = 0.0;

  coatheal::StateManager sm(config);
  std::vector<double> temps(10, -20.0);

  auto phase = sm.Update(150.0, temps, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kActivationRamp);

  temps.assign(10, 70.0);
  phase = sm.Update(150.0, temps, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kFloatHold);

  phase = sm.Update(400.0, temps, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kDescentFloor);
}

}  // namespace

int main() {
  TestPidBoundsAndAntiWindup();
  TestHeaterSchedulerCap();
  TestCommandParser();
  TestTelemetrySerializer();
  TestTelemetryQueuePersistenceAndAck();
  TestConfigParsesReliabilityFields();
  TestStateTransitions();

  std::cout << "All unit tests passed.\n";
  return 0;
}
