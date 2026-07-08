#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
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
  bool healthy() const override { return healthy_.load(); }

 private:
  std::string chip_;
  std::vector<std::size_t> output_lines_;
  double pwm_frequency_hz_ = 10.0;
  bool active_high_ = true;
  std::vector<double> duty_;
  std::atomic<bool> healthy_{false};
  std::atomic<bool> running_{false};
  std::thread worker_;
  mutable std::mutex mu_;
  void* chip_handle_ = nullptr;
  std::vector<void*> line_handles_;

  void PwmLoop();
  bool WriteLine(std::size_t channel, bool on);
  void AllOff();
};

}  // namespace coatheal
