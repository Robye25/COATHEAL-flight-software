#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/pid_controller.hpp"
#include "coatheal/telemetry.hpp"

namespace coatheal {

struct ControlOverrides {
  bool heaters_off = false;
  std::optional<std::pair<std::size_t, double>> single_heater_override;
  std::optional<double> all_heaters_override;
  std::optional<PidGains> pid_override;
};

class ThermalController {
 public:
  explicit ThermalController(const OnboardConfig& config);

  void Reset();

  std::vector<double> ComputeRequestedDuty(MissionPhase phase,
                                           const SensorSnapshot& sensors,
                                           double dt_seconds,
                                           const ControlOverrides& overrides);

  void UpdatePid(PidGains gains);

  // Per-channel over-temperature latch: true while any channel has tripped the
  // explicit per-channel cutoff. Cleared only by Reset() (RESET_CONTROL).
  bool overtemp_latched() const { return overtemp_latched_; }

  // True while sample spread exceeded uniformity_tolerance_c during kFloatHold
  // on the most recent tick.
  bool uniformity_ok() const { return uniformity_ok_; }

  // Per-channel latch vector, sized to heater_count. Indices 0..N-1 map to
  // sample channels; the electronics heater tracks max_box_temp_c.
  const std::vector<bool>& channel_latched() const { return channel_latched_; }

 private:
  double ComputeSampleSetpoint(MissionPhase phase, double dt_seconds);

  OnboardConfig config_;
  PidController sample_pid_;
  PidController box_pid_;
  double activation_setpoint_c_ = 0.0;
  std::vector<bool> channel_latched_;
  bool overtemp_latched_ = false;
  bool uniformity_ok_ = true;
};

}  // namespace coatheal
