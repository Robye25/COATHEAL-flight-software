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
#include "coatheal/system_controller.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_queue.hpp"
#include "coatheal/thermal_controller.hpp"

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

  auto lower_ping = parser.ParseLine("ping");
  assert(lower_ping.ok);
  assert(lower_ping.command.type == coatheal::CommandType::kPing);

  auto alias = parser.ParseLine("ON");
  assert(alias.ok);
  assert(alias.command.type == coatheal::CommandType::kForceStart);

  auto spaced_alias = parser.ParseLine("force start");
  assert(spaced_alias.ok);
  assert(spaced_alias.command.type == coatheal::CommandType::kForceStart);

  auto invalid = parser.ParseLine("SET_PID 1 2");
  assert(!invalid.ok);

  auto pid = parser.ParseLine("SET_PID All 0.2 0.02 0.03");
  assert(pid.ok);
  assert(pid.command.type == coatheal::CommandType::kSetPid);
  assert(pid.command.args[0] == "ALL");

  auto target = parser.ParseLine("SET_TEMP_TARGET 3 42.5");
  assert(target.ok);
  assert(target.command.type == coatheal::CommandType::kSetTempTarget);

  auto spaced_target = parser.ParseLine("set temp target 3 42.5");
  assert(spaced_target.ok);
  assert(spaced_target.command.type == coatheal::CommandType::kSetTempTarget);
  assert(spaced_target.command.args[0] == "3");
  assert(spaced_target.command.args[1] == "42.5");

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
  auto spaced_rtd_check = parser.ParseLine("check rtd click");
  assert(spaced_rtd_check.ok);
  assert(spaced_rtd_check.command.args[0] == "RTD_CLICK");
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
  {
    // MOTOR_DEBUG <id>: exactly one argument, the motor id.
    const auto dbg = parser.ParseLine("MOTOR_DEBUG 1");
    assert(dbg.ok);
    assert(dbg.command.type == coatheal::CommandType::kMotorDebug);
    assert(dbg.command.args.size() == 1 && dbg.command.args[0] == "1");
    assert(!parser.ParseLine("MOTOR_DEBUG").ok);
    assert(!parser.ParseLine("MOTOR_DEBUG 1 2").ok);
  }
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
    // compact_min_dead_bytes=1: compact on every acknowledge, so this test
    // keeps asserting the strict "acked frames are gone from disk" contract.
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024,
                                   /*compact_min_dead_bytes=*/1);
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

// The queue no longer rewrites its whole backing file on every mutation
// (that was O(backlog) disk I/O on the control loop and is what tripped the
// systemd watchdog with a large backlog). These are the new observable
// contracts: bounded PendingFrames, ack-then-crash re-delivery
// (at-least-once), torn/garbage line tolerance, and unconditional
// retention-age pruning at load.
void TestTelemetryQueueDeferredCompactionRetentionAndTornLines() {
  const std::filesystem::path queue_dir =
      std::filesystem::temp_directory_path() /
      ("coatheal_queue_test2_" +
       std::to_string(coatheal::CurrentUnixEpochSeconds()));

  auto make_frame = [](std::uint64_t seq) {
    coatheal::QueuedTelemetryFrame f;
    f.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
    f.session_id = "s1";
    f.seq = seq;
    f.frame = "DATA,s1," + std::to_string(seq) + ",2026-01-01T00:00:01Z,1,0";
    return f;
  };

  std::string error;
  {
    // compact_cheap_live_bytes=0 pins the deferred regime: with it at its
    // default these frames would be small enough to rewrite for free and
    // would be compacted away immediately. At-least-once redelivery only
    // survives for a backlog too large to rewrite cheaply, and that is
    // what these assertions cover.
    coatheal::TelemetryQueue queue(
        queue_dir.string(), 72.0, 1024 * 1024,
        coatheal::TelemetryQueue::kDefaultCompactMinDeadBytes,
        coatheal::TelemetryQueue::kDefaultCompactMaxLiveBytes,
        /*compact_cheap_live_bytes=*/0);
    assert(queue.Initialize(&error));
    for (std::uint64_t seq = 1; seq <= 4; ++seq) {
      assert(queue.Enqueue(make_frame(seq), &error));
    }
    assert(queue.Acknowledge("s1", 2, &error));
    assert(queue.size() == 2);

    // PendingFrames(max) returns the oldest frames, bounded.
    const auto batch = queue.PendingFrames(1);
    assert(batch.size() == 1);
    assert(batch.front().seq == 3);
  }

  {
    // Same directory reloaded: the acked frames were never compacted away,
    // so they come back (at-least-once re-delivery after a crash). The
    // ground station deduplicates; losing them here would be the bug.
    coatheal::TelemetryQueue queue(
        queue_dir.string(), 72.0, 1024 * 1024,
        coatheal::TelemetryQueue::kDefaultCompactMinDeadBytes,
        coatheal::TelemetryQueue::kDefaultCompactMaxLiveBytes,
        /*compact_cheap_live_bytes=*/0);
    assert(queue.Initialize(&error));
    assert(queue.size() == 4);
  }

  {
    // Garbage and a torn (partial) final line must not disable the queue.
    std::ofstream out((queue_dir / "pending.queue").string(), std::ios::app);
    out << "not a queue line\n";
    out << "12345\ts1";  // torn append: no trailing separator/frame/newline
    out.close();

    coatheal::TelemetryQueue queue(
        queue_dir.string(), 72.0, 1024 * 1024,
        coatheal::TelemetryQueue::kDefaultCompactMinDeadBytes,
        coatheal::TelemetryQueue::kDefaultCompactMaxLiveBytes,
        /*compact_cheap_live_bytes=*/0);
    assert(queue.Initialize(&error));
    assert(queue.size() == 4);
  }

  {
    // A frame older than retention is dropped at load, whatever the size cap.
    std::ofstream out((queue_dir / "pending.queue").string(), std::ios::app);
    const std::int64_t stale_epoch =
        coatheal::CurrentUnixEpochSeconds() - 80 * 3600;
    out << stale_epoch << "\told-session\t9\tDATA,old-session,9,stale\n";
    out.close();

    coatheal::TelemetryQueue queue(
        queue_dir.string(), 72.0, 1024 * 1024,
        coatheal::TelemetryQueue::kDefaultCompactMinDeadBytes,
        coatheal::TelemetryQueue::kDefaultCompactMaxLiveBytes,
        /*compact_cheap_live_bytes=*/0);
    assert(queue.Initialize(&error));
    const auto pending = queue.PendingFrames();
    assert(pending.size() == 4);
    for (const auto& frame : pending) {
      assert(frame.session_id == "s1");
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(queue_dir, ec);
}

// The healthy steady state must leave nothing already-acked on disk.
//
// Deferred compaction traded disk cleanliness for control-loop latency,
// and on the bench that trade surfaced as the ground station logging
// "[dup] dropped" for hundreds of frames after every hard power cut: the
// acked frames were still sitting in pending.queue and got re-sent on
// restart. Nothing is lost -- the ground deduplicates -- but it spends
// downlink re-sending frames that already landed. When the live set is
// small the rewrite is free, so there is no reason to carry them.
void TestDrainedQueueLeavesNothingToReplay() {
  const std::filesystem::path queue_dir =
      std::filesystem::temp_directory_path() /
      ("coatheal_queue_test3_" +
       std::to_string(coatheal::CurrentUnixEpochSeconds()));

  std::string error;
  {
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));
    // One frame enqueued and acked per tick, exactly like the control loop
    // does with a healthy link.
    for (std::uint64_t seq = 1; seq <= 50; ++seq) {
      coatheal::QueuedTelemetryFrame f;
      f.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
      f.session_id = "s1";
      f.seq = seq;
      f.frame = "DATA,s1," + std::to_string(seq) +
                ",2026-01-01T00:00:01Z,1,0,0,0,HEATER_DUTY=0.0,STATUS=SD_OK";
      assert(queue.Enqueue(f, &error));
      assert(queue.Acknowledge("s1", seq, &error));
    }
    assert(queue.size() == 0);
  }

  {
    // The power cut: reopen the same directory with no clean shutdown in
    // between. Nothing acked may come back.
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));
    assert(queue.size() == 0);
  }

  std::error_code ec;
  std::filesystem::remove_all(queue_dir, ec);
}

