#pragma once

#include <cstddef>
#include <string>

namespace coatheal {

// Visual status LED abstraction. Two instances are wired into the onboard
// runtime: a heartbeat LED toggled each tick from the main loop, and a
// mode-indicator LED whose blink pattern reflects the current SystemMode.
class StatusLed {
 public:
  enum class Pattern {
    kOff,
    kHeartbeat,
    kSolid,
    kFastBlink,
    kSOS,
  };

  virtual ~StatusLed() = default;

  virtual bool On() = 0;
  virtual bool Off() = 0;
  virtual bool Toggle() = 0;
  virtual bool SetBlink(int hz) = 0;
  virtual bool Set(Pattern pattern) = 0;

  virtual bool healthy() const = 0;
};

// Simulated implementation that logs state transitions to stderr. Selected
// when runtime.use_simulated_pwm=true so bench builds do not touch real GPIO.
class SimulatedStatusLed : public StatusLed {
 public:
  explicit SimulatedStatusLed(std::string label, std::size_t line);

  bool On() override;
  bool Off() override;
  bool Toggle() override;
  bool SetBlink(int hz) override;
  bool Set(Pattern pattern) override;

  bool healthy() const override { return true; }

  // Accessors used by tests.
  bool is_on() const { return on_; }
  int blink_hz() const { return blink_hz_; }
  Pattern pattern() const { return pattern_; }
  std::size_t line() const { return line_; }
  unsigned toggle_count() const { return toggle_count_; }

 private:
  std::string label_;
  std::size_t line_;
  bool on_ = false;
  int blink_hz_ = 0;
  Pattern pattern_ = Pattern::kOff;
  unsigned toggle_count_ = 0;
};

// Real libgpiod-backed LED driver. The line is requested low in the
// constructor and healthy() reflects both request and write failures.
class GpioStatusLed : public StatusLed {
 public:
  GpioStatusLed(std::string chip, std::size_t line, std::string label);
  ~GpioStatusLed() override;

  bool On() override;
  bool Off() override;
  bool Toggle() override;
  bool SetBlink(int hz) override;
  bool Set(Pattern pattern) override;

  bool healthy() const override { return healthy_; }

 private:
  bool Write(bool value);

  std::string chip_;
  std::size_t line_;
  std::string label_;
  bool on_ = false;
  int blink_hz_ = 0;
  Pattern pattern_ = Pattern::kOff;
  bool healthy_ = false;
  void* line_handle_ = nullptr;  // GpioOutput* opaque pointer
};

}  // namespace coatheal
