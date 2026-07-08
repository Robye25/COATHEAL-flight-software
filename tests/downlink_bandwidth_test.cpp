// Simulates a realistic 1 Hz telemetry stream for 60 s and asserts the
// serialized downlink bandwidth stays below the E-Link budget with margin.
// Target: < 100_000 bps (well under the 2 Mbps nominal E-Link budget).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "coatheal/status_flags.hpp"
#include "coatheal/telemetry.hpp"

namespace {

coatheal::TelemetryRecord MakeRealisticRecord(std::uint64_t seq) {
  coatheal::TelemetryRecord r;
  r.seq = seq;
  r.phase = coatheal::MissionPhase::kFloat;
  r.sensors.rtc_valid = true;
  r.sensors.timestamp_utc = "2026-04-13T12:00:00Z";
  r.sensors.ambient_temp_c = -55.23;
  r.sensors.ambient_pressure_mbar = 140.12;
  r.sensors.uv = 0.00012;
  // Rev C: 8 PT100 sample temperatures, 6 heater duties (5 W each).
  r.sensors.sample_temps_c = {
      -30.12, -30.23, -30.01, -30.30, -30.11, -30.22, -30.05, -30.33};
  r.sensors.sample_resistance_ohm = {
      100.00, 99.80, 99.55, 99.40, 99.20, 99.05, 0.0, 0.0};
  r.heater_duty = {0.250, 0.000, 0.250, 0.000, 0.000, 0.000};
  r.status.sd_ok = true;
  r.status.usb_ok = true;
  r.status.i2c_ok = true;
  r.status.spi_ok = true;
  r.status.link_ok = true;
  r.status.resistance_ok = true;
  return r;
}

}  // namespace

int main() {
  const std::string session_id = "coatheal-1718000000-123456";
  constexpr int kDurationSeconds = 60;
  constexpr int kFramesPerSecond = 1;
  constexpr std::uint64_t kBudgetBps = 100000;  // 100 kbps ceiling with margin

  std::uint64_t total_bytes = 0;
  for (int i = 0; i < kDurationSeconds * kFramesPerSecond; ++i) {
    const coatheal::TelemetryRecord record = MakeRealisticRecord(static_cast<std::uint64_t>(i));
    const std::string frame =
        coatheal::SerializeTelemetryDataFrame(record, session_id);
    // +1 for the newline framing the onboard appends before send().
    total_bytes += frame.size() + 1;
  }

  const double bps = (static_cast<double>(total_bytes) * 8.0) /
                     static_cast<double>(kDurationSeconds);

  std::cout << "[downlink_bw] total_bytes=" << total_bytes
            << " duration_s=" << kDurationSeconds
            << " bps=" << bps
            << " budget_bps=" << kBudgetBps << '\n';

  if (bps >= static_cast<double>(kBudgetBps)) {
    std::cerr << "[downlink_bw] FAIL: bandwidth " << bps
              << " bps exceeds budget " << kBudgetBps << " bps\n";
    return 1;
  }

  // Sanity: frames must be non-empty.
  assert(total_bytes > 0);

  std::cout << "[downlink_bw] PASS\n";
  return 0;
}
