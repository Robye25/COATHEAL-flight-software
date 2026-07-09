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
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
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
  bool rs485_ok() const { return rs485_ok_.load(); }
  bool sample_temp_ok() const { return sample_temp_ok_.load(); }
  bool uv_ok() const { return uv_ok_.load(); }
  bool simulated() const { return simulated_; }
  bool ActiveCheck(const std::string& component, std::string* details);
  std::string ComponentSummary() const;

  void NotePullCompleted(int motor_id);

  static double Max31865CodeToResistance(std::uint16_t code,
                                         double reference_ohm);
  static bool Pt100TemperatureFromResistance(double resistance_ohm,
                                             double* temperature_c);

 private:
  bool ReadDps310At(int address, double* temp_c, double* pressure_mbar);
  bool ReadAds1115At(int address, double* voltage);
  bool ReadDaq132m(std::vector<double>* temperatures,
                   std::vector<bool>* valid);
  bool ReadRtdClickMax31865(double* temperature_c, std::string* error);
  SensorSnapshot ReadSimulatedSnapshot(MissionPhase phase,
                                       const std::vector<double>& heater_duty,
                                       double dt_seconds);
  void DpsLoop();
  void AdsLoop();
  void DaqLoop();
  void RtdClickLoop();
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
  std::thread daq_thread_;
  std::thread rtd_thread_;
  ScalarCache ambient_temp_cache_;
  ScalarCache pressure_cache_;
  ScalarCache uv_cache_;
  std::vector<ScalarCache> sample_cache_;
  ComponentHealth dps_health_;
  ComponentHealth ads_health_;
  ComponentHealth daq_health_;
  ComponentHealth rtd_health_;
  int resolved_dps_address_ = -1;
  int resolved_ads_address_ = -1;
  std::string resolved_daq_device_;
  mutable std::mutex dps_io_mu_;
  mutable std::mutex ads_io_mu_;
  mutable std::mutex daq_io_mu_;
  mutable std::mutex rtd_io_mu_;
};

}  // namespace coatheal
