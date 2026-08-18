#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"
#include "coatheal/telemetry.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/i2c_bus.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"

namespace coatheal {

// Final BOM sensor facade. Physical I/O runs in independent polling workers;
// ReadSnapshot only copies the latest cache and therefore cannot block the
// control or telemetry loop.
class SensorManager {
 public:
  static constexpr std::size_t kSampleCount = 8;

  SensorManager(const OnboardConfig& config,
                SpiAdapter* spi,
                I2cAdapter* i2c,
                RtcAdapter* rtc,
                Ina3221Adapter* ina = nullptr);
  ~SensorManager();

  void Start();
  void Stop();

  SensorSnapshot ReadSnapshot(MissionPhase phase,
                              const std::vector<double>& heater_duty,
                              double dt_seconds);

  bool t_ambient_ok() const { return t_ambient_ok_.load(); }
  bool p_ambient_ok() const { return p_ambient_ok_.load(); }
  bool resistance_ok() const { return resistance_ok_.load(); }
  bool i2c_ok() const { return i2c_ok_.load(); }
  // Transitional: nothing drives the RS485 path any more, but the STATUS
  // wire field still carries it. Task 7 removes both together.
  bool rs485_ok() const { return rs485_ok_.load(); }
  bool sample_temp_ok() const { return sample_temp_ok_.load(); }
  bool uv_ok() const { return uv_ok_.load(); }
  bool simulated() const { return simulated_; }
  bool ActiveCheck(const std::string& component, std::string* details);
  std::string ComponentSummary() const;

  void NotePullCompleted(int motor_id);

  static bool Pt100TemperatureFromResistance(double resistance_ohm,
                                             double* temperature_c);

  // True only when every sample channel a heater actually controls is both
  // valid and fresh. Static so the policy is testable without threads or
  // hardware; see the definition for why it fails closed.
  static bool HeatedChannelsValid(const OnboardConfig& config,
                                  const std::vector<bool>& channel_valid);

 private:
  bool ReadDps310At(int address, double* temp_c, double* pressure_mbar);
  bool ReadAds1115At(int address, double* voltage);
  SensorSnapshot ReadSimulatedSnapshot(MissionPhase phase,
                                       const std::vector<double>& heater_duty,
                                       double dt_seconds);
  void DpsLoop();
  void AdsLoop();
  void SequentRtdLoop();
  bool WaitForPoll(int milliseconds);
  std::int64_t AgeMs(
      const std::chrono::steady_clock::time_point& value,
      bool has_value) const;
  ComponentState FailedState(bool has_success,
                             const std::chrono::steady_clock::time_point& last_success) const;

  struct ScalarCache {
    double value = 0.0;
    bool has_value = false;
    bool valid = false;
    std::chrono::steady_clock::time_point last_success{};
  };

  OnboardConfig config_;
  SpiAdapter* spi_ = nullptr;
  I2cAdapter* i2c_ = nullptr;
  RtcAdapter* rtc_ = nullptr;
  Ina3221Adapter* ina_ = nullptr;
  std::vector<double> sample_temps_c_;
  std::vector<double> sample_resistance_ohm_;
  double pressure_mbar_ = 1013.25;
  bool pressure_descending_ = true;
  std::atomic<bool> t_ambient_ok_{true};
  std::atomic<bool> p_ambient_ok_{true};
  std::atomic<bool> resistance_ok_{true};
  std::atomic<bool> i2c_ok_{false};
  std::atomic<bool> rs485_ok_{false};
  std::atomic<bool> sample_temp_ok_{false};
  std::atomic<bool> uv_ok_{false};
  bool simulated_ = false;
  mutable std::mutex cache_mu_;
  std::condition_variable stop_cv_;
  mutable std::mutex stop_mu_;
  std::atomic<bool> running_{false};
  std::thread dps_thread_;
  std::thread ads_thread_;
  std::thread rtd_thread_;
  ScalarCache ambient_temp_cache_;
  ScalarCache pressure_cache_;
  ScalarCache uv_cache_;
  std::vector<ScalarCache> sample_cache_;
  ComponentHealth dps_health_;
  ComponentHealth ads_health_;
  ComponentHealth rtd_health_;
  int resolved_dps_address_ = -1;
  int resolved_ads_address_ = -1;
  mutable std::mutex dps_io_mu_;
  mutable std::mutex ads_io_mu_;
  mutable std::mutex rtd_io_mu_;

  // Declared last so the constructor initialiser list can stay in
  // declaration order; rtd_ holds a pointer to rtd_bus_, so rtd_bus_ must
  // precede it here.
  LinuxI2cBus rtd_bus_;
  SequentRtdAdapter rtd_;
  SequentRtdAdapter::Identity rtd_identity_;
  bool rtd_probed_ = false;
  SequentRtdAdapter::Reading rtd_last_reading_;
  bool rtd_has_reading_ = false;
};

}  // namespace coatheal
