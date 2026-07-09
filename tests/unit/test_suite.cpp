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
#include "coatheal/sensor_manager.hpp"
#include "coatheal/state_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_queue.hpp"
#include "coatheal/thermal_controller.hpp"
#include "coatheal/tmc2240_driver.hpp"

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

void TestTmc2240CurrentConfiguration() {
  coatheal::Tmc2240CurrentSettings settings;
  assert(coatheal::Tmc2240Driver::CalculateCurrentSettings(
      0.8, 0.30, 0.0, &settings));
  assert(settings.range_code == 1U);
  assert(std::fabs(settings.range_a_peak - 2.0) < 1e-9);
  assert(settings.global_scaler == 145U);
  assert(((settings.ihold_irun >> 8U) & 0x1FU) == 31U);
  assert((settings.ihold_irun & 0x1FU) == 9U);

  assert(!coatheal::Tmc2240Driver::CalculateCurrentSettings(
      0.8, 0.30, 1.0, &settings));
  assert(!coatheal::Tmc2240Driver::CalculateCurrentSettings(
      2.2, 0.30, 0.0, &settings));
  assert(coatheal::Tmc2240Driver::CalculateCurrentSettings(
      1.0 / std::sqrt(2.0), 0.30, 1.0, &settings));
  assert(settings.global_scaler == 0U);
}

void TestHeaterSchedulerCap() {
  coatheal::PowerConfig power;
  power.max_active_heaters = 4;
  power.heater_nominal_w = 5.0;
  power.max_thermal_w = 20.0;

  // Rev C: 6 sample heaters, no box heater. Use SIZE_MAX sentinel.
  coatheal::HeaterScheduler scheduler(power, static_cast<std::size_t>(-1));
  std::vector<double> requested(6, 1.0);

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
  assert(power_sum <= 20.0 + 1e-6);
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

  auto pid = parser.ParseLine("SET_PID All 0.2 0.02 0.03");
  assert(pid.ok);
  assert(pid.command.type == coatheal::CommandType::kSetPid);
  assert(pid.command.args[0] == "ALL");

  auto target = parser.ParseLine("SET_TEMP_TARGET 3 42.5");
  assert(target.ok);
  assert(target.command.type == coatheal::CommandType::kSetTempTarget);

  auto sequence =
      parser.ParseLine("BENDSEQ_LOAD 1 flex 800:2.5:50 0:1");
  assert(sequence.ok);
  assert(sequence.command.type == coatheal::CommandType::kBendSeqLoad);
  assert(sequence.command.args.size() == 4);

  auto zero = parser.ParseLine("SET_POSITION_ZERO 1");
  assert(zero.ok);
  assert(zero.command.type == coatheal::CommandType::kSetPositionZero);

  auto check = parser.ParseLine("CHECK");
  assert(check.ok);
  assert(check.command.type == coatheal::CommandType::kCheck);
  auto targeted_check = parser.ParseLine("CHECK daq132m");
  assert(targeted_check.ok);
  assert(targeted_check.command.args[0] == "DAQ132M");
  auto rtd_check = parser.ParseLine("CHECK rtd_click");
  assert(rtd_check.ok);
  assert(rtd_check.command.args[0] == "RTD_CLICK");
  auto components = parser.ParseLine("COMPONENTS");
  assert(components.ok);
  assert(components.command.type == coatheal::CommandType::kComponents);
  assert(!parser.ParseLine("COMPONENTS extra").ok);

  // SET_TICK_HZ — flight-safe (no debug arm needed at parser layer).
  auto tick = parser.ParseLine("SET_TICK_HZ 0.5");
  assert(tick.ok);
  assert(tick.command.type == coatheal::CommandType::kSetTickHz);
  assert(tick.command.args.size() == 1);
  assert(!tick.command.is_extended);

  auto tick_bad = parser.ParseLine("SET_TICK_HZ");
  assert(!tick_bad.ok);

  auto phase = parser.ParseLine("SET_PHASE pre_float");
  assert(phase.ok);
  assert(phase.command.type == coatheal::CommandType::kSetPhase);
  assert(phase.command.args.size() == 1);
  assert(phase.command.args[0] == "PRE_FLOAT");

  auto manual_heat = parser.ParseLine("SET_ALL_DUTY 0.25");
  assert(manual_heat.ok);
  assert(!manual_heat.command.is_extended);

  auto heater_test = parser.ParseLine("HEATER_TEST 0 0.1 2.5");
  assert(heater_test.ok);
  assert(heater_test.command.type == coatheal::CommandType::kHeaterTest);
  assert(heater_test.command.args.size() == 3);
  assert(!parser.ParseLine("HEATER_TEST 0 0.1").ok);
}

