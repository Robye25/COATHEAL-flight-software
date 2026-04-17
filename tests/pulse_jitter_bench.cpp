// Agent B — pulse-thread jitter harness.
//
// Launches a StepperChannel with use_pulse_thread=true so pulses come from
// the dedicated RT thread (not from Tick()). Requests MoveToSteps(800, 0)
// which is exactly one mechanical revolution at microstep=4 (200 full
// steps/rev × 4 = 800 µsteps). Records the wall-clock delta between
// consecutive Step() calls on the driver, then reports p50/p99/max jitter
// against the commanded 400 µstep/s spacing (2500 µs period).
//
// Usage: coatheal_pulse_jitter_bench [--out <path>]
//   Prints summary to stdout; optionally writes per-pulse deltas to file.
//
// Threshold: p99 pulse jitter < 5 ms at 400 µstep/s.
//
// Build only runs on Linux (thread/sleep semantics); harmless on Windows too.

#include "coatheal/hal/stepper_driver.hpp"
#include "coatheal/motion_lock.hpp"
#include "coatheal/stepper_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Records every Step() timestamp into an internal ring-free vector. Pre-sized
// so push_back never reallocates during the measurement window.
class TimestampingDriver : public coatheal::StepperDriver {
 public:
  explicit TimestampingDriver(std::size_t reserve_n) { stamps_.reserve(reserve_n); }

  bool Enable(bool enable) override {
    enabled_ = enable;
    return true;
  }
  bool Step(bool /*forward*/) override {
    if (!enabled_) return false;
    stamps_.push_back(std::chrono::steady_clock::now());
    ++pulses_;
    return true;
  }
  void SetMicrostep(int divisor) override {
    if (divisor > 0) microstep_ = divisor;
  }
  bool healthy() const override { return true; }
  std::uint64_t pulses_issued() const override { return pulses_; }

  const std::vector<std::chrono::steady_clock::time_point>& stamps() const {
    return stamps_;
  }

