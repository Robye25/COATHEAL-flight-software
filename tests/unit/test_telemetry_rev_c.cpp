// Rev C telemetry serializer coverage.
//
// Exercises `SerializeTelemetryDataFrame` + `SerializeTelemetryPullEventFrame`
// against the wire contract:
//
//   * 8 sample_i columns, heater_count (=6) HEATER_DUTY= values;
//   * no humidity column, no box_temp column;
//   * RESISTANCE= carries 8 pipe-separated values; unmeasured samples "-";
//   * dual STEPPER0=/STEPPER1= path when `record.steppers` is populated;
//   * STATUS bitfield has no RS485 term (removed with the DAQ132M path),
//     includes HEATER_{INHIBITED,ACTIVE}, and the RESISTANCE_{OK,FAIL} suffix;
//   * COMPONENT_STATE carries a single SEQUENT_RTD term, not DAQ132M/RTD_CLICK;
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
  assert(Contains(line, "RESISTANCE_OK"));
  assert(!Contains(line, "RS485"));
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
  r.sensors.sequent_rtd.state = ComponentState::kDegraded;
  r.pwm_state = ComponentState::kDegraded;
  StepperStatus m0;
  m0.healthy = true;
  m0.missed_deadlines = 3;
  StepperStatus m1;
  r.steppers = {m0, m1};
  const std::string line = SerializeTelemetryDataFrame(r, "sess-health");
  assert(Contains(line, "SENSOR_VALID=AT:0|AP:1|UV:1|S0:0|S1:1"));
  assert(Contains(line, "SENSOR_AGE_MS=AT:-1|AP:-1|UV:-1|S0:-1|S1:125"));
  assert(Contains(line, "COMPONENT_STATE=DPS310:FAILED|ADS1115:OK|SEQUENT_RTD:DEGRADED"));
  assert(Contains(line, "|MOTOR0:OK|MOTOR1:FAILED|PWM:DEGRADED"));
  assert(Contains(line, "|missed:3|"));
  assert(!Contains(line, "DAQ132M"));
  assert(!Contains(line, "RTD_CLICK"));
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

void TestComponentStateUsesSequentRtdToken() {
  TelemetryRecord record;
  record.sensors.sample_temps_c.assign(8, 20.0);
  record.sensors.sample_temp_valid.assign(8, true);
  record.sensors.sample_temp_age_ms.assign(8, 0);
  record.sensors.sample_resistance_ohm.assign(8, 107.79);
  record.sensors.dps310.state = ComponentState::kOk;
  record.sensors.ads1115.state = ComponentState::kOk;
  record.sensors.sequent_rtd.state = ComponentState::kDegraded;
  record.heater_duty.assign(6, 0.0);
  record.steppers.resize(2);

  const std::string frame = SerializeTelemetryDataFrame(record, "sess-1");
  assert(frame.find("SEQUENT_RTD:DEGRADED") != std::string::npos);
  assert(frame.find("RTD_CLICK") == std::string::npos);
  assert(frame.find("DAQ132M") == std::string::npos);
}

void TestCtrlBlockAndStepperExtrasTokenOrder() {
  // Redesign spec §8: `CTRL=` sits between COMPONENT_STATE and STEPPER0,
  // and the three new stepper keys trail `src:` so older ground parsers
  // (which ignore unknown keys) keep working. The exact strings are the
  // contract the ground station's parser and CSV writer are built on.
  TelemetryRecord r = MakeBaseRecord();
  r.ctrl.fallback_active = true;
  r.ctrl.link_loss_s = 12.5;
  r.ctrl.energy_wh = 3.25;
  r.ctrl.budget_wh = 130.0;
  r.ctrl.budget_exhausted = false;
  r.ctrl.heaters_active = 2;
  r.ctrl.queue_depth = 7;
  StepperStatus m0;
  m0.position_steps = 100;
  m0.last_source = "cmd:MOVE";
  m0.zeroed = true;
  m0.seq_name = "flex";
  m0.seq_state = "run";
  // Drive-settings surface (2026-08-29): trails seqst for the same
  // forward-compat reason the seqst trio trails src.
  m0.run_current_a_rms = 0.8;
  m0.accel_steps_per_s2 = 200.0;
  m0.position_mm = 0.25;
  m0.target_mm = 0.5;
  StepperStatus m1;  // defaults: never zeroed, no sequence, no source
  r.steppers = {m0, m1};

  const std::string line = SerializeTelemetryDataFrame(r, "sess-b");
  assert(Contains(line,
                  ",CTRL=fallback:1|link_loss_s:12.5|energy_wh:3.25|budget_wh:130.0"
                  "|budget_exhausted:0|heaters_active:2|queue:7|plan:none,STEPPER0="));
  assert(Contains(line, "|src:cmd:MOVE|zeroed:1|seq:flex|seqst:run"
                        "|amps:0.80|acc:200.0|mm:0.250|mm_tgt:0.500,STEPPER1="));
  assert(Contains(line, "|src:-|zeroed:0|seq:-|seqst:idle"
                        "|amps:0.00|acc:0.0|mm:0.000|mm_tgt:0.000"));
  assert(line.find("COMPONENT_STATE=") < line.find(",CTRL="));

  // An empty plan string still serialises as the documented default word.
  r.ctrl.plan.clear();
  assert(Contains(SerializeTelemetryDataFrame(r, "sess-b"), "|plan:none,STEPPER0="));
}

void TestStatusFlagsDropRs485() {
  // RS485 hardware leaves with the DAQ-132M. A flag that can only ever read
  // OK is worse than no flag, so it must be gone from the wire, and its
  // removal must not leave a broken separator between its neighbours (the
  // real regression this migration risks: a dangling or doubled '|').
  StatusFlags flags;
  const std::string encoded = ToStatusBitfield(flags);
  assert(encoded.find("RS485") == std::string::npos);
  assert(Contains(encoded, "ENERGY_OK|PWM_OK"));
  assert(!Contains(encoded, "||"));
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
  TestComponentStateUsesSequentRtdToken();
  TestCtrlBlockAndStepperExtrasTokenOrder();
  TestStatusFlagsDropRs485();
  return 0;
}
