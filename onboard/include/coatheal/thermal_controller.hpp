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
  bool floor_control_enabled = true;
  std::optional<std::pair<std::size_t, double>> single_heater_override;
  std::optional<double> all_heaters_override;
  std::vector<std::optional<double>> heater_duty_overrides;
  std::vector<std::optional<double>> temp_targets_c;
  std::vector<std::optional<PidGains>> pid_overrides;
  std::optional<PidGains> pid_override;
  bool bench_open_loop_heaters = false;
};

// Rev C per-sample thermal controller:
//   * Explicit operator targets run in any RUN phase.
//   * Untargeted channels use phase.sample_floor_c only during link fallback.
//   * The fallback PID uses a hysteresis dead-band around the floor target.
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

  // Returns a duty vector of size heater_count (=6). Index i is
  // the duty for heater i, which corresponds to sample[i].
  std::vector<double> ComputeRequestedDuty(MissionPhase phase,
                                           const SensorSnapshot& sensors,
                                           double dt_seconds,
                                           const ControlOverrides& overrides);

  void UpdatePid(PidGains gains);
  void UpdatePid(std::size_t channel, PidGains gains);

  // Per-channel over-temperature latch (kept from Rev A for defense-in-depth
  // even though the fallback thermal goal is a floor, not a ceiling).
  bool overtemp_latched() const { return overtemp_latched_; }

  // True while sample spread across the controlled (heated) samples stayed
  // within uniformity_tolerance_c this tick.
  bool uniformity_ok() const { return uniformity_ok_; }

  const std::vector<bool>& channel_latched() const { return channel_latched_; }

  // True while each per-sample PID is currently driving output.
  const std::vector<bool>& sample_heating() const { return sample_heating_; }
  const std::vector<std::optional<double>>& active_targets() const {
    return active_targets_c_;
  }

 private:
  bool ShouldHeatPhase(MissionPhase phase) const;

  OnboardConfig config_;
  std::vector<PidController> sample_pids_;
  std::vector<bool> channel_latched_;
  std::vector<bool> sample_heating_;
  std::vector<std::optional<double>> active_targets_c_;
  bool overtemp_latched_ = false;
  bool uniformity_ok_ = true;
};

}  // namespace coatheal
