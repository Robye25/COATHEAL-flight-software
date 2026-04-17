#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/pid_controller.hpp"

namespace coatheal {

struct SensorSnapshot;  // defined in telemetry.hpp; forward-declared here
                        // so this header does not transitively include the
                        // telemetry/stepper headers (owned by other agents).

struct ControlOverrides {
  bool heaters_off = false;
  std::optional<std::pair<std::size_t, double>> single_heater_override;
  std::optional<double> all_heaters_override;
  std::optional<PidGains> pid_override;
};

// Rev B.1 floor controller:
//   * Per-sample PID setpoint = phase.sample_floor_c (shared across
//     ASCENT/FLOAT/DESCENT).
//   * PID is only active when sample < (floor - hysteresis); once sample
//     reaches floor it switches off, duty goes to 0, and the integrator is
//     frozen. kFloorHysteresisC defines the dead-band.
//   * 6 heaters drive samples 0..5 1:1 (heater[i] <-> sample[i]). Samples 6
//     and 7 are pulled but unheated — PT100 measured only.
//   * No electronics-box heater. `electronics_heater_index == SIZE_MAX` is
//     the sentinel; no box PID exists.
//   * kBoot / kLanded / kStopped force zero output.
class ThermalController {
 public:
  static constexpr double kFloorHysteresisC = 0.5;

  explicit ThermalController(const OnboardConfig& config);

  void Reset();

  // Returns a duty vector of size heater_count (=6 in Rev B.1). Index i is
  // the duty for heater i, which corresponds to sample[i].
  std::vector<double> ComputeRequestedDuty(MissionPhase phase,
                                           const SensorSnapshot& sensors,
                                           double dt_seconds,
                                           const ControlOverrides& overrides);

  void UpdatePid(PidGains gains);

  // Per-channel over-temperature latch (kept from Rev A for defense-in-depth
  // even though the Rev B thermal goal is a floor, not a ceiling).
  bool overtemp_latched() const { return overtemp_latched_; }

  // True while sample spread across the controlled (heated) samples stayed
  // within uniformity_tolerance_c this tick.
  bool uniformity_ok() const { return uniformity_ok_; }

  const std::vector<bool>& channel_latched() const { return channel_latched_; }

  // True while each per-sample PID is currently driving output (below
  // floor - hysteresis). Exposed for tests and telemetry.
  const std::vector<bool>& sample_heating() const { return sample_heating_; }

 private:
  bool ShouldHeatPhase(MissionPhase phase) const;

  OnboardConfig config_;
  std::vector<PidController> sample_pids_;
  std::vector<bool> channel_latched_;
  std::vector<bool> sample_heating_;
  bool overtemp_latched_ = false;
  bool uniformity_ok_ = true;
};

}  // namespace coatheal
