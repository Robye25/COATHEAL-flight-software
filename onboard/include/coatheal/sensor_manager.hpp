#pragma once

#include <array>
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
#include "coatheal/hal/max31865_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/hal/spi_bus.hpp"

namespace coatheal {

// Final BOM sensor facade. Physical I/O runs in independent polling workers;
// ReadSnapshot only copies the latest cache and therefore cannot block the
// control or telemetry loop.
class SensorManager {
 public:
  static constexpr std::size_t kSampleCount = 8;

  // rtd_bus_override lets a test substitute the RTD card's I2C transport
  // (e.g. FakeI2cBus) without touching the owned LinuxI2cBus. nullptr (the
  // default, and every production call site) means "use the owned bus" —
  // this parameter is purely additive. click1/2_bus_override do the same
  // for the two MAX31865 clicks' SPI transport (e.g. FakeSpiBus).
  SensorManager(const OnboardConfig& config,
                SpiAdapter* spi,
                I2cAdapter* i2c,
                RtcAdapter* rtc,
                Ina3221Adapter* ina = nullptr,
                I2cBus* rtd_bus_override = nullptr,
                SpiBus* click1_bus_override = nullptr,
                SpiBus* click2_bus_override = nullptr);
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

  // Component state for a successful RTD-card poll, from how many of its
  // channels validated. All → OK; some → DEGRADED (PARTIAL_CHANNELS); NONE
  // → FAILED (NO_VALID_CHANNELS): a card whose bus answers perfectly but
  // measures nothing is a failed instrument, not a degraded one (bench
  // 2026-08-29: all 8 harness channels open/short read as DEGRADED, which
  // suggested partial function that did not exist). Static for testability;
  // `error` receives the matching error token.
  static ComponentState RtdStateForValidCount(std::size_t valid_count,
                                              std::size_t channel_count,
                                              std::string* error);

 private:
  bool ReadDps310At(int address, double* temp_c, double* pressure_mbar);
  bool ReadAds1115At(int address, double* voltage);
  SensorSnapshot ReadSimulatedSnapshot(MissionPhase phase,
                                       const std::vector<double>& heater_duty,
                                       double dt_seconds);
  void DpsLoop();
  void AdsLoop();
  void SequentRtdLoop();
  void Max31865Loop();
  // Shared by Max31865Loop's poll pass and ActiveCheck's on-demand
  // check_max31865, so a passing/failing CHECK MAX31865 updates
  // max31865_health_/click_has_success_/click_last_success_ exactly the way
  // a worker poll would, instead of leaving stale health until the next
  // poll tick. Caller must already hold cache_mu_.
  void UpdateClickHealth(int click, bool ok,
                        const Max31865Adapter::Reading& reading,
                        const std::string& error,
                        const std::chrono::steady_clock::time_point& now);
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
  // Bus-level health of the Sequent RTD card: true only when the last
  // Probe/ReadAll conversation with the card actually succeeded. This is
  // distinct from rtd_health_, which also factors in per-channel plausibility
  // (open/short detection) that a bus-healthy card can still fail on one
  // sensor. AND-ed into i2c_ok_ in ReadSnapshot so I2C_FAIL asserts on an RTD
  // bus fault instead of staying silent once DPS310/ADS1115 are disabled.
  std::atomic<bool> rtd_bus_ok_{false};
  std::atomic<bool> sample_temp_ok_{false};
  std::atomic<bool> uv_ok_{false};
  // Bus-level health of the two MAX31865 clicks: true only when the last
  // one-shot conversation with BOTH clicks actually succeeded. Saturation
  // (Reading.valid == false, Reading.out_of_range == true) is a VALID
  // measurement of an out-of-range specimen -- the bus is fine, the channel
  // just characterised out of window -- so it must never key this flag; only
  // ReadOneShot's own call result (Max31865Loop's `ok[click]`) does. Read by
  // ReadSnapshot's max31865_click dispatch branch exactly the way
  // rtd_bus_ok_ feeds the sequent_rtd branch.
  std::atomic<bool> clicks_bus_ok_{false};
  bool simulated_ = false;
  mutable std::mutex cache_mu_;
  std::condition_variable stop_cv_;
  mutable std::mutex stop_mu_;
  std::atomic<bool> running_{false};
  std::thread dps_thread_;
  std::thread ads_thread_;
  std::thread rtd_thread_;
  std::thread max31865_thread_;
  ScalarCache ambient_temp_cache_;
  ScalarCache pressure_cache_;
  ScalarCache uv_cache_;
  std::vector<ScalarCache> sample_cache_;
  ComponentHealth dps_health_;
  ComponentHealth ads_health_;
  ComponentHealth rtd_health_;
  // Index 0 = click 0 (SAMPLE1), index 1 = click 1 (SAMPLE2). Written only
  // by Max31865Loop, under cache_mu_, exactly like rtd_health_.
  std::array<ComponentHealth, 2> max31865_health_;
  // Per-click last-successful-conversation bookkeeping for FailedState(),
  // mirroring ScalarCache's has_value/last_success pair. Written only by
  // Max31865Loop, under cache_mu_.
  bool click_has_success_[2] = {false, false};
  std::chrono::steady_clock::time_point click_last_success_[2];
  // Saturation-edge visibility. A specimen going out of range is a valid
  // measurement (bus healthy, channel invalid), so it never trips
  // clicks_bus_ok_ and the only wire evidence is a "-" appearing in a
  // RESISTANCE slot -- indistinguishable from an unmonitored sample. These
  // two carry a rate-limited stderr line on the valid -> out_of_range EDGE so
  // the operator sees it happen. Written only under cache_mu_, alongside the
  // health they describe. No wire change: this is a log line, nothing more.
  bool click_was_out_of_range_[2] = {false, false};
  std::chrono::steady_clock::time_point click_last_saturation_log_[2];
  bool click_has_saturation_log_[2] = {false, false};
  int resolved_dps_address_ = -1;
  int resolved_ads_address_ = -1;
  mutable std::mutex dps_io_mu_;
  mutable std::mutex ads_io_mu_;
  mutable std::mutex rtd_io_mu_;
  // Guards both click1_ and click2_'s SPI conversations. A single mutex
  // (rather than one per click) is deliberate: Max31865Loop always talks to
  // both clicks back-to-back every poll, and ActiveCheck's on-demand
  // MAX31865 conversation does the same, so there is no scenario where
  // holding it for both calls costs real concurrency -- it only has to keep
  // the worker and an on-demand CHECK from interleaving mid-conversation.
  mutable std::mutex clicks_io_mu_;