void TestHeaterSchedulerEnergyBudget() {
  // Budget exhaustion latches all heaters off for the rest of the mission
  // (BEXUS User Manual §5.2 — 150 Wh per-team allocation).
  coatheal::PowerConfig power;
  power.max_active_heaters = 4;
  power.heater_nominal_w = 5.0;
  power.max_thermal_w = 20.0;
  power.energy_budget_wh = 0.02;  // tiny budget so the test runs fast: 0.02 Wh

  // Rev C: 6 channels (6 heated samples, no box heater).
  coatheal::HeaterScheduler scheduler(power, static_cast<std::size_t>(-1));
  std::vector<double> requested(6, 1.0);

  // 20 W * dt / 3600 — at dt=1 s we burn 20/3600 ≈ 0.00556 Wh per tick.
  // After 4 ticks we should hit 0.0222 Wh, exceeding the 0.02 Wh budget.
  bool latched = false;
  for (int tick = 0; tick < 10; ++tick) {
    auto out = scheduler.Schedule(requested, true, 1.0);
    if (scheduler.is_budget_exhausted()) {
      // Once latched, all subsequent ticks must be all-zero.
      for (double d : out) {
        assert(d == 0.0);
      }
      latched = true;
    }
  }
  assert(latched);
  assert(scheduler.energy_consumed_wh() >= power.energy_budget_wh - 1e-9);

  // Reset() unlatches.
  scheduler.Reset();
  assert(!scheduler.is_budget_exhausted());
  assert(scheduler.energy_consumed_wh() == 0.0);
  auto out = scheduler.Schedule(requested, true, 1.0);
  int active = 0;
  for (double d : out) if (d > 1e-6) ++active;
  assert(active > 0);

  // Budget disabled (== 0) should never latch even after many ticks.
  power.energy_budget_wh = 0.0;
  coatheal::HeaterScheduler unbounded(power, static_cast<std::size_t>(-1));
  for (int tick = 0; tick < 100; ++tick) {
    unbounded.Schedule(requested, true, 1.0);
    assert(!unbounded.is_budget_exhausted());
  }
}

void TestVacuumRegime() {
  // BEXUS User Manual §5.6: experiment acceptance pressure is 5 mbar.
  // The simulated sensor must reach the float-pressure regime so we can
  // verify the FSM stays stable in FLOAT at flight pressure.
  coatheal::OnboardConfig config;
  config.transition.pre_float_mbar = 140.0;
  config.transition.debounce_samples = 1;
  config.transition.float_to_descent_mbar = 300.0;
  config.transition.descent_to_landed_mbar = 800.0;
  config.hardware.heater_count = 6;
  config.hardware.electronics_heater_index = static_cast<std::size_t>(-1);
  config.runtime.use_simulated_sensors = true;

  coatheal::SensorManager sensors(config, nullptr, nullptr, nullptr);
  std::vector<double> heater_duty(6, 0.0);

  // Step the simulator forward at 1 Hz for 20 minutes — long enough for
  // pressure to descend below the 140 mbar ascent->float threshold and reach
  // the 5 mbar floor that matches BEXUS float conditions.
  double min_pressure = 1e9;
  for (int i = 0; i < 1200; ++i) {
    auto snap = sensors.ReadSnapshot(coatheal::MissionPhase::kFloat,
                                     heater_duty, 1.0);
    if (snap.ambient_pressure_mbar < min_pressure) {
      min_pressure = snap.ambient_pressure_mbar;
    }
  }
  // Must reach the 5 mbar floor.
  assert(min_pressure <= 5.5);

  // FSM stays in FLOAT while pressure is below the descent threshold.
  coatheal::StateManager sm(config);
  std::vector<double> samples(8, 5.0);
  // Drive BOOT → ASCENT → FLOAT via pressure transitions.
  auto p = sm.Update(900.0, samples, {}, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kAscent);
  p = sm.Update(120.0, samples, {}, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kPreFloat);
  p = sm.Update(80.0, samples, {}, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kFloat);
  // Vacuum-regime pressure (5 mbar) must NOT trip the descent transition.
  p = sm.Update(5.0, samples, {}, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kFloat);
  // ...but a real descent (>= 300 mbar) must.
  p = sm.Update(350.0, samples, {}, std::chrono::steady_clock::now());
  assert(p == coatheal::MissionPhase::kDescent);
}

