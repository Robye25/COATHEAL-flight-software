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

 private:
  double ComputeSampleSetpoint(MissionPhase phase, double dt_seconds);

  OnboardConfig config_;
  PidController sample_pid_;
  PidController box_pid_;
  double activation_setpoint_c_ = 0.0;
};

}  // namespace coatheal