  // Declared last so the constructor initialiser list can stay in
  // declaration order; rtd_ holds a pointer to rtd_bus_active_, so both
  // must precede it here. Same reasoning extends to the two MAX31865 click
  // buses/adapters declared alongside it.
  LinuxI2cBus rtd_bus_;
  // The bus rtd_ and every rtd_bus_.available() check actually use: the
  // constructor sets this to rtd_bus_override when a test supplies one,
  // otherwise &rtd_bus_. Everything downstream reads through this pointer
  // rather than rtd_bus_ directly, so an injected fake really does control
  // whether the RTD worker thread starts and what it sees as available.
  I2cBus* rtd_bus_active_ = nullptr;
  SequentRtdAdapter rtd_;
  SequentRtdAdapter::Identity rtd_identity_;
  bool rtd_probed_ = false;
  SequentRtdAdapter::Reading rtd_last_reading_;
  bool rtd_has_reading_ = false;
  // Edge state for SequentRtdLoop's channel-diagnosis journal line: log
  // when the per-channel fault pattern changes, at most once a minute.
  std::array<RtdChannelFault, SequentRtdAdapter::kChannelCount>
      rtd_last_fault_pattern_{};
  bool rtd_has_fault_pattern_ = false;
  std::chrono::steady_clock::time_point rtd_last_fault_log_{};
  bool rtd_has_fault_log_ = false;

  // Owned SPI transports for the two MAX31865 clicks (production: real
  // LinuxSpiBus, one per click since each is a distinct spidev device).
  LinuxSpiBus click1_bus_;
  LinuxSpiBus click2_bus_;
  // The bus click1_/click2_ and every click*_bus_active_->available() check
  // actually use: the constructor sets these to click1/2_bus_override when a
  // test supplies one, otherwise &click1_bus_/&click2_bus_ -- the same
  // override-pointer pattern as rtd_bus_active_ above.
  SpiBus* click1_bus_active_ = nullptr;
  SpiBus* click2_bus_active_ = nullptr;
  Max31865Adapter click1_;
  Max31865Adapter click2_;
};

}  // namespace coatheal