// Unacked frames are still durable -- the point of the queue. Compacting a
// drained queue must not be confused with discarding a backlog.
void TestUnackedFramesStillSurviveRestart() {
  const std::filesystem::path queue_dir =
      std::filesystem::temp_directory_path() /
      ("coatheal_queue_test4_" +
       std::to_string(coatheal::CurrentUnixEpochSeconds()));

  std::string error;
  {
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
      coatheal::QueuedTelemetryFrame f;
      f.queued_epoch_s = coatheal::CurrentUnixEpochSeconds();
      f.session_id = "s2";
      f.seq = seq;
      f.frame = "DATA,s2," + std::to_string(seq) + ",unacked";
      assert(queue.Enqueue(f, &error));
    }
    assert(queue.Acknowledge("s2", 2, &error));  // link died after seq 2
    assert(queue.size() == 3);
  }

  {
    coatheal::TelemetryQueue queue(queue_dir.string(), 72.0, 1024 * 1024);
    assert(queue.Initialize(&error));
    const auto pending = queue.PendingFrames();
    assert(pending.size() == 3);
    assert(pending.front().seq == 3);
    assert(pending.back().seq == 5);
  }

  std::error_code ec;
  std::filesystem::remove_all(queue_dir, ec);
}

// Writes a complete, valid baseline config to a unique temp path, with
// `extra` appended so a test can override or add individual keys. Returns
// the path. The INI parser is last-assignment-wins, so an appended line
// overrides the same key in the baseline.
std::string WriteTempConfig(const std::string& extra = "") {
  static int counter = 0;
  const std::filesystem::path cfg_path =
      std::filesystem::temp_directory_path() /
      ("coatheal_cfg_test_" + std::to_string(coatheal::CurrentUnixEpochSeconds()) +
       "_" + std::to_string(++counter) + ".ini");

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
  out << "power.max_active_heaters=3\n";
  out << "power.max_thermal_w=15\n";
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
  out << "heater.target_min_c=0.0\n";
  out << "heater.target_max_c=80.0\n";
  out << "sensor.pressure_source=dps310\n";
  out << "sensor.dps310_i2c_addr=0x77\n";
  out << "sensor.uv_source=guva_s12sd_ads1115\n";
  out << "sensor.ads1115_i2c_addr=0x48\n";
  out << "sensor.uv_ads1115_channel=0\n";
  out << "sensor.uv_full_scale_v=4.096\n";
  out << "sensor.resistance_source=disabled\n";
  out << "sensor.max31865_reference_ohm=470.0\n";
  out << "sensor.max31865_poll_ms=1000\n";
  out << "sensor.max31865_sample_indices=0,4\n";
  out << "heater.output_lines=19,13,6,5,24,23\n";
  out << "heater.pwm_frequency_hz=1.0\n";
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
  out << "motor0.driver=tmc5160\n";
  out << "motor0.gpio_chip=/dev/gpiochip0\n";
  out << "motor0.spi_device=/dev/spidev0.0\n";
  out << "motor0.cs_line=22\n";
  out << "motor0.enable_line=20\n";
  out << "motor0.run_current_a_rms=2.0\n";
  out << "motor0.hold_current_frac=0.30\n";
  out << "motor0.stealth_chop=true\n";
  out << "motor0.spi_speed_hz=1000000\n";
  out << "motor0.sense_resistor_ohm=0.075\n";
  out << "motor0.samples=0,1,2,3\n";
  out << "motor1.driver=tmc5160\n";
  out << "motor1.gpio_chip=/dev/gpiochip0\n";
  out << "motor1.spi_device=/dev/spidev0.0\n";
  out << "motor1.cs_line=27\n";
  out << "motor1.enable_line=21\n";
  out << "motor1.run_current_a_rms=2.0\n";
  out << "motor1.hold_current_frac=0.30\n";
  out << "motor1.stealth_chop=true\n";
  out << "motor1.spi_speed_hz=1000000\n";
  out << "motor1.sense_resistor_ohm=0.075\n";
  out << "motor1.samples=4,5,6,7\n";
  out << extra;
  out.close();

  return cfg_path.string();
}

