#pragma once

namespace coatheal {

struct PidGains {
  double kp = 0.0;
  double ki = 0.0;
  double kd = 0.0;
};

class PidController {
 public:
  PidController() = default;
  PidController(PidGains gains, double output_min, double output_max,
                double integral_min, double integral_max);

  void SetGains(PidGains gains);
  void Reset();

  double Update(double setpoint, double measured, double dt_seconds);

 private:
  PidGains gains_;
  double output_min_ = 0.0;
  double output_max_ = 1.0;
  double integral_min_ = -10.0;
  double integral_max_ = 10.0;
  double integral_ = 0.0;
  double prev_error_ = 0.0;
  bool has_prev_ = false;
};

}  // namespace coatheal