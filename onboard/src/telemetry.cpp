#include "coatheal/telemetry.hpp"

#include <iomanip>
#include <sstream>

namespace coatheal {

namespace {

void AppendStepperSegment(std::ostringstream& oss, const StepperStatus& st,
                          int motor_index) {
  // Rev-B wire format: `STEPPER<n>=pos:...|tgt:...|...`. The per-segment
  // schema is unchanged from the legacy single-stepper frame so ground
  // parsers can reuse one key-value splitter.
  oss << ",STEPPER" << motor_index
      << "=pos:" << st.position_steps
      << "|tgt:" << st.target_steps
      << "|hz:" << std::setprecision(2) << st.step_hz
      << "|us:" << st.microstep
      << "|en:" << (st.enabled ? 1 : 0)
      << "|mv:" << (st.moving ? 1 : 0)
      << "|hold:" << (st.holding ? 1 : 0)
      << "|hold_s:" << std::setprecision(2) << st.hold_remaining_s
      << "|pulses:" << st.pulses_total
      << "|src:" << (st.last_source.empty() ? std::string("-") : st.last_source);
}

}  // namespace

// Rev B.1 DATA-frame schema:
//   DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_temp_c>,
//        <ambient_pressure_mbar>,<uv>,<sample_0>...<sample_N>,
//        HEATER_DUTY=d0|d1|...,
//        RESISTANCE=r0|r1|...   (- for unmeasured samples),
//        PHASE=...,MODE=...,STATUS=...,
//        STEPPER0=...,STEPPER1=...
// Humidity and box_temp are gone (no BME280, no box sensor). RESISTANCE is
// new (INA3221 sample-resistance instrument).
std::string SerializeTelemetryDataFrame(const TelemetryRecord& record,
                                        const std::string& session_id) {
  std::ostringstream oss;
  oss << "DATA," << session_id << ',' << record.seq << ',' << record.sensors.timestamp_utc << ','
      << (record.sensors.rtc_valid ? 1 : 0) << ',' << std::fixed << std::setprecision(2)
      << record.sensors.ambient_temp_c << ',' << record.sensors.ambient_pressure_mbar << ','
      << record.sensors.uv;

  // Rev-B.1: 8 sample_i columns. Heater duty has heater_count (=6) values.
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

  // Rev B.1: RESISTANCE= carries one pipe-separated value per sample. The
  // two INA3221 chips cover 6 of 8 samples; unmeasured samples (6, 7) are
  // emitted as "-" so the column count always equals sample_temps_c.size().
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

  // Rev B.1 dual-stepper telemetry: one STEPPER<n>= segment per motor. The
  // legacy single-STEPPER= path has been retired — Rev B.1 is a breaking
  // wire change anyway, and ground parsers already accept the indexed form.
  for (std::size_t i = 0; i < record.steppers.size(); ++i) {
    AppendStepperSegment(oss, record.steppers[i], static_cast<int>(i));
  }
  return oss.str();
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