 private:
  bool enabled_ = false;
  int microstep_ = 1;
  std::uint64_t pulses_ = 0;
  std::vector<std::chrono::steady_clock::time_point> stamps_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string out_path;
  std::int64_t move_usteps = 800;  // spec: one revolution at microstep=4
  std::size_t skip_usteps = 500;   // skip through the trapezoidal ramp
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (a == "--move" && i + 1 < argc) move_usteps = std::stoll(argv[++i]);
    else if (a == "--skip" && i + 1 < argc) skip_usteps = std::stoul(argv[++i]);
  }

  const std::int64_t kMoveUsteps = move_usteps;
  constexpr double kMaxStepHz = 100.0;       // full-step
  constexpr int kMicrostep = 4;              // default
  // Expected ustep period = 1 / (100 * 4) = 2500 µs.
  constexpr double kExpectedPeriodUs = 1.0e6 / (kMaxStepHz * kMicrostep);

  coatheal::StepperChannelConfig cfg;
  cfg.channel_id = 0;
  cfg.full_steps_per_rev = 200;
  cfg.max_step_hz = kMaxStepHz;
  cfg.default_step_hz = kMaxStepHz;
  cfg.accel_steps_per_s2 = 200.0;
  cfg.microstep = kMicrostep;
  cfg.max_position_steps = 200000;
  cfg.samples = {0, 1, 2, 3};
  cfg.pull_travel_full_steps = 200;
  cfg.pull_hold_s = 0.0;
  cfg.enable_on_boot = true;
  cfg.use_pulse_thread = true;  // <-- the path under test

  auto driver = std::make_unique<TimestampingDriver>(
      static_cast<std::size_t>(kMoveUsteps + 16));
  TimestampingDriver* driver_raw = driver.get();

  coatheal::MotionLock lock;

  {
    coatheal::StepperChannel ch(std::move(cfg), std::move(driver), &lock);

    std::string err;
    if (!ch.MoveToSteps(kMoveUsteps, 0.0, &err)) {
      std::fprintf(stderr, "MoveToSteps failed: %s\n", err.c_str());
      return 1;
    }

    // Poll the driver's pulse counter. Gating on Snapshot().moving is unsafe
    // under use_pulse_thread=true because the RT thread body never flips
    // moving_ back to false on its own (Tick() is never called here). This
    // is a diagnostic harness, not a correctness test — we break the moment
    // the move is delivered.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
      if (driver_raw->pulses_issued() >= static_cast<std::uint64_t>(kMoveUsteps)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }  // channel dtor joins pulse thread

  const auto& ts = driver_raw->stamps();
  if (ts.size() < 2) {
    std::fprintf(stderr, "only %zu pulses observed (need >= 2)\n", ts.size());
    return 2;
  }

  // Skip the first N µsteps so ramp-up doesn't pollute steady-state stats.
  const std::size_t kSkip = skip_usteps;
  if (ts.size() <= kSkip + 10) {
    std::fprintf(stderr, "too few pulses after skip: total=%zu skip=%zu\n",
                 ts.size(), kSkip);
    return 3;
  }

  std::vector<double> deltas_us;
  deltas_us.reserve(ts.size());
  for (std::size_t i = kSkip + 1; i < ts.size(); ++i) {
    const auto d = std::chrono::duration_cast<std::chrono::microseconds>(
        ts[i] - ts[i - 1]);
    deltas_us.push_back(static_cast<double>(d.count()));
  }

  std::vector<double> jitter_us;
  jitter_us.reserve(deltas_us.size());
  for (double d : deltas_us) {
    jitter_us.push_back(std::abs(d - kExpectedPeriodUs));
  }

  std::vector<double> sorted_deltas = deltas_us;
  std::sort(sorted_deltas.begin(), sorted_deltas.end());
  std::vector<double> sorted_jitter = jitter_us;
  std::sort(sorted_jitter.begin(), sorted_jitter.end());

  auto pick = [&](const std::vector<double>& v, double q) {
    const std::size_t idx = std::min(
        v.size() - 1, static_cast<std::size_t>(v.size() * q));
    return v[idx];
  };

  const double d_p50 = pick(sorted_deltas, 0.50);
  const double d_p99 = pick(sorted_deltas, 0.99);
  const double d_min = sorted_deltas.front();
  const double d_max = sorted_deltas.back();

  const double j_p50 = pick(sorted_jitter, 0.50);
  const double j_p99 = pick(sorted_jitter, 0.99);
  const double j_max = sorted_jitter.back();
  double j_sum = 0.0;
  for (double x : jitter_us) j_sum += x;
  const double j_avg = j_sum / static_cast<double>(jitter_us.size());

  std::printf(
      "[pulse-jitter] pulses=%zu samples=%zu expected_us=%.1f "
      "delta_min_us=%.1f delta_p50_us=%.1f delta_p99_us=%.1f delta_max_us=%.1f "
      "jitter_avg_us=%.1f jitter_p50_us=%.1f jitter_p99_us=%.1f jitter_max_us=%.1f\n",
      ts.size(), deltas_us.size(), kExpectedPeriodUs,
      d_min, d_p50, d_p99, d_max,
      j_avg, j_p50, j_p99, j_max);

  if (!out_path.empty()) {
    std::ofstream f(out_path);
    f << "idx,delta_us,jitter_us\n";
    for (std::size_t i = 0; i < deltas_us.size(); ++i) {
      f << i << ',' << deltas_us[i] << ',' << jitter_us[i] << '\n';
    }
  }

  // Exit code encodes whether we hit the < 5 ms p99 jitter threshold.
  constexpr double kThresholdUs = 5000.0;
  if (j_p99 >= kThresholdUs) {
    std::fprintf(stderr, "[pulse-jitter] FAIL: p99 jitter %.1f us >= %.1f us\n",
                 j_p99, kThresholdUs);
    return 4;
  }
  return 0;
}