void TestConfigParsesReliabilityFields() {
  const std::string cfg_path = WriteTempConfig();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(cfg_path, &cfg, &error));
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
  assert(std::fabs(cfg.power.max_thermal_w - 15.0) < 1e-9);
  assert(cfg.power.max_active_heaters == 3U);
  assert(std::fabs(cfg.heater_safety.target_min_c - 0.0) < 1e-9);
  assert(std::fabs(cfg.heater_safety.target_max_c - 80.0) < 1e-9);
  assert(cfg.sensors.dps310_i2c_addr == 0x77);
  assert(cfg.sensors.ads1115_i2c_addr == 0x48);
  assert(cfg.sensors.uv_ads1115_channel == 0);
  assert(cfg.sensors.resistance_source == "disabled");
  assert(std::fabs(cfg.sensors.max31865_reference_ohm - 470.0) < 1e-9);
  assert(cfg.sensors.max31865_poll_ms == 1000);
  assert(cfg.sensors.max31865_sample_indices ==
        std::vector<std::size_t>({0, 4}));
  assert(cfg.heaters.output_lines.size() == 6U);
  assert(cfg.heaters.output_lines[0] == 19U);
  assert(cfg.heaters.output_lines[5] == 23U);
  assert(std::fabs(cfg.heaters.pwm_frequency_hz - 1.0) < 1e-9);
  assert(cfg.heaters.active_high);
  assert(std::fabs(cfg.heaters.debug_max_duty - 0.25) < 1e-9);
  assert(std::fabs(cfg.heaters.debug_max_seconds - 10.0) < 1e-9);
  assert(cfg.pull.microstep == 4);
  assert(cfg.pull.travel_full_steps == 200);
  assert(cfg.motors[0].driver == "tmc5160");
  assert(cfg.motors[0].gpio_chip == "/dev/gpiochip0");
  assert(cfg.motors[0].spi_device == "/dev/spidev0.0");
  assert(cfg.motors[0].cs_line == 22U);
  assert(cfg.motors[0].enable_line == 20U);
  assert(std::fabs(cfg.motors[0].sense_resistor_ohm - 0.075) < 1e-9);
  assert(cfg.motors[0].samples == std::vector<std::size_t>({0, 1, 2, 3}));
  assert(cfg.motors[1].driver == "tmc5160");
  assert(cfg.motors[1].gpio_chip == "/dev/gpiochip0");
  assert(cfg.motors[1].spi_device == "/dev/spidev0.0");
  assert(cfg.motors[1].cs_line == 27U);
  assert(cfg.motors[1].enable_line == 21U);
  assert(std::fabs(cfg.motors[1].sense_resistor_ohm - 0.075) < 1e-9);
  assert(cfg.motors[1].samples == std::vector<std::size_t>({4, 5, 6, 7}));

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestConfigRejectsGpioCollisions() {
  const std::filesystem::path cfg_path =
      std::filesystem::temp_directory_path() / "coatheal_gpio_collision.ini";
  std::ofstream out(cfg_path);
  // Last entry (20) deliberately collides with the default motor0.enable_line
  // (BCM 20, see OnboardConfig()); the other five are the real v3 heater
  // lines and must not themselves collide with anything reserved.
  out << "heater.output_lines=19,13,6,5,24,20\n";
  out.close();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(!coatheal::LoadConfigFromIni(cfg_path.string(), &cfg, &error));
  // Robust to the configured chip path (e.g. "/dev/gpiochip0") rather than
  // asserting the old hardcoded "BCM GPIO" phrasing the message used to have.
  assert(error.find("line 20 assigned to both") != std::string::npos);
  assert(error.find("motor0.enable_line") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestConfigRejectsReservedGpioCollisions() {
  // v3 reserved lines (Sequent RTD HAT + hardware SPI0 chip-selects) must
  // never be claimable by a heater or motor. Six-entry list (matches the
  // default hardware.heater_count=6) so the count-vs-heater_count check
  // passes and the GPIO claim check is actually reached.
  const std::filesystem::path cfg_path =
      std::filesystem::temp_directory_path() / "coatheal_reserved_gpio_collision.ini";
  std::ofstream out(cfg_path);
  out << "heater.output_lines=17,13,6,5,24,23\n";
  out.close();

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(!coatheal::LoadConfigFromIni(cfg_path.string(), &cfg, &error));
  assert(error.find("sequent_hat") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

void TestConfigRejectsRetiredMotorKeys() {
  // tmc2240 is a retired driver identity: the error must name the
  // retirement, not just reject the value generically (a config still on
  // the old driver should tell the operator what to change it to).
  {
    const std::string path = WriteTempConfig("motor0.driver=tmc2240\n");
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find("retired") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  // step_line/dir_line/pulse_high_us no longer have parse branches, so a
  // stale INI carrying one must fall into the "unknown motor config key"
  // path (not the generic top-level "unknown config key" path), guarding
  // any field deployment still on a pre-v3 INI.
  {
    const std::string path = WriteTempConfig("motor0.step_line=19\n");
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find("unknown motor config key") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  // sense_resistor_ohm must be validated (0, 1) exclusive; 0 is invalid.
  {
    const std::string path = WriteTempConfig("motor0.sense_resistor_ohm=0\n");
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find("invalid motor0 configuration") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  // current_range_a_peak (the retired TMC2240 range-select model) no
  // longer has a parse branch either, same treatment as step_line above.
  {
    const std::string path =
        WriteTempConfig("motor0.current_range_a_peak=0\n");
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find("unknown motor config key") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

void TestConfigRejectsOvercurrentCeiling() {
  // TMC5160 hardware ceiling: at the default sense_resistor_ohm=0.075, the
  // sense resistor's maximum deliverable current is 0.325/0.075 = 4.3333
  // A_peak, i.e. 4.3333/sqrt(2) = 3.0641 A_rms. 3.08 A_rms sits just above
  // that (3.08*sqrt(2) = 4.3558 A_peak > 4.3333) while staying under the
  // *separate* flat (0, 3.1] ceiling -- deliberately chosen so this test
  // isolates the sense-resistor-derived rule: deleting only that rule
  // (leaving the flat bound in place) must make this assertion fail, since
  // 3.08 alone would then load successfully.
  const std::string path = WriteTempConfig("motor0.run_current_a_rms=3.08\n");
  coatheal::OnboardConfig cfg;
  std::string error;
  assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
  assert(error.find(
             "exceeds the sense resistor's deliverable current ceiling") !=
         std::string::npos);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestConfigRejectsFlatCurrentBound() {
  // Isolates the flat (0, 3.1] absolute ceiling from the sense-resistor
  // ceiling above: with sense_resistor_ohm=0.05 the sense-resistor ceiling
  // is 0.325/0.05 = 6.5 A_peak, i.e. 6.5/sqrt(2) = 4.5962 A_rms.
  // run_current_a_rms=3.5 is well under that (3.5*sqrt(2) = 4.9497 <
  // 6.5 A_peak, so the sense-resistor rule does NOT fire) but exceeds the
  // flat 3.1 bound -- so this test can only pass because the flat-bound
  // rule specifically fired. Deleting only that rule (leaving the
  // sense-resistor ceiling in place) must make this assertion fail, since
  // 3.5/0.05 alone would then load successfully.
  const std::string path = WriteTempConfig(
      "motor0.run_current_a_rms=3.5\n"
      "motor0.sense_resistor_ohm=0.05\n");
  coatheal::OnboardConfig cfg;
  std::string error;
  assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
  assert(error.find("motor0.run_current_a_rms must be in (0, 3.1]") !=
         std::string::npos);
  // Distinct from the sense-resistor ceiling's fragment: this case must
  // NOT be rejected via that other mechanism.
  assert(error.find("exceeds the sense resistor's deliverable current "
                     "ceiling") == std::string::npos);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestConfigAcceptsFlatCurrentBoundary() {
  // Both-directions companion to TestConfigRejectsFlatCurrentBound: the
  // flat bound is inclusive, "(0, 3.1]", so exactly 3.1 A_rms must load
  // successfully. sense_resistor_ohm=0.05 keeps the sense-resistor ceiling
  // (6.5 A_peak, i.e. 4.5962 A_rms) well clear of 3.1 so only the flat
  // bound's own edge is exercised. Catches a `>` -> `>=` mutation on the
  // flat-bound comparison that TestConfigRejectsFlatCurrentBound's 3.5
  // A_rms case cannot: 3.5 is rejected either way.
  const std::string path = WriteTempConfig(
      "motor0.run_current_a_rms=3.1\n"
      "motor0.sense_resistor_ohm=0.05\n");
  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(path, &cfg, &error));
  assert(std::fabs(cfg.motors[0].run_current_a_rms - 3.1) < 1e-9);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestSequentRtdConfigDefaultsAndParsing() {
  coatheal::OnboardConfig defaults;
  assert(defaults.sensors.sequent_rtd_stack == 0);
  assert(defaults.sensors.sequent_rtd_poll_ms == 1000);
  assert(defaults.sensors.sequent_rtd_expect_sensor_type == "pt100");
  assert(defaults.sensors.sequent_rtd_channels.size() == 8);
  assert(defaults.sensors.sequent_rtd_channels[0] == 1);
  assert(defaults.sensors.sequent_rtd_channels[7] == 8);
  // v3: the MAX31865 dual-click sample-resistance instrument is the shipped
  // default resistance_source (see config.hpp), not "disabled". The Sequent
  // RTD HAT remains the sole sample-*temperature* source regardless of which
  // resistance_source is selected -- that is a separate config axis.
  assert(defaults.sensors.resistance_source == "max31865_click");

  const std::string path = WriteTempConfig(
      "sensor.sequent_rtd_stack=2\n"
      "sensor.sequent_rtd_channels=3,2,1,4,5,6,7,8\n"
      "sensor.sequent_rtd_poll_ms=500\n"
      "sensor.sequent_rtd_expect_sensor_type=pt100\n"
      "sensor.sequent_rtd_resistance_min_ohm=70.0\n"
      "sensor.sequent_rtd_resistance_max_ohm=380.0\n"
      "sensor.sequent_rtd_crosscheck_tol_c=1.5\n"
      "sensor.resistance_source=sequent_rtd\n");

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(path, &cfg, &error));
  assert(cfg.sensors.sequent_rtd_stack == 2);
  assert(cfg.sensors.sequent_rtd_channels[0] == 3);
  assert(cfg.sensors.sequent_rtd_poll_ms == 500);
  // pt100 is also the default, so this line only proves the key is known and
  // parsed, not that a non-default value round-trips: pt100 is now the *only*
  // accepted value (see TestSequentRtdConfigRejectsBadValues for the pt1000
  // rejection and why it exists).
  assert(cfg.sensors.sequent_rtd_expect_sensor_type == "pt100");
  assert(std::fabs(cfg.sensors.sequent_rtd_crosscheck_tol_c - 1.5) < 1e-9);
  assert(std::fabs(cfg.sensors.sequent_rtd_resistance_min_ohm - 70.0) < 1e-9);
  assert(std::fabs(cfg.sensors.sequent_rtd_resistance_max_ohm - 380.0) < 1e-9);
  // Widened validation must actually accept the new value, not just the
  // default: this is the behavioural change, not a cosmetic one.
  assert(cfg.sensors.resistance_source == "sequent_rtd");

  // Full order, not just the first entry: a reversed or mis-assigned list
  // would otherwise slip through.
  const std::vector<std::size_t> expected_channels = {3, 2, 1, 4, 5, 6, 7, 8};
  assert(cfg.sensors.sequent_rtd_channels == expected_channels);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestSequentRtdConfigRejectsBadValues() {
  struct Case { const char* body; const char* fragment; };
  const Case cases[] = {
    {"sensor.sequent_rtd_stack=8\n", "sequent_rtd_stack"},
    {"sensor.sequent_rtd_channels=1,2,3\n", "must have hardware.sample_count"},
    {"sensor.sequent_rtd_channels=1,1,3,4,5,6,7,8\n", "contains duplicates"},
    {"sensor.sequent_rtd_channels=0,2,3,4,5,6,7,8\n", "entries must be 1..8"},
    {"sensor.sequent_rtd_channels=9,2,3,4,5,6,7,8\n", "entries must be 1..8"},
    {"sensor.sequent_rtd_expect_sensor_type=pt500\n", "expect_sensor_type"},
    // Recorded spec deviation: pt1000 is advertised on the card and handled
    // by SequentRtdAdapter::Probe(), but ApplyValidation's card-vs-CVD
    // cross-check hardcodes the PT100 curve and the plausibility window is a
    // PT100 window, so a pt1000 config would load and then mark every channel
    // invalid forever - every heater clamped, no diagnostic. Rejecting at
    // load is the recorded ruling; the exact wording is asserted because the
    // operator-facing explanation is the point of the rejection.
    {"sensor.sequent_rtd_expect_sensor_type=pt1000\n",
     "must be pt100 (pt1000 is recognised but not implemented: the CVD "
     "cross-check and resistance window are PT100-only)"},
    {"sensor.sequent_rtd_resistance_min_ohm=400.0\n", "sequent_rtd_resistance_min_ohm"},
  };
  for (const Case& c : cases) {
    const std::string path = WriteTempConfig(c.body);
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find(c.fragment) != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

// Owner hard rule: <= 3 active heaters, <= 15.0 W thermal. The rule was
// enforced only by HeaterScheduler at runtime; nothing stopped an INI from
// raising the ceiling the scheduler enforces. Each case is isolated -- the
// baseline WriteTempConfig body is otherwise valid, so exactly one mechanism
// can reject each of these.
void TestPowerCapRejectsValuesAboveTheOwnerRule() {
  struct Case { const char* body; const char* fragment; };
  const Case cases[] = {
    // Above the ceiling.
    {"power.max_active_heaters=4\n", "owner power rule: never more than 3"},
    {"power.max_active_heaters=6\n", "owner power rule: never more than 3"},
    // Zero-sanity, same mechanism, opposite end.
    {"power.max_active_heaters=0\n", "must be 1..3"},
    // Above the thermal ceiling. The inclusive edge (exactly 15.0 loads) is
    // pinned by TestPowerCapAcceptsTheOwnerValues below.
    {"power.max_thermal_w=15.5\n", "must be > 0 and <= 15.0"},
    {"power.max_thermal_w=20.0\n", "must be > 0 and <= 15.0"},
    {"power.max_thermal_w=0\n", "must be > 0 and <= 15.0"},
    {"power.max_thermal_w=-1.0\n", "must be > 0 and <= 15.0"},
  };
  for (const Case& c : cases) {
    const std::string path = WriteTempConfig(c.body);
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find(c.fragment) != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

// Both directions: the owner's own values must still LOAD. Without this, a
// validator that rejected everything would pass the negative cases above.
void TestPowerCapAcceptsTheOwnerValues() {
  struct Case { const char* body; };
  const Case cases[] = {
    {"power.max_active_heaters=3\npower.max_thermal_w=15.0\n"},
    {"power.max_active_heaters=1\npower.max_thermal_w=0.1\n"},
  };
  for (const Case& c : cases) {
    const std::string path = WriteTempConfig(c.body);
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

void TestMax31865ConfigDefaultsAndParsing() {
  coatheal::OnboardConfig defaults;
  assert(std::fabs(defaults.sensors.max31865_reference_ohm - 470.0) < 1e-9);
  assert(defaults.sensors.max31865_poll_ms == 1000);
  // Owner-flagged placeholder: first specimen of each motor group
  // (motor0.samples starts at 0, motor1.samples starts at 4).
  assert(defaults.sensors.max31865_sample_indices ==
        std::vector<std::size_t>({0, 4}));
  assert(defaults.sensors.resistance_source == "max31865_click");

  const std::string path = WriteTempConfig(
      "sensor.max31865_reference_ohm=430.0\n"
      "sensor.max31865_poll_ms=250\n"
      "sensor.max31865_sample_indices=2,6\n"
      "sensor.resistance_source=max31865_click\n");

  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(path, &cfg, &error));
  assert(std::fabs(cfg.sensors.max31865_reference_ohm - 430.0) < 1e-9);
  assert(cfg.sensors.max31865_poll_ms == 250);
  assert(cfg.sensors.max31865_sample_indices ==
        std::vector<std::size_t>({2, 6}));
  assert(cfg.sensors.resistance_source == "max31865_click");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void TestMax31865ConfigRejectsBadValues() {
  struct Case { const char* body; const char* fragment; };
  const Case cases[] = {
    {"sensor.max31865_reference_ohm=0\n",
     "sensor.max31865_reference_ohm must be > 0"},
    {"sensor.max31865_reference_ohm=-5\n",
     "sensor.max31865_reference_ohm must be > 0"},
    {"sensor.max31865_poll_ms=0\n",
     "sensor.max31865_poll_ms must be > 0"},
    {"sensor.max31865_sample_indices=0\n",
     "sensor.max31865_sample_indices must have exactly two entries"},
    {"sensor.max31865_sample_indices=0,1,2\n",
     "sensor.max31865_sample_indices must have exactly two entries"},
    // Isolates the distinctness rule from the two rules above/below: the
    // count is exactly two and every entry is in range, so only a
    // duplicate-entries check can fire here.
    {"sensor.max31865_sample_indices=3,3\n",
     "sensor.max31865_sample_indices entries must be distinct"},
    {"sensor.max31865_sample_indices=0,8\n",
     "sensor.max31865_sample_indices entries must be less than "
     "hardware.sample_count"},
  };
  for (const Case& c : cases) {
    const std::string path = WriteTempConfig(c.body);
    coatheal::OnboardConfig cfg;
    std::string error;
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    assert(error.find(c.fragment) != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

void TestLegacySensorKeysAreRejected() {
  const char* legacy[] = {
    "sensor.sample_temperature_source=rtd_click_max31865\n",
    "sensor.rtd_click_enabled=true\n",
    "sensor.rtd_click_spi_device=/dev/spidev0.0\n",
    "sensor.daq132m_enabled=true\n",
    "sensor.daq132m_device=/dev/ttyUSB0\n",
  };
  for (const char* body : legacy) {
    const std::string path = WriteTempConfig(body);
    coatheal::OnboardConfig cfg;
    std::string error;
    // Unknown keys must be rejected loudly, not silently ignored, so a
    // stale deployed INI cannot boot with the operator believing it applied.
    // The load-bearing part is the error message: it must name the exact
    // offending key, so this cannot pass because some unrelated validation
    // (e.g. a coincidentally-invalid GPIO or range check) fired first.
    assert(!coatheal::LoadConfigFromIni(path, &cfg, &error));
    const std::string key = std::string(body).substr(0, std::string(body).find('='));
    assert(error.find(key) != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
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

  // A loopback bench command (priority 0, see HandleCommandLine) must not
  // displace the known ground station even while disconnected...
  client.ObserveGroundStation("127.0.0.1", 4000, 5000, 0);
  assert(client.current_host() == "169.254.10.11");
  // ...but is accepted when nothing better was ever heard.
  coatheal::TelemetryClient bare("", 4000, 5000, 2000, false, 4100, "", "",
                                 2000, 30, 5, 100);
  bare.ObserveGroundStation("127.0.0.1", 4000, 5000, 0);
  assert(bare.current_host() == "127.0.0.1");
}

// ---------------------------------------------------------------------------
// Radio silence (redesign spec §9). A bare SystemController -- constructed
// but never Initialize()d -- drives the command surface without sockets,
// threads, or hardware; that is exactly the seam HandleCommandLine exposes.

bool ContainsText(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

std::filesystem::path FreshQueueDir(const std::string& tag) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("coatheal_radio_" + tag + "_" +
       std::to_string(coatheal::CurrentUnixEpochSeconds()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

coatheal::OnboardConfig LoadRadioTestConfig(const std::filesystem::path& queue_dir) {
  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(WriteTempConfig(), &cfg, &error));
  // The flag file lives under storage.queue_dir; point it at a private
  // temp directory so tests never touch a real queue.
  cfg.storage.queue_dir = queue_dir.string();
  return cfg;
}

void TestRadioSilenceBlocksEverythingButResume() {
  const std::filesystem::path queue_dir = FreshQueueDir("whitelist");
  coatheal::SystemController controller(LoadRadioTestConfig(queue_dir));
  assert(!controller.radio_silent());

  const std::string silenced = controller.HandleCommandLine("RADIO_SILENCE", "");
  assert(silenced.rfind("ACK,RADIO_SILENCE", 0) == 0);
  assert(controller.radio_silent());
  assert(std::filesystem::exists(queue_dir / "radio_silence"));

  // Refused before any handler runs: the reason must be the silence, not
  // the (also true) "stepper unavailable" a bare controller would give.
  const std::string move = controller.HandleCommandLine("STEPPER_MOVE 0 100", "");
  assert(move.rfind("NACK,STEPPER_MOVE", 0) == 0);
  assert(ContainsText(move, "radio silence active"));
  assert(!ContainsText(move, "stepper unavailable"));

  // No side effect: ARM is refused and the mode stays STANDBY.
  const std::string arm = controller.HandleCommandLine("ARM", "");
  assert(arm.rfind("NACK,ARM", 0) == 0);
  assert(ContainsText(arm, "radio silence active"));
  const std::string status = controller.HandleCommandLine("STATUS", "");
  assert(status.rfind("ACK,STATUS", 0) == 0);
  assert(ContainsText(status, ";silence=1"));
  assert(ContainsText(status, ";mode=STANDBY"));
  assert(controller.HandleCommandLine("PING", "") == "ACK,PING,pong");

  const std::string resumed = controller.HandleCommandLine("RADIO_RESUME", "");
  assert(resumed.rfind("ACK,RADIO_RESUME", 0) == 0);
  assert(!controller.radio_silent());
  assert(!std::filesystem::exists(queue_dir / "radio_silence"));
  assert(ContainsText(controller.HandleCommandLine("STATUS", ""), ";silence=0"));
  // After resume the whitelist is gone: a move is judged on its own merits
  // (no stepper here, so a different NACK), and ARM goes through.
  const std::string move_after = controller.HandleCommandLine("STEPPER_MOVE 0 100", "");
  assert(!ContainsText(move_after, "radio silence active"));
  const std::string arm_after = controller.HandleCommandLine("ARM", "");
  assert(arm_after.rfind("ACK,ARM", 0) == 0);
  assert(ContainsText(controller.HandleCommandLine("STATUS", ""), ";mode=RUN"));

  std::filesystem::remove_all(queue_dir);
}

void TestRadioSilencePersistsAcrossRestart() {
  const std::filesystem::path queue_dir = FreshQueueDir("persist");
  {
    coatheal::SystemController first(LoadRadioTestConfig(queue_dir));
    assert(first.HandleCommandLine("RADIO_SILENCE", "").rfind("ACK,", 0) == 0);
  }  // process "exit" with the flag file left behind

  coatheal::SystemController second(LoadRadioTestConfig(queue_dir));
  assert(second.radio_silent());
  assert(ContainsText(second.HandleCommandLine("STATUS", ""), ";silence=1"));
  assert(ContainsText(second.HandleCommandLine("ARM", ""), "radio silence active"));
  assert(second.HandleCommandLine("RADIO_RESUME", "").rfind("ACK,", 0) == 0);

  coatheal::SystemController third(LoadRadioTestConfig(queue_dir));
  assert(!third.radio_silent());
  assert(ContainsText(third.HandleCommandLine("STATUS", ""), ";silence=0"));

  std::filesystem::remove_all(queue_dir);
}

void TestRadioSilenceGatesBeaconAndHelloReply() {
  coatheal::TelemetryClient client("", 4000, 5000, 2000, false, 4100, "", "",
                                   2000, 30, 5, 100);
  // Transmitting and not connected: the beacon loop may broadcast and the
  // listener may answer a GS_HELLO.
  assert(client.beacon_allowed());
  assert(client.hello_reply_allowed());

  client.SetTransmitEnabled(false);
  assert(!client.beacon_allowed());
  assert(!client.hello_reply_allowed());
  assert(client.beacons_sent() == 0);
  assert(client.hello_replies_sent() == 0);

  client.SetTransmitEnabled(true);
  assert(client.beacon_allowed());
  assert(client.hello_reply_allowed());
}

// ---------------------------------------------------------------------------
// Link-loss failsafe plan (redesign spec §10). Same bare-controller seam as
// the radio-silence tests: the command surface and the persistence file are
// exercised without hardware; the state machine itself is covered by
// tests/unit/test_fallback_planner.cpp.

void TestFallbackCommandParsing() {
  coatheal::CommandParser parser;
  const coatheal::CommandParseResult plan = parser.ParseLine("FALLBACK_PLAN 0 800 5 50");
  assert(plan.ok);
  assert(plan.command.type == coatheal::CommandType::kFallbackPlan);
  assert(plan.command.name == "FALLBACK_PLAN");
  assert(plan.command.args.size() == 4);
  assert(parser.ParseLine("FALLBACK_PLAN 1 600 3").ok);
  assert(!parser.ParseLine("FALLBACK_PLAN 1 600").ok);
  assert(!parser.ParseLine("FALLBACK_PLAN 1 600 3 50 extra").ok);
  for (const char* line : {"FALLBACK_ARM", "FALLBACK_DISARM", "FALLBACK_STATUS"}) {
    assert(parser.ParseLine(line).ok);
    assert(!parser.ParseLine(std::string(line) + " 1").ok);
  }
  assert(parser.ParseLine("FALLBACK_ARM").command.type == coatheal::CommandType::kFallbackArm);
  assert(parser.ParseLine("FALLBACK_DISARM").command.type == coatheal::CommandType::kFallbackDisarm);
  assert(parser.ParseLine("FALLBACK_STATUS").command.type == coatheal::CommandType::kFallbackStatus);
}

void TestFallbackPlanCommands() {
  const std::filesystem::path queue_dir = FreshQueueDir("fallback");
  coatheal::SystemController controller(LoadRadioTestConfig(queue_dir));

  // Nothing loaded: arming is refused and the status says so.
  const std::string arm_empty = controller.HandleCommandLine("FALLBACK_ARM", "");
  assert(arm_empty.rfind("NACK,FALLBACK_ARM", 0) == 0);
  assert(ContainsText(arm_empty, "no plan loaded"));
  assert(controller.HandleCommandLine("FALLBACK_STATUS", "") ==
         "ACK,FALLBACK_STATUS,state=none;armed=0;deadline_s=1800;deadline_started=0;m0=-;m1=-");
  assert(ContainsText(controller.HandleCommandLine("STATUS", ""), ";plan=none"));

  // Validation mirrors BENDSEQ_LOAD; motor ids come from the config, so a
  // bare controller (no stepper) can be pre-loaded on the bench.
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_PLAN 2 800 5", ""), "invalid motor id"));
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_PLAN 0 999999 5", ""), "invalid target"));
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_PLAN 0 800 -1", ""), "invalid hold_s"));
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_PLAN 0 800 5 500", ""), "invalid speed_hz"));
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_PLAN 0 800", ""), "invalid argument count"));
  assert(!std::filesystem::exists(queue_dir / "fallback_plan.txt"));

  assert(controller.HandleCommandLine("FALLBACK_PLAN 0 800 5 50", "") ==
         "ACK,FALLBACK_PLAN,motor=0;target=800;hold_s=5;speed_hz=50");
  assert(controller.HandleCommandLine("FALLBACK_PLAN 1 600 3", "") ==
         "ACK,FALLBACK_PLAN,motor=1;target=600;hold_s=3;speed_hz=0");
  assert(std::filesystem::exists(queue_dir / "fallback_plan.txt"));
  assert(controller.HandleCommandLine("FALLBACK_ARM", "") == "ACK,FALLBACK_ARM,plan=armed");
  assert(controller.HandleCommandLine("FALLBACK_STATUS", "") ==
         "ACK,FALLBACK_STATUS,state=armed;armed=1;deadline_s=1800;deadline_started=0;"
         "m0=800/5/50/pending;m1=600/3/0/pending");
  assert(ContainsText(controller.HandleCommandLine("STATUS", ""), ";plan=armed"));

  // Radio silence refuses the plan commands like everything else.
  assert(controller.HandleCommandLine("RADIO_SILENCE", "").rfind("ACK,", 0) == 0);
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_DISARM", ""), "radio silence active"));
  assert(controller.HandleCommandLine("RADIO_RESUME", "").rfind("ACK,", 0) == 0);
  assert(ContainsText(controller.HandleCommandLine("FALLBACK_STATUS", ""), "state=armed"));

  // The armed plan survives a restart; DISARM clears it, on disk too.
  {
    coatheal::SystemController restarted(LoadRadioTestConfig(queue_dir));
    const std::string status = restarted.HandleCommandLine("FALLBACK_STATUS", "");
    assert(ContainsText(status, "state=armed;armed=1"));
    assert(ContainsText(status, "m0=800/5/50/pending;m1=600/3/0/pending"));
    assert(ContainsText(restarted.HandleCommandLine("STATUS", ""), ";plan=armed"));
    assert(restarted.HandleCommandLine("FALLBACK_DISARM", "") == "ACK,FALLBACK_DISARM,plan=none");
  }
  coatheal::SystemController third(LoadRadioTestConfig(queue_dir));
  const std::string after = third.HandleCommandLine("FALLBACK_STATUS", "");
  assert(ContainsText(after, "state=none;armed=0"));
  assert(ContainsText(after, "m0=800/5/50/pending"));

  // A corrupt file is ignored: no plan, and the controller still starts.
  {
    std::ofstream out(queue_dir / "fallback_plan.txt", std::ios::trunc);
    out << "state=armed\nm0=not,a,plan,line\n";
  }
  coatheal::SystemController corrupt(LoadRadioTestConfig(queue_dir));
  assert(ContainsText(corrupt.HandleCommandLine("FALLBACK_STATUS", ""), "state=none;armed=0;deadline_s=1800;deadline_started=0;m0=-;m1=-"));

  std::filesystem::remove_all(queue_dir);
}

void TestFallbackConfigValidation() {
  coatheal::OnboardConfig cfg;
  std::string error;
  assert(coatheal::LoadConfigFromIni(WriteTempConfig(), &cfg, &error));
  assert(cfg.fallback.bend_min_c == -40.0);
  assert(cfg.fallback.bend_max_c == 40.0);
  assert(cfg.fallback.bend_deadline_s == 1800.0);
  assert(cfg.fallback.landed_safe);

  coatheal::OnboardConfig custom;
  assert(coatheal::LoadConfigFromIni(
      WriteTempConfig("fallback.bend_min_c=-20\nfallback.bend_max_c=20\n"
                      "fallback.bend_deadline_s=600\nfallback.landed_safe=false\n"),
      &custom, &error));
  assert(custom.fallback.bend_min_c == -20.0);
  assert(custom.fallback.bend_max_c == 20.0);
  assert(custom.fallback.bend_deadline_s == 600.0);
  assert(!custom.fallback.landed_safe);

  // A fresh config per load: the parser fills the struct before validating,
  // so a rejected window would otherwise leak into the next case.
  coatheal::OnboardConfig inverted;
  assert(!coatheal::LoadConfigFromIni(
      WriteTempConfig("fallback.bend_min_c=10\nfallback.bend_max_c=5\n"), &inverted, &error));
  assert(error == "fallback.bend_max_c must be > fallback.bend_min_c");
  coatheal::OnboardConfig flat;
  assert(!coatheal::LoadConfigFromIni(
      WriteTempConfig("fallback.bend_min_c=5\nfallback.bend_max_c=5\n"), &flat, &error));
  assert(error == "fallback.bend_max_c must be > fallback.bend_min_c");
  coatheal::OnboardConfig negative;
  assert(!coatheal::LoadConfigFromIni(
      WriteTempConfig("fallback.bend_deadline_s=-1\n"), &negative, &error));
  assert(error == "fallback.bend_deadline_s must be >= 0");
}

}  // namespace

int main() {
  TestPidBoundsAndAntiWindup();
  TestHeaterSchedulerCap();
  TestHeaterSchedulerEnergyBudget();
  TestCommandParser();
  TestTelemetrySerializer();
  TestTelemetryQueuePersistenceAndAck();
  TestTelemetryQueueDeferredCompactionRetentionAndTornLines();
  TestDrainedQueueLeavesNothingToReplay();
  TestUnackedFramesStillSurviveRestart();
  TestConfigParsesReliabilityFields();
  TestConfigRejectsGpioCollisions();
  TestConfigRejectsReservedGpioCollisions();
  TestConfigRejectsRetiredMotorKeys();
  TestConfigRejectsOvercurrentCeiling();
  TestConfigRejectsFlatCurrentBound();
  TestConfigAcceptsFlatCurrentBoundary();
  TestSequentRtdConfigDefaultsAndParsing();
  TestSequentRtdConfigRejectsBadValues();
  TestPowerCapRejectsValuesAboveTheOwnerRule();
  TestPowerCapAcceptsTheOwnerValues();
  TestMax31865ConfigDefaultsAndParsing();
  TestMax31865ConfigRejectsBadValues();
  TestLegacySensorKeysAreRejected();
  TestStateTransitions();
  TestManualHeaterOverrideWithoutFloorControl();
  TestVacuumRegime();
  TestDiscoveryBeaconParser();
  TestCommandPeerCanSeedTelemetryTarget();
  TestRadioSilenceBlocksEverythingButResume();
  TestRadioSilencePersistsAcrossRestart();
  TestRadioSilenceGatesBeaconAndHelloReply();
  TestFallbackCommandParsing();
  TestFallbackPlanCommands();
  TestFallbackConfigValidation();

  std::cout << "All unit tests passed.\n";
  return 0;
}
