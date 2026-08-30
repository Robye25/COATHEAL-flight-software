#include "coatheal/pid_autotuner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace coatheal {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  return std::accumulate(v.begin(), v.end(), 0.0) /
         static_cast<double>(v.size());
}

}  // namespace

void PidAutoTuner::Start(const Config& cfg, double now_s) {
  cfg_ = cfg;
  result_ = Result{};
  error_.clear();
  start_s_ = now_s;
  relay_on_ = true;
  first_on_phase_ = true;
  have_on_switch_ = false;
  last_on_switch_s_ = 0.0;
  trough_c_ = std::numeric_limits<double>::max();
  peak_c_ = std::numeric_limits<double>::lowest();
  peaks_c_.clear();
  troughs_c_.clear();
  periods_s_.clear();
  state_ = State::kRunning;
}

void PidAutoTuner::Abort(const std::string& reason) {
  if (state_ == State::kRunning) Fail(reason);
}

void PidAutoTuner::Fail(const std::string& reason) {
  state_ = State::kFailed;
  error_ = reason;
}

double PidAutoTuner::Tick(bool temp_valid, double temp_c, double now_s) {
  if (state_ != State::kRunning) return 0.0;

  if (!temp_valid) {
    Fail("temperature reading became invalid");
    return 0.0;
  }
  if (temp_c >= cfg_.abort_ceiling_c) {
    Fail("temperature exceeded the tune ceiling");
    return 0.0;
  }
  if (now_s - start_s_ > cfg_.timeout_s) {
    Fail("timed out before the oscillation settled");
    return 0.0;
  }

  const double high = cfg_.setpoint_c + cfg_.hysteresis_c;
  const double low = cfg_.setpoint_c - cfg_.hysteresis_c;

  if (relay_on_) {
    trough_c_ = std::min(trough_c_, temp_c);
    if (temp_c >= high) {
      // ON -> OFF: the ON phase's trough is complete. The very first ON
      // phase is the approach from ambient — its "trough" is just the
      // starting temperature, not part of the limit cycle.
      if (!first_on_phase_) {
        troughs_c_.push_back(trough_c_);
      }
      first_on_phase_ = false;
      relay_on_ = false;
      peak_c_ = std::numeric_limits<double>::lowest();
    }
  } else {
    peak_c_ = std::max(peak_c_, temp_c);
    if (temp_c <= low) {
      // OFF -> ON: one full period boundary. The OFF phase's peak is
      // complete, and the time between consecutive ON-switches is Tu.
      peaks_c_.push_back(peak_c_);
      if (have_on_switch_) {
        periods_s_.push_back(now_s - last_on_switch_s_);
      }
      have_on_switch_ = true;
      last_on_switch_s_ = now_s;
      relay_on_ = true;
      trough_c_ = std::numeric_limits<double>::max();

      const int need = std::max(1, cfg_.cycles);
      if (static_cast<int>(periods_s_.size()) >= need &&
          static_cast<int>(troughs_c_.size()) >= need &&
          static_cast<int>(peaks_c_.size()) >= need) {
        Finish();
        return 0.0;
      }
    }
  }

  return relay_on_ ? cfg_.relay_duty : 0.0;
}

void PidAutoTuner::Finish() {
  const double amplitude =
      (Mean(peaks_c_) - Mean(troughs_c_)) / 2.0;
  const double tu = Mean(periods_s_);

  if (amplitude < 0.05) {
    Fail("oscillation amplitude too small to measure (< 0.05 C)");
    return;
  }
  if (tu < 5.0) {
    Fail("oscillation period too short for the control rate");
    return;
  }

  // Relay half-amplitude: the relay swings 0..relay_duty about its mean,
  // so d = relay_duty / 2.
  const double d = cfg_.relay_duty / 2.0;
  const double ku = 4.0 * d / (kPi * amplitude);

  result_.ku = ku;
  result_.tu_s = tu;
  result_.amplitude_c = amplitude;
  result_.cycles_used = static_cast<int>(periods_s_.size());

  // Tyreus–Luyben (parallel form: ki = kp/Ti, kd = kp*Td).
  result_.kp = ku / 3.2;
  const double ti_tl = 2.2 * tu;
  const double td_tl = tu / 6.3;
  result_.ki = result_.kp / ti_tl;
  result_.kd = result_.kp * td_tl;

  // Classic Ziegler–Nichols PID, for reference.
  result_.zn_kp = 0.6 * ku;
  const double ti_zn = tu / 2.0;
  const double td_zn = tu / 8.0;
  result_.zn_ki = result_.zn_kp / ti_zn;
  result_.zn_kd = result_.zn_kp * td_zn;

  state_ = State::kDone;
}

}  // namespace coatheal
