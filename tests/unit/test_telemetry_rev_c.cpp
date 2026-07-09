// Rev C telemetry serializer coverage.
//
// Exercises `SerializeTelemetryDataFrame` + `SerializeTelemetryPullEventFrame`
// against the wire contract:
//
//   * 8 sample_i columns, heater_count (=6) HEATER_DUTY= values;
//   * no humidity column, no box_temp column;
//   * RESISTANCE= carries 8 pipe-separated values; unmeasured samples "-";
//   * dual STEPPER0=/STEPPER1= path when `record.steppers` is populated;
//   * STATUS bitfield includes RS485, HEATER_{INHIBITED,ACTIVE}, and the
//     new RESISTANCE_{OK,FAIL} suffix;
//   * EVT,PULL frame round-trips with both populated and empty samples.

#include <cassert>
#include <cstdint>
#include <string>

#include "coatheal/telemetry.hpp"

using namespace coatheal;

namespace {

TelemetryRecord MakeBaseRecord() {
  TelemetryRecord r;
  r.seq = 42;
  r.phase = MissionPhase::kFloat;
  r.mode = SystemMode::kRun;
  r.sensors.rtc_valid = true;
  r.sensors.timestamp_utc = "2026-04-16T12:00:00Z";
  r.sensors.ambient_temp_c = -10.23;
  r.sensors.ambient_pressure_mbar = 140.12;
  r.sensors.uv = 0.00012;
  // Rev C: 8 sample temps, 6 heater duties.
  r.sensors.sample_temps_c = {5.1, 5.2, 5.0, 5.3, 5.1, 5.2, 5.0, 5.3};
  // Rev C keeps the compatibility resistance field. Values of 0.0 serialize
  // as "-" because the final BOM has no resistance instrument.
  r.sensors.sample_resistance_ohm = {100.0, 99.0, 98.5, 98.0, 97.5, 97.0, 0.0, 0.0};
  r.heater_duty = {0.25, 0.0, 0.25, 0.0, 0.0, 0.0};
  // Default flags are already mostly `true`; make sure the compatibility
  // additions are at their default so the wire bit count is exercised.
  r.status.rs485_ok = true;
  r.status.heater_inhibited = false;
  r.status.resistance_ok = true;
  return r;
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

void TestDataFrameLacksHumidityAndBoxTemp() {
  TelemetryRecord r = MakeBaseRecord();
  StepperStatus m0;
  m0.position_steps = 10;
  r.steppers = {m0};
  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  // No humidity, no box_temp columns anywhere.
  // The pre-sample fields are now: rtc_valid,ambient_temp,pressure,uv
  // followed immediately by the 8 sample columns.
  // Quick structural check: the 5th through 8th comma-delimited fields
  // should be the four non-sample scalar fields (ts/rtc/temp/pressure/uv).
  // We count commas up to the HEATER_DUTY marker.
  // Count commas before HEATER_DUTY= token.
  const std::size_t h = line.find("HEATER_DUTY=");
  assert(h != std::string::npos);
  std::size_t commas_before = 0;
  for (std::size_t i = 0; i < h; ++i) if (line[i] == ',') ++commas_before;
  // Expected leading commas up to HEATER_DUTY=:
  //   DATA<1>sess-b<2>seq<3>ts<4>rtc<5>t<6>p<7>uv<8..15>sample_0..7<16>HEATER_DUTY=
  // That is 16 commas total *including* the leading one before HEATER_DUTY.
  assert(commas_before == 16);
}

void TestResistanceColumn() {
  TelemetryRecord r = MakeBaseRecord();
  StepperStatus m0;
  r.steppers = {m0};
  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  // Six measured values (100..97) plus two "-" placeholders.
  assert(Contains(line, "RESISTANCE=100.000|99.000|98.500|98.000|97.500|97.000|-|-"));
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
  assert(Contains(line, "HEATER_INHIBITED"));
  // Heater-duty count should be 6 in the wire form.
  assert(Contains(line, "HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.000"));
  assert(Contains(line, "RS485_OK"));
  assert(Contains(line, "RESISTANCE_OK"));
}

void TestResistanceFailStatus() {
  TelemetryRecord r = MakeBaseRecord();
  r.status.resistance_ok = false;
  // With the instrument down, the simulator/controller would have zeroed
  // the resistance vector; test the wire form.
  r.sensors.sample_resistance_ohm.assign(8, 0.0);
  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  assert(Contains(line, "RESISTANCE_FAIL"));
  assert(Contains(line, "RESISTANCE=-|-|-|-|-|-|-|-"));
}

void TestHealthMetadataSerialization() {
  TelemetryRecord r = MakeBaseRecord();
  r.sensors.ambient_temp_valid = false;
  r.sensors.ambient_temp_age_ms = -1;
  r.sensors.sample_temp_valid =
      {false, true, false, false, false, false, false, false};
  r.sensors.sample_temp_age_ms =
      {-1, 125, -1, -1, -1, -1, -1, -1};
  r.sensors.dps310.state = ComponentState::kFailed;
  r.sensors.ads1115.state = ComponentState::kOk;
  r.sensors.daq132m.state = ComponentState::kDegraded;
  r.pwm_state = ComponentState::kDegraded;
  StepperStatus m0;
  m0.healthy = true;
  m0.missed_deadlines = 3;
  StepperStatus m1;
  r.steppers = {m0, m1};
  const std::string line = SerializeTelemetryDataFrame(r, "sess-health");
  assert(Contains(line, "SENSOR_VALID=AT:0|AP:1|UV:1|S0:0|S1:1"));
  assert(Contains(line, "SENSOR_AGE_MS=AT:-1|AP:-1|UV:-1|S0:-1|S1:125"));
  assert(Contains(line, "COMPONENT_STATE=DPS310:FAILED|ADS1115:OK|DAQ132M:DEGRADED"));
  assert(Contains(line, "|MOTOR0:OK|MOTOR1:FAILED|PWM:DEGRADED"));
  assert(Contains(line, "|missed:3|"));
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
  TestDataFrameLacksHumidityAndBoxTemp();
  TestResistanceColumn();
  TestDualStepperEmitsIndexedSegments();
  TestResistanceFailStatus();
  TestHealthMetadataSerialization();
  TestPullEventFrameSerialization();
  TestPullEventEmptySamplesRendersDash();
  return 0;
}
