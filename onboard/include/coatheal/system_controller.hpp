#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "coatheal/command_parser.hpp"
#include "coatheal/command_server.hpp"
#include "coatheal/config.hpp"
#include "coatheal/fatigue_sequencer.hpp"
#include "coatheal/heater_scheduler.hpp"
#include "coatheal/motion_lock.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/state_manager.hpp"
#include "coatheal/stepper_controller.hpp"
#include "coatheal/storage_manager.hpp"
#include "coatheal/system_mode.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_queue.hpp"
#include "coatheal/thermal_controller.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/pwm_controller.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/hal/status_led.hpp"

namespace coatheal {

class SystemController {
 public:
  explicit SystemController(OnboardConfig config);

  bool Initialize(std::string* error);
  int Run();

 private:
  bool DrainTelemetryQueue(bool* link_ok, std::string* error);
  std::string HandleCommandLine(const std::string& line,
                                const std::string& peer_ip);

  OnboardConfig config_;
  CommandParser parser_;
  CommandServer command_server_;

  SpiAdapter spi_;
  I2cAdapter i2c_;
  RtcAdapter rtc_;
  Ina3221Adapter ina_;

  std::unique_ptr<PwmController> pwm_;
  std::unique_ptr<StatusLed> status_led_;  // heartbeat, toggled each tick
  std::unique_ptr<StatusLed> mode_led_;    // system-mode indicator
  SensorManager sensor_manager_;
  StateManager state_manager_;
  ThermalController thermal_controller_;
  // MotionLock must be declared BEFORE scheduler_ so the scheduler ctor can
  // take its address (member-init order is declaration order). Note: this
  // lock is ONLY used as a fallback for test harnesses that never construct
  // the StepperController. In production, `active_motion_lock_` is
  // repointed at `stepper_->motion_lock()` in Initialize() so the heater
  // interlock and pull-event edge detector observe the same lock every
  // StepperChannel actually acquires. (Agent C, 2026-04-17 routing fix.)
  MotionLock motion_lock_;
  HeaterScheduler scheduler_;
  StorageManager storage_manager_;
  TelemetryQueue telemetry_queue_;
  TelemetryClient telemetry_client_;
  std::unique_ptr<StepperController> stepper_;
  std::unique_ptr<FatigueSequencer> fatigue_sequencer_;

  std::atomic<bool> running_{true};
  std::atomic<bool> debug_armed_{false};
  std::atomic<SystemMode> mode_{SystemMode::kStandby};
  std::atomic<double> live_tick_hz_{1.0};

  mutable std::mutex overrides_mu_;
  StateOverrides state_overrides_;
  ControlOverrides control_overrides_;

  std::vector<double> last_heater_duty_;
  std::uint64_t seq_ = 0;
  bool link_seen_ = false;
  double link_loss_s_ = 0.0;
  bool link_loss_fallback_active_ = false;

  // Rev B pull-event bookkeeping: emit EVT,PULL after each motor completes a
  // pull cycle. Edge-detects channel moving true->false while the MotionLock
  // was held by that motor.
  struct PullState {
    bool was_moving = false;
    bool lock_held = false;
    std::string start_ts;
    std::int64_t start_pos = 0;
  };
  std::vector<PullState> pull_state_;
  std::uint32_t next_pull_id_ = 1;

  // Points at the MotionLock the stepper actually uses (see Initialize()
  // for the routing rationale). nullptr until Initialize() runs.
  MotionLock* active_motion_lock_ = nullptr;
};

}  // namespace coatheal