void TestTelemetrySerializer() {
  coatheal::TelemetryRecord record;
  record.seq = 42;
  record.phase = coatheal::MissionPhase::kFloat;
  record.sensors.timestamp_utc = "2026-03-31T12:00:00Z";
  record.sensors.rtc_valid = true;
  record.sensors.ambient_temp_c = -40.0;
  record.sensors.ambient_pressure_mbar = 120.0;
  record.sensors.uv = 1.2;
  record.sensors.sample_temps_c = {7.0, 6.5, 7.2};
  record.sensors.sample_resistance_ohm = {100.0, 99.5, 0.0};
  record.heater_duty = {0.1, 0.2, 0.3};

  const std::string frame = coatheal::SerializeTelemetryDataFrame(record, "session-abc");
  assert(frame.rfind("DATA,session-abc,42,", 0) == 0);
  assert(frame.find("HEATER_DUTY=") != std::string::npos);
  assert(frame.find("RESISTANCE=") != std::string::npos);
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
    f1.frame = "DATA,s1,1,2026-01-01T00:00:01Z,1,0,0,0,0,0,HEATER_DUTY=0.0,PHASE=ASCENT,STATUS=SD_OK";

    coatheal::QueuedTelemetryFrame f2 = f1;
    f2.seq = 2;
    f2.frame = "DATA,s1,2,2026-01-01T00:00:02Z,1,0,0,0,0,0,HEATER_DUTY=0.0,PHASE=ASCENT,STATUS=SD_OK";

    coatheal::QueuedTelemetryFrame ev;
    ev.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
    ev.session_id = "s1";
    ev.seq = 3;
    ev.frame = "EVT,PULL,s1,3,0,2026-01-01T00:00:03Z,200,1.00,0|1";

    assert(queue.Enqueue(f1, &error));
    assert(queue.Enqueue(f2, &error));
    assert(queue.Enqueue(ev, &error));
    assert(queue.size() == 3);
    assert(queue.Acknowledge("s1", 1, &error));
    assert(queue.size() == 2);
    assert(queue.AcknowledgeExact(ev, &error));
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
  out << "runtime.use_simulated_sensors=false\n";
  out << "runtime.gpio_chip=/dev/gpiochip0\n";
  out << "manual.manual_first=true\n";
  out << "manual.link_loss_fallback_enabled=true\n";
  out << "manual.link_loss_fallback_s=12.5\n";
  out << "comms.telemetry_host=\n";
  out << "comms.static_ground_ip=\n";
  out << "comms.static_pi_ip=169.254.10.10\n";
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
  // Rev C phase keys: floor-only fallback thermal policy (no box target).
  out << "phase.sample_floor_c=5\n";
  out << "phase.uniformity_tolerance_c=2\n";
  out << "transition.ascent_to_float_mbar=100\n";
  out << "transition.float_to_descent_mbar=300\n";
  out << "transition.descent_to_landed_mbar=800\n";
  out << "power.max_active_heaters=4\n";
  out << "power.max_thermal_w=20\n";
  out << "power.max_system_w=48.23\n";
  out << "power.heater_nominal_w=5\n";
  out << "power.energy_budget_wh=130.0\n";
  out << "power.logic_regulator_v=5.0\n";
  out << "power.stepper_regulator_v=12.0\n";
  out << "pid.kp=0.2\n";
  out << "pid.ki=0.02\n";
  out << "pid.kd=0.03\n";
  // Final BOM: 8 samples, 6 heaters, no box heater.
  out << "hardware.sample_count=8\n";
  out << "hardware.heater_count=6\n";
  out << "sensor.sample_temperature_source=rtd_click_max31865\n";
  out << "sensor.daq132m_enabled=false\n";
  out << "sensor.daq132m_device=/dev/ttyUSB0\n";
  out << "sensor.daq132m_baud=9600\n";
  out << "sensor.daq132m_parity=N\n";
  out << "sensor.daq132m_data_bits=8\n";
  out << "sensor.daq132m_stop_bits=1\n";
  out << "sensor.daq132m_slave_id=1\n";
  out << "sensor.daq132m_function_code=4\n";
  out << "sensor.daq132m_register_base=0\n";
  out << "sensor.daq132m_register_count=8\n";
  out << "sensor.daq132m_c_per_count=0.1\n";
  out << "sensor.daq132m_c_offset=-5.0\n";
  out << "heater.target_min_c=0.0\n";
  out << "heater.target_max_c=80.0\n";
  out << "sensor.rtd_click_enabled=true\n";
  out << "sensor.rtd_click_spi_device=/dev/spidev0.0\n";
  out << "sensor.rtd_click_cs_line=7\n";
  out << "sensor.rtd_click_drdy_line=25\n";
  out << "sensor.rtd_click_wires=3\n";
  out << "sensor.rtd_click_sample_channel=0\n";
  out << "sensor.rtd_click_reference_ohm=400.0\n";
  out << "sensor.rtd_click_filter_hz=50\n";
  out << "sensor.rtd_click_spi_speed_hz=500000\n";
  out << "sensor.pressure_source=dps310\n";
  out << "sensor.dps310_i2c_addr=0x77\n";
  out << "sensor.uv_source=guva_s12sd_ads1115\n";
  out << "sensor.ads1115_i2c_addr=0x48\n";
  out << "sensor.uv_ads1115_channel=0\n";
  out << "sensor.uv_full_scale_v=4.096\n";
  out << "sensor.resistance_source=disabled\n";
  out << "heater.output_lines=17,18,27,5,6,13\n";
  out << "heater.pwm_frequency_hz=10.0\n";
  out << "heater.active_high=true\n";
  out << "heater.debug_max_duty=0.25\n";
  out << "heater.debug_max_seconds=10.0\n";
  out << "hal.status_led_enabled=false\n";
  out << "hal.mode_led_enabled=false\n";
  out << "pull.max_step_hz=100.0\n";
  out << "pull.accel_steps_per_s2=200.0\n";
  out << "pull.microstep=4\n";
  out << "pull.travel_full_steps=200\n";
  out << "pull.hold_s=5.0\n";
  out << "motor0.driver=tmc2240\n";
  out << "motor0.gpio_chip=/dev/gpiochip0\n";
  out << "motor0.spi_device=/dev/spidev0.0\n";
  out << "motor0.cs_line=22\n";
  out << "motor0.step_line=19\n";
  out << "motor0.dir_line=26\n";
  out << "motor0.enable_line=12\n";
  out << "motor0.run_current_a_rms=2.0\n";
  out << "motor0.current_range_a_peak=0\n";
  out << "motor0.hold_current_frac=0.30\n";
  out << "motor0.stealth_chop=true\n";
  out << "motor0.spi_speed_hz=1000000\n";
  out << "motor0.samples=0,1,2,3\n";
  out << "motor1.driver=tmc2240\n";
  out << "motor1.gpio_chip=/dev/gpiochip0\n";
  out << "motor1.spi_device=/dev/spidev0.0\n";
  out << "motor1.cs_line=23\n";
  out << "motor1.step_line=16\n";
  out << "motor1.dir_line=20\n";
  out << "motor1.enable_line=21\n";
  out << "motor1.run_current_a_rms=2.0\n";
  out << "motor1.current_range_a_peak=0\n";
  out << "motor1.hold_current_frac=0.30\n";
  out << "motor1.stealth_chop=true\n";
  out << "motor1.spi_speed_hz=1000000\n";
  out << "motor1.samples=4,5,6,7\n";
  out.close();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(cfg_path.string(), &cfg, &error));
  assert(cfg.comms.discovery_enabled);
  assert(cfg.comms.telemetry_host.empty());
  assert(cfg.comms.static_ground_ip.empty());
  assert(cfg.comms.static_pi_ip == "169.254.10.10");
  assert(cfg.storage.queue_max_bytes == 1024U);
  assert(!cfg.runtime.use_simulated_pwm);
  assert(!cfg.runtime.use_simulated_sensors);
  assert(cfg.manual.manual_first);
  assert(cfg.manual.link_loss_fallback_enabled);
  assert(std::fabs(cfg.manual.link_loss_fallback_s - 12.5) < 1e-9);
  assert(std::fabs(cfg.power.energy_budget_wh - 130.0) < 1e-9);
  assert(std::fabs(cfg.power.logic_regulator_v - 5.0) < 1e-9);
  assert(std::fabs(cfg.power.stepper_regulator_v - 12.0) < 1e-9);
  assert(cfg.hardware.sample_count == 8U);
  assert(cfg.hardware.heater_count == 6U);
  assert(cfg.hardware.electronics_heater_index == static_cast<std::size_t>(-1));
  assert(std::fabs(cfg.power.heater_nominal_w - 5.0) < 1e-9);
  assert(std::fabs(cfg.power.max_thermal_w - 20.0) < 1e-9);
  assert(cfg.sensors.sample_temperature_source == "rtd_click_max31865");
  assert(!cfg.sensors.daq132m_enabled);
  assert(cfg.sensors.daq132m_device == "/dev/ttyUSB0");
  assert(cfg.sensors.daq132m_register_count == 8);
  assert(cfg.sensors.daq132m_function_code == 4);
  assert(std::fabs(cfg.sensors.daq132m_c_offset - (-5.0)) < 1e-9);
  assert(cfg.sensors.rtd_click_enabled);
  assert(cfg.sensors.rtd_click_sample_channel == 0U);
  assert(std::fabs(cfg.sensors.rtd_click_reference_ohm - 400.0) < 1e-9);
  assert(cfg.sensors.rtd_click_filter_hz == 50);
  assert(cfg.sensors.rtd_click_spi_speed_hz == 500000U);
  assert(std::fabs(cfg.heater_safety.target_min_c - 0.0) < 1e-9);
  assert(std::fabs(cfg.heater_safety.target_max_c - 80.0) < 1e-9);
  assert(cfg.sensors.dps310_i2c_addr == 0x77);
  assert(cfg.sensors.ads1115_i2c_addr == 0x48);
  assert(cfg.sensors.uv_ads1115_channel == 0);
  assert(cfg.sensors.resistance_source == "disabled");
  assert(cfg.heaters.output_lines.size() == 6U);
  assert(cfg.heaters.output_lines[0] == 17U);
  assert(cfg.heaters.output_lines[5] == 13U);
  assert(std::fabs(cfg.heaters.pwm_frequency_hz - 10.0) < 1e-9);
  assert(cfg.heaters.active_high);
  assert(std::fabs(cfg.heaters.debug_max_duty - 0.25) < 1e-9);
  assert(std::fabs(cfg.heaters.debug_max_seconds - 10.0) < 1e-9);
  assert(cfg.pull.microstep == 4);
  assert(cfg.pull.travel_full_steps == 200);
  assert(cfg.motors[0].driver == "tmc2240");
  assert(cfg.motors[0].gpio_chip == "/dev/gpiochip0");
  assert(cfg.motors[0].spi_device == "/dev/spidev0.0");
  assert(cfg.motors[0].cs_line == 22U);
  assert(cfg.motors[0].step_line == 19U);
  assert(cfg.motors[0].dir_line == 26U);
  assert(cfg.motors[0].enable_line == 12U);
  assert(cfg.motors[0].samples == std::vector<std::size_t>({0, 1, 2, 3}));
  assert(cfg.motors[1].driver == "tmc2240");
  assert(cfg.motors[1].gpio_chip == "/dev/gpiochip0");
  assert(cfg.motors[1].spi_device == "/dev/spidev0.0");
  assert(cfg.motors[1].cs_line == 23U);
  assert(cfg.motors[1].step_line == 16U);
  assert(cfg.motors[1].dir_line == 20U);
  assert(cfg.motors[1].enable_line == 21U);
  assert(cfg.motors[1].samples == std::vector<std::size_t>({4, 5, 6, 7}));

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestConfigRejectsGpioCollisions() {
  const std::filesystem::path cfg_path =
      std::filesystem::temp_directory_path() / "coatheal_gpio_collision.ini";
  std::ofstream out(cfg_path);
  out << "heater.output_lines=17,18,27,5,6,12\n";
  out.close();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(!coatheal::LoadConfigFromIni(cfg_path.string(), &cfg, &error));
  assert(error.find("BCM GPIO 12 assigned to both") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestStateTransitions() {
  // Fallback phase tracking is pressure-only and never starts motion.
  coatheal::OnboardConfig config;
  config.transition.pre_float_mbar = 200.0;
  config.transition.ascent_to_float_mbar = 160.0;
  config.transition.float_to_descent_mbar = 350.0;
  config.transition.descent_to_landed_mbar = 800.0;
  config.transition.debounce_samples = 1;

  coatheal::StateManager sm(config);
  std::vector<double> samples(8, 5.0);

  // First tick out of BOOT lands in ASCENT.
  auto phase = sm.Update(900.0, samples, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kAscent);

  // 150 mbar: <= ascent_to_float_mbar (200). Transitions to FLOAT.
  phase = sm.Update(150.0, samples, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kPreFloat);
  phase = sm.Update(150.0, samples, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kFloat);

  // 400 mbar: >= float_to_descent_mbar (350). Transitions to DESCENT.
  phase = sm.Update(400.0, samples, {}, std::chrono::steady_clock::now());
  assert(phase == coatheal::MissionPhase::kDescent);
}

void TestManualHeaterOverrideWithoutFloorControl() {
  coatheal::OnboardConfig config;
  config.hardware.heater_count = 6;
  config.hardware.electronics_heater_index = static_cast<std::size_t>(-1);
  coatheal::ThermalController tc(config);

  coatheal::SensorSnapshot snap;
  snap.sample_temps_c.assign(8, 20.0);
  coatheal::ControlOverrides overrides;
  overrides.floor_control_enabled = false;
  overrides.all_heaters_override = 0.25;

  const auto duty = tc.ComputeRequestedDuty(coatheal::MissionPhase::kBoot,
                                            snap, 1.0, overrides);
  assert(duty.size() == 6);
  for (double d : duty) {
    assert(std::fabs(d - 0.25) < 1e-9);
  }
}

void TestDiscoveryBeaconParser() {
  // discovery_enabled=false keeps Start()/Stop() a no-op so the test does not
  // open real sockets — ProcessIncomingDiscoveryLine is still usable.
  coatheal::TelemetryClient client("", 4000, 5000, 2000, false, 4100, "", "",
                                   2000, 30, 5, 100);

  const bool ok = client.ProcessIncomingDiscoveryLine(
      "GS_BEACON,abc,4000,5000,200", "10.0.0.42");
  assert(ok);

  const coatheal::GroundStationAdvert latest = client.latest_gs();
  assert(latest.valid);
  assert(latest.host == "10.0.0.42");
  assert(latest.telemetry_port == 4000);
  assert(latest.command_port == 5000);
  assert(latest.priority == 200);

  // Malformed line: wrong field count — must not crash, must return false.
  const bool bad = client.ProcessIncomingDiscoveryLine("GS_BEACON,abc,4000",
                                                        "10.0.0.42");
  assert(!bad);

  // Non-beacon line returns false (GS_HELLO is handled in the listener loop).
  const bool other = client.ProcessIncomingDiscoveryLine(
      "RANDOM_JUNK,1,2,3", "10.0.0.42");
  assert(!other);
}

void TestCommandPeerCanSeedTelemetryTarget() {
  coatheal::TelemetryClient client("", 4000, 5000, 2000, false, 4100, "", "",
                                   2000, 30, 5, 100);

  client.ObserveGroundStation("169.254.10.11", 4000, 5000, 1000);

  const coatheal::GroundStationAdvert latest = client.latest_gs();
  assert(latest.valid);
  assert(latest.host == "169.254.10.11");
  assert(latest.telemetry_port == 4000);
  assert(latest.command_port == 5000);
  assert(latest.priority == 1000);
  assert(client.current_host() == "169.254.10.11");
}

}  // namespace

int main() {
  TestPidBoundsAndAntiWindup();
  TestTmc2240CurrentConfiguration();
  TestHeaterSchedulerCap();
  TestHeaterSchedulerEnergyBudget();
  TestCommandParser();
  TestTelemetrySerializer();
  TestTelemetryQueuePersistenceAndAck();
  TestConfigParsesReliabilityFields();
  TestConfigRejectsGpioCollisions();
  TestStateTransitions();
  TestManualHeaterOverrideWithoutFloorControl();
  TestVacuumRegime();
  TestDiscoveryBeaconParser();
  TestCommandPeerCanSeedTelemetryTarget();

  std::cout << "All unit tests passed.\n";
  return 0;
}
