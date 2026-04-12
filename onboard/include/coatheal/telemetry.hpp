#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coatheal/phase.hpp"
#include "coatheal/status_flags.hpp"
#include "coatheal/system_mode.hpp"

namespace coatheal {

struct SensorSnapshot {
  bool rtc_valid = true;
  std::string timestamp_utc;
  double ambient_temp_c = 0.0;
  double ambient_pressure_mbar = 1013.25;
  double ambient_humidity_pct = 0.0;
  double uv = 0.0;
  double box_temp_c = 0.0;
  std::vector<double> sample_temps_c;
};

struct TelemetryRecord {
  std::uint64_t seq = 0;
  MissionPhase phase = MissionPhase::kAscentHold;
  SystemMode mode = SystemMode::kStandby;
  SensorSnapshot sensors;
  std::vector<double> heater_duty;
  StatusFlags status;
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

}  // namespace coatheal
