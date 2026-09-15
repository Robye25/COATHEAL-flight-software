// A realistic 1 Hz telemetry frame against the 24 kbps E-Link budget
// (docs/link-budget.md): what one frame holds while it is out -- the frame,
// the ground station's TCP ACK and ACK line, our ACK, and the resets an abort
// could cost -- and what stays counted once it was answered, compressed and
// plain. test_link_budget.cpp replays whole outages against the same model;
// this pins the per-frame numbers the budget split was sized on.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "coatheal/link_budget.hpp"
#include "coatheal/status_flags.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_codec.hpp"

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
  const std::string session_id = "coatheal-1789498045-582267";
  const std::string frame = coatheal::TagFrameForTransmit(
      coatheal::SerializeTelemetryDataFrame(MakeRealisticRecord(123456), session_id), 0, 0);

  // What stays counted after a timely answer that carried the TCP ACK.
  const auto settled = [](std::uint32_t cost) {
    return cost - 2 * coatheal::wire::kReset - coatheal::wire::kPureAck;
  };

  const std::size_t plain_payload = frame.size() + 1;
  const std::uint32_t plain_cost = coatheal::TelemetryClient::FrameCostBytes(frame, plain_payload);
  std::cout << "[downlink_bw] plain frame " << plain_payload << " B: holds " << plain_cost
            << " B, " << settled(plain_cost) << " B once answered\n";
  // This record fits the share on its own; a full bench frame (about 1.1 kB)
  // does not, which is why the codec is required.
  assert(plain_cost <= coatheal::kOnboardShareBytes);

  if (coatheal::TelemetryCodecAvailable()) {
    const std::string z1 = coatheal::EncodeTelemetryLineZ1(frame);
    assert(!z1.empty());
    const std::size_t z1_payload = z1.size() + 1;
    const std::uint32_t z1_cost = coatheal::TelemetryClient::FrameCostBytes(frame, z1_payload);
    std::cout << "[downlink_bw] z1 frame " << z1_payload << " B: holds " << z1_cost << " B, "
              << settled(z1_cost) << " B once answered; share " << coatheal::kOnboardShareBytes
              << " B per second\n";
    // A replayed frame fits next to an answered live frame in one second.
    assert(settled(z1_cost) + z1_cost <= coatheal::kOnboardShareBytes);
    // One compressed frame per second, answers included: under 8 kbps.
    assert(settled(z1_cost) * 8 <= 8000);
  }

  std::cout << "[downlink_bw] PASS\n";
  return 0;
}
