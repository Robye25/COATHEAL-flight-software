#pragma once

#include <string>
#include <vector>

namespace coatheal {

// Relay (Åström–Hägglund) PID auto-tuner for one heater channel.
//
// The classic bounded-excursion method for a heat-only actuator: switch the
// heater between 0 and `relay_duty` around `setpoint_c` with a ±
// `hysteresis_c` band, let the plant limit-cycle, and measure the induced
// oscillation. With relay half-amplitude d = relay_duty/2 and temperature
// half-amplitude a, the ultimate gain is Ku = 4d/(πa) and the ultimate
// period Tu is the measured cycle time. Gains follow Tyreus–Luyben
// (kp = Ku/3.2, Ti = 2.2·Tu, Td = Tu/6.3), chosen over classic
// Ziegler–Nichols because this plant is lag-dominant (film heater far
// ahead of its PT100) and TL trades a slower rise for far less overshoot —
// exactly the trade the 85 °C latch wants. Both rule sets are reported.
//
// Pure logic: the caller owns actuation, sampling, and every safety gate
// (validity, latch, mode, scheduler clamping); Tick() just advances the
// state machine and returns the duty the relay wants this tick. All
// times are the caller's monotonic seconds.
class PidAutoTuner {
 public:
  struct Config {
    double setpoint_c = 40.0;
    // Relay high level. The caller clamps this to heater.max_duty before
    // starting.
    double relay_duty = 0.5;
    // Half-width of the switching band. Must comfortably exceed sensor
    // ripple or the relay chatters at the noise frequency.
    double hysteresis_c = 1.0;
    // Complete limit-cycle periods to average (the approach transient is
    // excluded by construction: the first trough belongs to the approach
    // and is discarded).
    int cycles = 4;
    double timeout_s = 1500.0;
    // Hard abort ceiling; caller derives it from the setpoint and the
    // overtemp latch (e.g. min(setpoint+15, max_sample_temp-5)).
    double abort_ceiling_c = 80.0;
  };

  struct Result {
    double ku = 0.0;            // ultimate gain
    double tu_s = 0.0;          // ultimate period
    double amplitude_c = 0.0;   // temperature half-amplitude of the cycle
    int cycles_used = 0;
    // Tyreus–Luyben (recommended, what APPLY uses).
    double kp = 0.0, ki = 0.0, kd = 0.0;
    // Classic Ziegler–Nichols, for reference/comparison.
    double zn_kp = 0.0, zn_ki = 0.0, zn_kd = 0.0;
  };

  enum class State { kIdle, kRunning, kDone, kFailed };

  void Start(const Config& cfg, double now_s);
  void Abort(const std::string& reason);

  // Advance one control tick. `temp_valid` false at any point fails the
  // tune (a relay driven by garbage would "measure" garbage). Returns the
  // duty to command this tick (0 unless kRunning).
  double Tick(bool temp_valid, double temp_c, double now_s);

  State state() const { return state_; }
  bool active() const { return state_ == State::kRunning; }
  const Result& result() const { return result_; }
  const Config& config() const { return cfg_; }
  const std::string& error() const { return error_; }
  // Progress for STATUS: completed measurement cycles / requested.
  int cycles_done() const { return static_cast<int>(periods_s_.size()); }
  bool relay_on() const { return relay_on_; }
  double elapsed_s(double now_s) const {
    return state_ == State::kIdle ? 0.0 : now_s - start_s_;
  }

 private:
  void Fail(const std::string& reason);
  void Finish();

  State state_ = State::kIdle;
  Config cfg_;
  Result result_;
  std::string error_;
  double start_s_ = 0.0;

  bool relay_on_ = true;          // start heating: the approach half-cycle
  bool first_on_phase_ = true;    // its trough is the approach — discarded
  bool have_on_switch_ = false;
  double last_on_switch_s_ = 0.0;
  double trough_c_ = 0.0;         // running min of the current ON phase
  double peak_c_ = 0.0;           // running max of the current OFF phase
  std::vector<double> peaks_c_;
  std::vector<double> troughs_c_;
  std::vector<double> periods_s_;
};

}  // namespace coatheal
