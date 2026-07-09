#include "coatheal/hal/pwm_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "coatheal/hal/gpio_output.hpp"

namespace coatheal {

SimulatedPwmController::SimulatedPwmController(std::size_t channels) : duty_(channels, 0.0) {}

bool SimulatedPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size()) {
    return false;
  }
  duty_[channel] = std::clamp(duty, 0.0, 1.0);
  return true;
}

LibgpiodPwmController::LibgpiodPwmController(
    std::string chip,
    std::size_t channels,
    std::vector<std::size_t> output_lines,
    double pwm_frequency_hz,
    bool active_high)
    : chip_(std::move(chip)),
      output_lines_(std::move(output_lines)),
      pwm_frequency_hz_(pwm_frequency_hz),
      active_high_(active_high),
      duty_(channels, 0.0) {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (output_lines_.size() != channels || output_lines_.empty()) {
    return;
  }
  line_handles_.assign(channels, nullptr);
  const int off_value = active_high_ ? 0 : 1;
  bool all_ok = true;
  for (std::size_t channel = 0; channel < output_lines_.size(); ++channel) {
    const std::size_t offset = output_lines_[channel];
    auto* line = RequestGpioOutput(
        chip_, offset, "coatheal-heater", off_value != 0);
    if (line == nullptr) {
      all_ok = false;
      continue;
    }
    line_handles_[channel] = line;
  }
  healthy_ = all_ok;
  running_.store(true);
  worker_ = std::thread(&LibgpiodPwmController::PwmLoop, this);
#else
  healthy_ = false;
#endif
}

LibgpiodPwmController::~LibgpiodPwmController() {
  running_.store(false);
  if (worker_.joinable()) {
    worker_.join();
  }
  AllOff();
#ifdef COATHEAL_HAS_LIBGPIOD
  for (void* handle : line_handles_) {
    ReleaseGpioOutput(static_cast<GpioOutput*>(handle));
  }
  line_handles_.clear();
#endif
}

bool LibgpiodPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size() || !channel_healthy(channel)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    duty_[channel] = std::clamp(duty, 0.0, 1.0);
  }
  return true;
}

bool LibgpiodPwmController::channel_healthy(std::size_t channel) const {
  std::lock_guard<std::mutex> lock(handles_mu_);
  return channel < line_handles_.size() &&
         line_handles_[channel] != nullptr;
}

std::size_t LibgpiodPwmController::healthy_channel_count() const {
  std::lock_guard<std::mutex> lock(handles_mu_);
  return static_cast<std::size_t>(std::count_if(
      line_handles_.begin(), line_handles_.end(),
      [](const void* handle) { return handle != nullptr; }));
}

bool LibgpiodPwmController::WriteLine(std::size_t channel, bool on) {
#ifdef COATHEAL_HAS_LIBGPIOD
  std::lock_guard<std::mutex> lock(handles_mu_);
  if (channel >= line_handles_.size() ||
      line_handles_[channel] == nullptr) return false;
  const int physical_value = on ? (active_high_ ? 1 : 0)
                                : (active_high_ ? 0 : 1);
  if (!SetGpioOutput(static_cast<GpioOutput*>(line_handles_[channel]),
                     physical_value != 0)) {
    ReleaseGpioOutput(static_cast<GpioOutput*>(line_handles_[channel]));
    line_handles_[channel] = nullptr;
    healthy_ = false;
    return false;
  }
  return true;
#else
  (void)channel;
  (void)on;
  return false;
#endif
}

void LibgpiodPwmController::AllOff() {
  for (std::size_t i = 0; i < line_handles_.size(); ++i) {
    WriteLine(i, false);
  }
}

void LibgpiodPwmController::RetryMissingLines() {
#ifdef COATHEAL_HAS_LIBGPIOD
  std::lock_guard<std::mutex> lock(handles_mu_);
  const int off_value = active_high_ ? 0 : 1;
  bool all_ok = !line_handles_.empty();
  for (std::size_t channel = 0; channel < line_handles_.size(); ++channel) {
    if (line_handles_[channel] == nullptr) {
      line_handles_[channel] = RequestGpioOutput(
          chip_, output_lines_[channel], "coatheal-heater",
          off_value != 0);
    }
    all_ok = all_ok && line_handles_[channel] != nullptr;
  }
  healthy_ = all_ok;
#endif
}

void LibgpiodPwmController::PwmLoop() {
  constexpr int kSlices = 100;
  const double safe_frequency = std::max(0.1, pwm_frequency_hz_);
  const auto slice_duration = std::chrono::duration<double>(
      1.0 / (safe_frequency * static_cast<double>(kSlices)));
  std::vector<bool> last_state(duty_.size(), false);
  auto next_retry = std::chrono::steady_clock::now();

  while (running_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_retry) {
      RetryMissingLines();
      next_retry = now + std::chrono::seconds(2);
    }
    std::vector<double> snapshot;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot = duty_;
    }
    for (int slice = 0; slice < kSlices && running_.load(); ++slice) {
      for (std::size_t channel = 0; channel < snapshot.size(); ++channel) {
        const bool on = static_cast<double>(slice) <
                        std::round(snapshot[channel] * kSlices);
        if (on != last_state[channel]) {
          WriteLine(channel, on);
          last_state[channel] = on;
        }
      }
      std::this_thread::sleep_for(slice_duration);
    }
  }
  AllOff();
}

}  // namespace coatheal
