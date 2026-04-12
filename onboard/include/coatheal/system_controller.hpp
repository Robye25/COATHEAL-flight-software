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
#include "coatheal/heater_scheduler.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/state_manager.hpp"
#include "coatheal/storage_manager.hpp"
#include "coatheal/system_mode.hpp"
#include "coatheal/telemetry_client.hpp"
#include "coatheal/telemetry_queue.hpp"
#include "coatheal/thermal_controller.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/pwm_controller.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"

namespace coatheal {

class SystemController {
 public:
  explicit SystemController(OnboardConfig config);

  bool Initialize(std::string* error);
  int Run();

 private:
  bool DrainTelemetryQueue(bool* link_ok, std::string* error);
  std::string HandleCommandLine(const std::string& line);

  OnboardConfig config_;
  CommandParser parser_;
  CommandServer command_server_;

  SpiAdapter spi_;
  I2cAdapter i2c_;
  RtcAdapter rtc_;

  std::unique_ptr<PwmController> pwm_;
  SensorManager sensor_manager_;
  StateManager state_manager_;
  ThermalController thermal_controller_;
  HeaterScheduler scheduler_;
  StorageManager storage_manager_;
  TelemetryQueue telemetry_queue_;
  TelemetryClient telemetry_client_;

  std::atomic<bool> running_{true};
  std::atomic<bool> debug_armed_{false};
  std::atomic<SystemMode> mode_{SystemMode::kStandby};
  std::atomic<double> live_tick_hz_{1.0};

  mutable std::mutex overrides_mu_;
  StateOverrides state_overrides_;
  ControlOverrides control_overrides_;

  std::vector<double> last_heater_duty_;
  std::uint64_t seq_ = 0;
};

}  // namespace coatheal
