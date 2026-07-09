#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coatheal/component_health.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/status_flags.hpp"
#include "coatheal/stepper_controller.hpp"
#include "coatheal/system_mode.hpp"

namespace coatheal {

struct SensorSnapshot {
  bool rtc_valid = true;
  std::string timestamp_utc;
  double ambient_temp_c = 0.0;
  double ambient_pressure_mbar = 0.0;
  double uv = 0.0;
  std::vector<double> sample_temps_c;
  bool ambient_temp_valid = true;
  bool ambient_pressure_valid = true;
  bool uv_valid = true;
  bool sample_temps_valid = true;
  std::vector<bool> sample_temp_valid;
  std::int64_t ambient_temp_age_ms = -1;
  std::int64_t ambient_pressure_age_ms = -1;
  std::int64_t uv_age_ms = -1;
  std::vector<std::int64_t> sample_temp_age_ms;
  ComponentHealth dps310;
  ComponentHealth ads1115;
  ComponentHealth daq132m;
  bool simulated = false;
  // Compatibility field. The final Rev C BOM has no resistance instrument, so
  // disabled readings are reported as 0.0 here and serialized as "-".
  std::vector<double> sample_resistance_ohm;
};

struct TelemetryRecord {
  std::uint64_t seq = 0;
  MissionPhase phase = MissionPhase::kBoot;
  SystemMode mode = SystemMode::kStandby;
  SensorSnapshot sensors;
  std::vector<double> heater_duty;
  StatusFlags status;
  ComponentState pwm_state = ComponentState::kFailed;
  // Vector of motor snapshots. `steppers[0]` = M0, `steppers[1]` = M1.
  // Each motor is emitted as a `STEPPER<n>=...` segment on the wire.
  std::vector<StepperStatus> steppers;
};

std::string SerializeTelemetryDataFrame(const TelemetryRecord& record,
                                        const std::string& session_id);

struct HeatingCycleEvent {
  std::uint32_t cycle_id = 0;
  std::string start_ts;               // UTC ISO-8601
  double peak_temp_c = 0.0;
  double hold_duration_s = 0.0;
  double cooldown_rate_c_per_s = 0.0; // positive = cooling
  std::size_t specimen_index = 0;
};

// Newline-terminated, CSV-style with a distinct prefix so the ground parser
// can route it separately from DATA frames.
// Format: EVT,CYCLE,<session_id>,<cycle_id>,<start_ts>,<peak_temp_c>,
//         <hold_duration_s>,<cooldown_rate_c_per_s>,<specimen_index>
std::string SerializeHeatingCycleEvent(const HeatingCycleEvent& event,
                                       const std::string& session_id);

// One-per-pull completion event emitted by the stepper subsystem.
// A "pull" is a single bend-and-hold motion applied to one or more specimens.
// `samples` carries the specimen indices touched by the pull; the ground
// wire encoding is pipe-separated (e.g. "0|1|2|3").
struct HeatingPullEvent {
  std::uint32_t pull_id = 0;
  int motor_id = 0;                   // 0 = M0, 1 = M1
  std::string start_ts;               // UTC ISO-8601
  std::int64_t steps_moved = 0;       // signed, = final_pos - start_pos
  double hold_s = 0.0;                // time held at target
  std::vector<std::size_t> samples;   // specimen indices the pull covered
};

// Format: EVT,PULL,<session_id>,<pull_id>,<motor_id>,<start_ts>,
//         <steps_moved>,<hold_s>,<samples>
// where <samples> is pipe-separated ("0|1|2|3"); empty samples => "-".
std::string SerializeTelemetryPullEventFrame(const HeatingPullEvent& event,
                                             const std::string& session_id);

}  // namespace coatheal
