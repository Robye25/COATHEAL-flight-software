#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace coatheal {

class PwmController {
 public:
  virtual ~PwmController() = default;
  virtual bool SetDuty(std::size_t channel, double duty) = 0;
  virtual bool healthy() const = 0;
};

class SimulatedPwmController : public PwmController {
 public:
  explicit SimulatedPwmController(std::size_t channels);

  bool SetDuty(std::size_t channel, double duty) override;
  bool healthy() const override { return true; }

  const std::vector<double>& duty() const { return duty_; }

 private:
  std::vector<double> duty_;
};

class LibgpiodPwmController : public PwmController {
 public:
  LibgpiodPwmController(std::string chip,
                        std::size_t channels,
                        std::vector<std::size_t> output_lines = {},
                        double pwm_frequency_hz = 10.0,
                        bool active_high = true);
  ~LibgpiodPwmController() override;

  bool SetDuty(std::size_t channel, double duty) override;
  bool healthy() const override { return healthy_; }

 private:
  std::string chip_;
  std::vector<std::size_t> output_lines_;
  double pwm_frequency_hz_ = 10.0;
  bool active_high_ = true;
  std::vector<double> duty_;
  bool healthy_ = false;
};

}  // namespace coatheal
