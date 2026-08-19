#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace coatheal {

// Renders ONE software-PWM period of `slices` slices across `channels`
// lines.
//
// `duty_at(channel)` is called once per slice per channel -- NOT once per
// period. That is the load-bearing property, not a style choice. Heaters run
// at `heater.pwm_frequency_hz = 1.0`, so one period is a full SECOND;
// sampling the duty once per period meant a `SetDuty(0)` from
// `SystemController::InhibitHeatersForMotion()` (or a MotionLock acquisition)
// could take up to 1000 ms to reach the GPIO -- a heater staying energised
// for most of a second after motion was commanded. Reading inside the slice
// loop bounds that latency to one slice: ~10 ms at 1 Hz.
//
// `write(channel, on)` is invoked only on a CHANGE, tracked through
// `*last_state`, so the GPIO write rate is exactly what it was before.
// `running()` is polled per slice so a shutdown can abort mid-period, and
// `wait_slice()` paces one slice.
//
// Free function template so it is exercisable on hosts with no libgpiod:
// LibgpiodPwmController's own loop cannot run here (its worker thread is
// only started under COATHEAL_HAS_LIBGPIOD), but this is the part of it that
// carries the timing decision.
template <typename DutyAt, typename Write, typename Running,
          typename WaitSlice>
void RenderPwmPeriod(int slices, std::size_t channels, DutyAt duty_at,
                     Write write, Running running, WaitSlice wait_slice,
                     std::vector<bool>* last_state) {
  for (int slice = 0; slice < slices && running(); ++slice) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const bool on =
          static_cast<double>(slice) <
          std::round(duty_at(channel) * static_cast<double>(slices));
      if (on != (*last_state)[channel]) {
        write(channel, on);
        (*last_state)[channel] = on;
      }
    }
    wait_slice();
  }
}

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
