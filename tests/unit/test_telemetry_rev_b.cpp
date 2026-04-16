// Rev-B telemetry serializer coverage.
//
// Exercises `SerializeTelemetryDataFrame` + `SerializeTelemetryPullEventFrame`
// against the wire contract documented in `docs/protocol.md`:
//
//   * 8 sample_i columns, 9 HEATER_DUTY= values (8 sample + 1 BOX);
//   * legacy single-STEPPER path when `record.steppers` is empty;
//   * dual STEPPER0=/STEPPER1= path when `record.steppers` is populated;
//   * STATUS bitfield includes the new RS485 + HEATER_{INHIBITED,ACTIVE}
//     suffixes;
//   * EVT,PULL frame round-trips with both populated and empty samples.
//
// Kept in its own translation unit so `tests/unit/test_suite.cpp` is not
// touched (that file is owned by another agent).

#include <cassert>
#include <cstdint>
#include <string>

#include "coatheal/telemetry.hpp"

using namespace coatheal;

namespace {

TelemetryRecord MakeBaseRecord() {
  TelemetryRecord r;
  r.seq = 42;
  r.phase = MissionPhase::kFloatHold;
  r.mode = SystemMode::kRun;
  r.sensors.rtc_valid = true;
  r.sensors.timestamp_utc = "2026-04-16T12:00:00Z";
  r.sensors.ambient_temp_c = -10.23;
  r.sensors.ambient_pressure_mbar = 140.12;
  r.sensors.ambient_humidity_pct = 12.4;
  r.sensors.uv = 0.00012;
  r.sensors.box_temp_c = 3.10;
  // Rev-B: 8 sample temps.
  r.sensors.sample_temps_c = {5.1, 5.2, 5.0, 5.3, 5.1, 5.2, 5.0, 5.3};
  // Rev-B: 9 heater duties (8 sample + BOX).
  r.heater_duty = {0.25, 0.0, 0.25, 0.0, 0.0, 0.0, 0.0, 0.0, 0.05};
  // Default flags are already mostly `true`; make sure the Rev-B additions
  // are at their default so the wire bit count is exercised.
  r.status.rs485_ok = true;
  r.status.heater_inhibited = false;
  return r;
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

void TestLegacySingleStepperStillWorks() {
  TelemetryRecord r = MakeBaseRecord();
  r.stepper.position_steps = 100;
  r.stepper.target_steps = 200;
  r.stepper.step_hz = 400.0;
  r.stepper.microstep = 16;
  r.stepper.enabled = true;
  r.stepper.moving = true;
  r.stepper.last_source = "cmd:MOVE";
  // Do NOT populate `r.steppers` — serializer must emit legacy STEPPER=.
  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  assert(Contains(line, ",STEPPER=pos:100|tgt:200|hz:400"));
  assert(!Contains(line, ",STEPPER0=")); // no indexed form
  assert(!Contains(line, ",STEPPER1="));
  // Heater count is 9 in the wire form.
  assert(Contains(line, "HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.000|0.000|0.000|0.050"));
  // STATUS contains the Rev-B bits.
  assert(Contains(line, "RS485_OK"));
  assert(Contains(line, "HEATER_ACTIVE"));
}

void TestDualStepperEmitsIndexedSegments() {
  TelemetryRecord r = MakeBaseRecord();
  StepperStatus m0;
  m0.position_steps = 100; m0.target_steps = 200; m0.step_hz = 400.0;
  m0.microstep = 16; m0.enabled = true; m0.moving = true;
  m0.last_source = "cmd:MOVE";
  StepperStatus m1;
  m1.position_steps = -50; m1.target_steps = -50; m1.step_hz = 200.0;
  m1.microstep = 8; m1.enabled = true; m1.holding = true;
  m1.hold_remaining_s = 3.5; m1.pulses_total = 50;
  m1.last_source = "phase:FLOAT";
  r.steppers = {m0, m1};
  r.status.heater_inhibited = true;

  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  assert(Contains(line, ",STEPPER0=pos:100|tgt:200"));
  assert(Contains(line, ",STEPPER1=pos:-50|tgt:-50"));
  assert(!Contains(line, ",STEPPER=pos:")); // unindexed form suppressed
  assert(Contains(line, "HEATER_INHIBITED"));
  // Sample count via comma count: 10 leading fixed columns + 8 samples
  // + HEATER_DUTY + PHASE + MODE + STATUS + STEPPER0 + STEPPER1 = 23 commas.
  std::size_t commas = 0;
  for (char c : line) if (c == ',') ++commas;
  assert(commas == 23);
}

void TestPullEventFrameSerialization() {
  HeatingPullEvent ev;
  ev.pull_id = 3;
  ev.motor_id = 1;
  ev.start_ts = "2026-04-16T10:21:00Z";
  ev.steps_moved = 2400;
  ev.hold_s = 12.0;
  ev.samples = {0, 1, 2, 3};
  const std::string line = SerializeTelemetryPullEventFrame(ev, "sess-b");
  assert(Contains(line, "EVT,PULL,sess-b,3,1,2026-04-16T10:21:00Z,2400,12.00,0|1|2|3"));
}

void TestPullEventEmptySamplesRendersDash() {
  HeatingPullEvent ev;
  ev.pull_id = 4;
  ev.motor_id = 0;
  ev.start_ts = "2026-04-16T10:22:00Z";
  ev.steps_moved = -1200;
  ev.hold_s = 0.0;
  // samples intentionally empty — renderer must emit "-" not "".
  const std::string line = SerializeTelemetryPullEventFrame(ev, "sess-b");
  assert(Contains(line, "EVT,PULL,sess-b,4,0,2026-04-16T10:22:00Z,-1200,0.00,-"));
}

}  // namespace

int main() {
  TestLegacySingleStepperStillWorks();
  TestDualStepperEmitsIndexedSegments();
  TestPullEventFrameSerialization();
  TestPullEventEmptySamplesRendersDash();
  return 0;
}
