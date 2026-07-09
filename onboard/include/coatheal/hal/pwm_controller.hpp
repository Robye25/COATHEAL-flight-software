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
  virtual bool channel_healthy(std::size_t channel) const = 0;
  virtual std::size_t channel_count() const = 0;
  virtual std::size_t healthy_channel_count() const = 0;
};

class SimulatedPwmController : public PwmController {
 public:
  explicit SimulatedPwmController(std::size_t channels);

  bool SetDuty(std::size_t channel, double duty) override;
  bool healthy() const override { return true; }
  bool channel_healthy(std::size_t channel) const override {
    return channel < duty_.size();
  }
  std::size_t channel_count() const override { return duty_.size(); }
  std::size_t healthy_channel_count() const override { return duty_.size(); }

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
  bool channel_healthy(std::size_t channel) const override;
  std::size_t channel_count() const override { return duty_.size(); }
  std::size_t healthy_channel_count() const override;

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
  mutable std::mutex handles_mu_;
  std::vector<void*> line_handles_;

  void PwmLoop();
  bool WriteLine(std::size_t channel, bool on);
  void RetryMissingLines();
  void AllOff();
};

}  // namespace coatheal
