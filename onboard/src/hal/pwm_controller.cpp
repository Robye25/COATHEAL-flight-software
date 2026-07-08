#include "coatheal/hal/pwm_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#ifdef COATHEAL_HAS_LIBGPIOD
#include <gpiod.h>
#endif

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
  auto* chip = gpiod_chip_open(chip_.c_str());
  if (chip == nullptr) {
    return;
  }
  chip_handle_ = chip;
  line_handles_.reserve(channels);
  const int off_value = active_high_ ? 0 : 1;
  for (const std::size_t offset : output_lines_) {
    gpiod_line* line = gpiod_chip_get_line(chip, static_cast<unsigned int>(offset));
    if (line == nullptr ||
        gpiod_line_request_output(line, "coatheal-heater", off_value) < 0) {
      AllOff();
      for (void* handle : line_handles_) {
        gpiod_line_release(static_cast<gpiod_line*>(handle));
      }
      line_handles_.clear();
      gpiod_chip_close(chip);
      chip_handle_ = nullptr;
      return;
    }
    line_handles_.push_back(line);
  }
  healthy_ = true;
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
    gpiod_line_release(static_cast<gpiod_line*>(handle));
  }
  line_handles_.clear();
  if (chip_handle_ != nullptr) {
    gpiod_chip_close(static_cast<gpiod_chip*>(chip_handle_));
    chip_handle_ = nullptr;
  }
#endif
}

bool LibgpiodPwmController::SetDuty(std::size_t channel, double duty) {
  if (channel >= duty_.size()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    duty_[channel] = std::clamp(duty, 0.0, 1.0);
  }
  return healthy_;
}

bool LibgpiodPwmController::WriteLine(std::size_t channel, bool on) {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (channel >= line_handles_.size()) return false;
  const int physical_value = on ? (active_high_ ? 1 : 0)
                                : (active_high_ ? 0 : 1);
  if (gpiod_line_set_value(
          static_cast<gpiod_line*>(line_handles_[channel]), physical_value) < 0) {
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

void LibgpiodPwmController::PwmLoop() {
  constexpr int kSlices = 100;
  const double safe_frequency = std::max(0.1, pwm_frequency_hz_);
  const auto slice_duration = std::chrono::duration<double>(
      1.0 / (safe_frequency * static_cast<double>(kSlices)));
  std::vector<bool> last_state(duty_.size(), false);

  while (running_.load()) {
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
