#include "coatheal/telemetry.hpp"

#include <iomanip>
#include <sstream>

namespace coatheal {

namespace {

void AppendStepperSegment(std::ostringstream& oss, const StepperStatus& st,
                          int motor_index) {
  // Rev C wire format: `STEPPER<n>=pos:...|tgt:...|...`. The per-segment
  // schema is unchanged from the legacy single-stepper frame so ground
  // parsers can reuse one key-value splitter.
  oss << ",STEPPER" << motor_index
      << "=pos:" << st.position_steps
      << "|tgt:" << st.target_steps
      << "|hz:" << std::setprecision(2) << st.step_hz
      << "|us:" << st.microstep
      << "|ok:" << (st.healthy ? 1 : 0)
      << "|en:" << (st.enabled ? 1 : 0)
      << "|mv:" << (st.moving ? 1 : 0)
      << "|hold:" << (st.holding ? 1 : 0)
      << "|hold_s:" << std::setprecision(2) << st.hold_remaining_s
      << "|pulses:" << st.pulses_total
      << "|missed:" << st.missed_deadlines
      << "|src:" << (st.last_source.empty() ? std::string("-") : st.last_source)
      // Redesign spec §8: appended after src so older ground parsers, which
      // ignore unknown keys, keep working.
      << "|zeroed:" << (st.zeroed ? 1 : 0)
      << "|seq:" << (st.seq_name.empty() ? std::string("-") : st.seq_name)
      << "|seqst:" << (st.seq_state.empty() ? std::string("idle") : st.seq_state)
      // 2026-08-29 drive-settings surface: run current, ramp accel, and the
      // ball-screw-lead-derived linear position. Appended last so older
      // ground parsers, which ignore unknown keys, keep working.
      << "|amps:" << std::setprecision(2) << st.run_current_a_rms
      << "|acc:" << std::setprecision(1) << st.accel_steps_per_s2
      << "|mm:" << std::setprecision(3) << st.position_mm
      << "|mm_tgt:" << std::setprecision(3) << st.target_mm;
}

void AppendCtrlSegment(std::ostringstream& oss, const CtrlStatus& ctrl) {
  // Redesign spec §8: `CTRL=` sits between COMPONENT_STATE and STEPPER0.
  // Precision is fixed per key so the ground CSV round-trips byte-for-byte.
  oss << ",CTRL=fallback:" << (ctrl.fallback_active ? 1 : 0)
      << "|link_loss_s:" << std::fixed << std::setprecision(1) << ctrl.link_loss_s
      << "|energy_wh:" << std::setprecision(2) << ctrl.energy_wh
      << "|budget_wh:" << std::setprecision(1) << ctrl.budget_wh
      << "|budget_exhausted:" << (ctrl.budget_exhausted ? 1 : 0)
      << "|heaters_active:" << ctrl.heaters_active
      << "|queue:" << ctrl.queue_depth
      << "|plan:" << (ctrl.plan.empty() ? std::string("none") : ctrl.plan);
}

}  // namespace

// Rev C DATA-frame schema:
//   DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_temp_c>,
//        <ambient_pressure_mbar>,<uv>,<sample_0>...<sample_N>,
//        HEATER_DUTY=d0|d1|...,
//        RESISTANCE=r0|r1|...   (- for unmeasured samples),
//        PHASE=...,MODE=...,STATUS=...,SENSOR_VALID=...,SENSOR_AGE_MS=...,
//        COMPONENT_STATE=...,CTRL=...,
//        STEPPER0=...,STEPPER1=...
// Humidity and box_temp are not emitted. RESISTANCE's meaning follows
// sensor.resistance_source, whose v3 default is max31865_click: the two
// MAX31865 clicks' measured specimen resistance, written only into the two
// sensor.max31865_sample_indices slots (every other slot stays 0.0 and wires
// as "-"). The pre-v3 sequent_rtd source is still accepted and puts the
// Sequent RTD card's per-channel PT100 element resistance in every slot;
// "disabled" makes every slot "-".
std::string SerializeTelemetryDataFrame(const TelemetryRecord& record,
                                        const std::string& session_id) {
  std::ostringstream oss;
  oss << "DATA," << session_id << ',' << record.seq << ',' << record.sensors.timestamp_utc << ','
      << (record.sensors.rtc_valid ? 1 : 0) << ',' << std::fixed << std::setprecision(2)
      << record.sensors.ambient_temp_c << ',' << record.sensors.ambient_pressure_mbar << ','
      << record.sensors.uv;

  // Final BOM: 8 sample_i columns. Heater duty has heater_count (=6) values.
  // The ground parser locates HEATER_DUTY= by token name, so the sample
  // count is inferred from position, not a hardcoded constant.
  for (double temp : record.sensors.sample_temps_c) {
    oss << ',' << temp;
  }

  oss << ",HEATER_DUTY=";
  for (std::size_t i = 0; i < record.heater_duty.size(); ++i) {
    if (i != 0) {
      oss << '|';
    }
    oss << std::setprecision(3) << record.heater_duty[i];
  }

  // RESISTANCE= carries one pipe-separated value per sample for compatibility.
  // Disabled or unavailable readings are emitted as "-".
  oss << ",RESISTANCE=";
  const std::size_t nres = record.sensors.sample_resistance_ohm.size();
  const std::size_t nsamp = record.sensors.sample_temps_c.size();
  for (std::size_t i = 0; i < nsamp; ++i) {
    if (i != 0) {
      oss << '|';
    }
    if (i < nres && record.sensors.sample_resistance_ohm[i] > 0.0) {
      oss << std::setprecision(3) << record.sensors.sample_resistance_ohm[i];
    } else {
      oss << '-';
    }
  }

  oss << ",PHASE=" << ToString(record.phase) << ",MODE=" << ToString(record.mode)
      << ",STATUS=" << ToStatusBitfield(record.status);

  oss << ",SENSOR_VALID=AT:"
      << (record.sensors.ambient_temp_valid ? 1 : 0)
      << "|AP:" << (record.sensors.ambient_pressure_valid ? 1 : 0)
      << "|UV:" << (record.sensors.uv_valid ? 1 : 0);
  for (std::size_t i = 0; i < record.sensors.sample_temps_c.size(); ++i) {
    const bool valid =
        i < record.sensors.sample_temp_valid.size() &&
        record.sensors.sample_temp_valid[i];
    oss << "|S" << i << ':' << (valid ? 1 : 0);
  }

  oss << ",SENSOR_AGE_MS=AT:" << record.sensors.ambient_temp_age_ms
      << "|AP:" << record.sensors.ambient_pressure_age_ms
      << "|UV:" << record.sensors.uv_age_ms;
  for (std::size_t i = 0; i < record.sensors.sample_temps_c.size(); ++i) {
    const std::int64_t age =
        i < record.sensors.sample_temp_age_ms.size()
            ? record.sensors.sample_temp_age_ms[i]
            : -1;
    oss << "|S" << i << ':' << age;
  }

  const bool motor0_ok =
      record.steppers.size() > 0 && record.steppers[0].healthy;
  const bool motor1_ok =
      record.steppers.size() > 1 && record.steppers[1].healthy;
  oss << ",COMPONENT_STATE=DPS310:" << ToString(record.sensors.dps310.state)
      << "|ADS1115:" << ToString(record.sensors.ads1115.state)
      << "|SEQUENT_RTD:" << ToString(record.sensors.sequent_rtd.state)
      << "|MOTOR0:" << (motor0_ok ? "OK" : "FAILED")
      << "|MOTOR1:" << (motor1_ok ? "OK" : "FAILED")
      << "|PWM:" << ToString(record.pwm_state);

  AppendCtrlSegment(oss, record.ctrl);

  // Dual-stepper telemetry: one STEPPER<n>= segment per motor.
  for (std::size_t i = 0; i < record.steppers.size(); ++i) {
    AppendStepperSegment(oss, record.steppers[i], static_cast<int>(i));
  }
  return oss.str();
}

std::string TagFrameForTransmit(const std::string& frame,
                                std::int64_t queued_epoch_s,
                                std::int64_t now_epoch_s) {
  if (frame.rfind("DATA,", 0) != 0) return frame;
  const std::int64_t age = now_epoch_s > queued_epoch_s ? now_epoch_s - queued_epoch_s : 0;
  return frame + ",TX=" + std::to_string(age);
}

std::string SerializeHeatingCycleEvent(const HeatingCycleEvent& event,
                                       const std::string& session_id) {
  std::ostringstream oss;
  oss << "EVT,CYCLE," << session_id << ',' << event.cycle_id << ',' << event.start_ts << ','
      << std::fixed << std::setprecision(2) << event.peak_temp_c << ','
      << std::setprecision(2) << event.hold_duration_s << ','
      << std::setprecision(4) << event.cooldown_rate_c_per_s << ','
      << event.specimen_index;
  return oss.str();
}

std::string SerializeTelemetryPullEventFrame(const HeatingPullEvent& event,
                                             const std::string& session_id) {
  // Pipe-separated specimen indices. We use "-" for an empty list so the
  // field is never empty (simpler CSV parsers happier that way).
  std::ostringstream samples;
  if (event.samples.empty()) {
    samples << '-';
  } else {
    for (std::size_t i = 0; i < event.samples.size(); ++i) {
      if (i != 0) samples << '|';
      samples << event.samples[i];
    }
  }
  std::ostringstream oss;
  oss << "EVT,PULL," << session_id << ',' << event.pull_id << ',' << event.motor_id
      << ',' << event.start_ts << ',' << event.steps_moved << ','
      << std::fixed << std::setprecision(2) << event.hold_s << ','
      << samples.str();
  return oss.str();
}

}  // namespace coatheal
