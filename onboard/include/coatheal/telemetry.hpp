#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coatheal/phase.hpp"
#include "coatheal/status_flags.hpp"

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
  SensorSnapshot sensors;
  std::vector<double> heater_duty;
  StatusFlags status;
};

std::string SerializeTelemetryDataFrame(const TelemetryRecord& record,
                                        const std::string& session_id);

}  // namespace coatheal
