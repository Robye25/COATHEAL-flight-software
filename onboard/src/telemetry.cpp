#include "coatheal/telemetry.hpp"

#include <iomanip>
#include <sstream>

namespace coatheal {


std::string SerializeTelemetryDataFrame(const TelemetryRecord& record,
                                        const std::string& session_id) {
  std::ostringstream oss;
  oss << "DATA," << session_id << ',' << record.seq << ',' << record.sensors.timestamp_utc << ','
      << (record.sensors.rtc_valid ? 1 : 0) << ',' << std::fixed << std::setprecision(2)
      << record.sensors.ambient_temp_c << ',' << record.sensors.ambient_pressure_mbar << ','
      << record.sensors.ambient_humidity_pct << ',' << record.sensors.uv << ','
      << record.sensors.box_temp_c;

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

  oss << ",PHASE=" << ToString(record.phase) << ",MODE=" << ToString(record.mode)
      << ",STATUS=" << ToStatusBitfield(record.status);
  return oss.str();
}

}  // namespace coatheal
