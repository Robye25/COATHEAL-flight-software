// Rev C SensorManager resistance compatibility coverage.

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/hal/i2c_adapter.hpp"
#include "coatheal/hal/ina3221_adapter.hpp"
#include "coatheal/hal/rtc_adapter.hpp"
#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "coatheal/hal/spi_adapter.hpp"
#include "coatheal/hal/spi_bus.hpp"
#include "coatheal/sensor_manager.hpp"
#include "coatheal/telemetry.hpp"
#include "fake_i2c_bus.hpp"
#include "fake_spi_bus.hpp"

using namespace coatheal;

namespace {

// Mirrors BlankImage() in test_sequent_rtd_adapter.cpp: the minimum register
// image for Probe()/ReadAll() to succeed structurally against a
// default-configured SequentRtdAdapter (PT100, stack 0, channel_map 1..8).
std::vector<std::uint8_t> BlankRtdImage() {
  std::vector<std::uint8_t> image(140, 0);
  image[sequent_rtd::kCardType] = 7;   // hardware >= 5.0
  image[sequent_rtd::kRevMajor] = 1;
  image[sequent_rtd::kRevMinor] = 5;
  image[sequent_rtd::kRevHwMajor] = 7;
  image[sequent_rtd::kRevHwMinor] = 0;
  image[sequent_rtd::kPt1000] = 0;     // PT100, matches OnboardConfig default
  return image;
}

SensorManager MakeSensorManager(Ina3221Adapter* ina,
                                SpiAdapter* spi,
                                I2cAdapter* i2c,
                                RtcAdapter* rtc) {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.runtime.use_simulated_sensors = true;
  config.sensors.resistance_source = "disabled";
  return SensorManager(config, spi, i2c, rtc, ina);
}

void TestDisabledResistanceSerializesAsDashes() {
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm = MakeSensorManager(&ina, &spi, &i2c, &rtc);

  const SensorSnapshot snap = sm.ReadSnapshot(MissionPhase::kAscent,
                                              std::vector<double>(6, 0.0),
                                              0.1);
  assert(sm.resistance_ok());
  assert(snap.sample_resistance_ohm.size() == 8);
  for (double value : snap.sample_resistance_ohm) {
    assert(value == 0.0);
  }

  TelemetryRecord rec;
  rec.seq = 0;
  rec.sensors = snap;
  rec.phase = MissionPhase::kAscent;
  rec.mode = SystemMode::kStandby;
  rec.heater_duty = std::vector<double>(6, 0.0);
  rec.steppers.resize(2);
  const std::string line = SerializeTelemetryDataFrame(rec, "sess-init");

  const auto pos = line.find("RESISTANCE=");
  assert(pos != std::string::npos);
  const auto end = line.find(',', pos);
  const std::string rest = line.substr(pos, end - pos);
  assert(rest == "RESISTANCE=-|-|-|-|-|-|-|-");
}

void TestDisabledResistanceIgnoresPullNotifications() {
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm = MakeSensorManager(&ina, &spi, &i2c, &rtc);

  for (int k = 0; k < 5; ++k) {
    sm.NotePullCompleted(1);
  }
  const SensorSnapshot snap = sm.ReadSnapshot(MissionPhase::kFloat,
                                              std::vector<double>(6, 0.0),
                                              0.1);
  for (double value : snap.sample_resistance_ohm) {
    assert(value == 0.0);
  }
}

void TestMissingSensorsReturnImmediatelyWithInvalidNanValues() {
  OnboardConfig config;
  config.runtime.use_simulated_sensors = false;
  config.sensors.dps310_enabled = false;
  config.sensors.ads1115_enabled = false;
  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina);
  sm.Start();
  const auto begin = std::chrono::steady_clock::now();
  const SensorSnapshot snap = sm.ReadSnapshot(
      MissionPhase::kAscent, std::vector<double>(6, 0.0), 1.0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin);
  assert(elapsed.count() < 50);
  assert(!snap.ambient_temp_valid);
  assert(!snap.ambient_pressure_valid);
  assert(!snap.uv_valid);
  assert(std::isnan(snap.ambient_temp_c));
  assert(std::isnan(snap.ambient_pressure_mbar));
  assert(std::isnan(snap.uv));
  assert(snap.sample_temps_c.size() == 8);
  assert(snap.sample_temp_valid.size() == 8);
  for (std::size_t i = 0; i < snap.sample_temps_c.size(); ++i) {
    assert(std::isnan(snap.sample_temps_c[i]));
    assert(!snap.sample_temp_valid[i]);
    assert(snap.sample_temp_age_ms[i] == -1);
  }
  assert(snap.dps310.state == ComponentState::kDisabled);
  assert(snap.ads1115.state == ComponentState::kDisabled);
  // The Sequent card is polled unconditionally, so this slot now reports the
  // real card. On a host with no Linux I2C bus that is DISABLED; on Linux
  // with no card wired it is DISCOVERING then FAILED. What holds everywhere
  // is that a card which never answered is never reported OK.
  assert(snap.sequent_rtd.state != ComponentState::kOk);
  // Smoke check only: every sample is invalid in this fixture, so this
  // assertion holds under both the old any_of policy and the new
  // heated-channels policy. The policy itself is covered by the
  // HeatedChannelsValid tests below, which were verified to fail when the
  // policy is mutated. Differentiating coverage at the ReadSnapshot level
  // needs an I2cBus injection seam that SensorManager does not yet have:
  // sample_cache_ entries only become valid inside SequentRtdLoop, off a
  // real card, and no public API can mark one valid from a test.
  assert(!sm.sample_temp_ok());
  sm.Stop();
}

// SensorManager::Pt100TemperatureFromResistance is now a thin forwarder onto
// the HAL's Pt100TemperatureFromOhms. These are the conversion assertions
// that used to live in TestMax31865Pt100Conversion; the MAX31865 raw-code
// half of that test went away with the RTD Click driver.
void TestPt100WrapperStillForwards() {
  double temp = 0.0;
  assert(SensorManager::Pt100TemperatureFromResistance(100.0, &temp));
  assert(std::fabs(temp) < 0.05);

  assert(SensorManager::Pt100TemperatureFromResistance(138.5055, &temp));
  assert(std::fabs(temp - 100.0) < 0.1);

  assert(SensorManager::Pt100TemperatureFromResistance(80.306, &temp));
  assert(std::fabs(temp - (-50.0)) < 0.2);

  // Out of PT100 range: the wrapper must propagate the rejection, not
  // silently succeed with a clamped value.
  assert(!SensorManager::Pt100TemperatureFromResistance(1000.0, &temp));
}

void TestHeatedChannelPolicyIgnoresUnheatedSamples() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.heaters.temperature_channels = {0, 1, 2, 3, 4, 5};

  // Samples 6 and 7 are pulled but unheated: their validity must not
  // affect sample_temp_ok_.
  std::vector<bool> valid(8, true);
  valid[6] = false;
  valid[7] = false;
  assert(SensorManager::HeatedChannelsValid(config, valid));

  valid[3] = false;  // a heated channel drops
  assert(!SensorManager::HeatedChannelsValid(config, valid));
}

void TestHeatedChannelPolicyRejectsOutOfRangeMapping() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.heaters.temperature_channels = {0, 1, 99};
  const std::vector<bool> valid(8, true);
  // A mapping past the end of the sample vector must fail closed.
  assert(!SensorManager::HeatedChannelsValid(config, valid));

  // Same policy seen from the other side: an in-range mapping against a
  // short validity vector is still a mapping past the end. This is the case
  // that distinguishes "fail closed" from "skip channels we cannot index",
  // which would otherwise report the thermal path healthy off two channels.
  const std::vector<bool> short_valid(2, true);
  config.heaters.temperature_channels = {0, 1, 2};
  assert(!SensorManager::HeatedChannelsValid(config, short_valid));

  // And the same helper does say yes once the vector actually covers the
  // mapping, so the assertions above are rejecting the mapping and not the
  // helper rejecting everything.
  const std::vector<bool> long_enough(3, true);
  assert(SensorManager::HeatedChannelsValid(config, long_enough));
}

void TestHeatedChannelPolicyFailsClosedOnEmptyMapping() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.heaters.temperature_channels.clear();
  const std::vector<bool> all_valid(8, true);
  // An empty mapping means we cannot know which channels gate heating.
  // Fail closed: never report the thermal path as safe on a config error.
  assert(!SensorManager::HeatedChannelsValid(config, all_valid));
}

// Shared setup for the two i2c_ok()/RTD-bus-health tests below. DPS310 and
// ADS1115 are disabled so the `!enabled || valid` terms in ReadSnapshot's
// i2c_ok_ formula both collapse to `true`, leaving i2c_ok() driven entirely
// by rtd_bus_ok_ — exactly the scenario the controller ruling calls out
// ("with DPS310 and ADS1115 disabled, I2C_OK reports healthy while the RTD
// card is dead"). sequent_rtd_poll_ms is set small so the bounded waits
// below stay short without racing the worker thread's first pass.
OnboardConfig MakeRtdBusTestConfig() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.runtime.use_simulated_sensors = false;
  config.sensors.dps310_enabled = false;
  config.sensors.ads1115_enabled = false;
  config.sensors.sequent_rtd_poll_ms = 5;
  return config;
}

// i2c_ok's RTD contribution is bus-level (Probe/ReadAll succeeding), not
// per-channel plausibility, so a card that never lets the bus open is the
// right fixture: it never has to touch resistance/temperature plausibility
// to prove the point.
void TestRtdStateForValidCountTriState() {
  // ALL valid -> OK, SOME -> DEGRADED, NONE -> FAILED. The last branch is
  // the 2026-08-29 bench fix: a card whose bus answers perfectly while
  // every probe is open/shorted used to report DEGRADED, which suggested
  // partial function that did not exist.
  std::string error;
  assert(SensorManager::RtdStateForValidCount(8, 8, &error) ==
         ComponentState::kOk);
  assert(error == "NONE");
  assert(SensorManager::RtdStateForValidCount(3, 8, &error) ==
         ComponentState::kDegraded);
  assert(error == "PARTIAL_CHANNELS");
  assert(SensorManager::RtdStateForValidCount(0, 8, &error) ==
         ComponentState::kFailed);
  assert(error == "NO_VALID_CHANNELS");
}

// The exact register image the bench harness produced on 2026-08-29:
// ±366.000 Ω open sentinels on seven channels, a 0.2 Ω short on one, with
// the card's own (nonsense) temperatures alongside.
std::vector<std::uint8_t> OpenHarnessRtdImage() {
  std::vector<std::uint8_t> image = BlankRtdImage();
  const float temps[8] = {-1210.39f, 690.909f, -259.207f, 690.909f,
                          690.909f, -1210.39f, -1210.39f, 690.909f};
  const float res[8] = {-366.0f, 366.0f, 0.205f, 366.0f,
                        366.0f, -366.0f, -366.0f, 366.0f};
  for (int channel = 0; channel < sequent_rtd::kChannels; ++channel) {
    std::memcpy(image.data() + sequent_rtd::kRtdVal1 + channel * 4,
                &temps[channel], sizeof(float));
    std::memcpy(image.data() + sequent_rtd::kRtdRes1 + channel * 4,
                &res[channel], sizeof(float));
  }
  return image;
}

void TestOpenHarnessReportsFailedWithChannelDiagnosis() {
  const OnboardConfig config = MakeRtdBusTestConfig();

  FakeI2cBus bus;
  bus.SetImage(OpenHarnessRtdImage());

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &bus);
  sm.Start();

  std::string summary;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    summary = sm.ComponentSummary();
    if (summary.find("sequent_rtd=FAILED") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();

  // Zero valid channels on a healthy bus is a FAILED instrument, and the
  // reply must carry the per-channel diagnosis a technician can act on.
  assert(summary.find("sequent_rtd=FAILED") != std::string::npos);
  assert(summary.find("sequent_rtd_error=NO_VALID_CHANNELS") !=
         std::string::npos);
  assert(summary.find("sequent_rtd_valid=0/8") != std::string::npos);
  assert(summary.find("sequent_rtd_ch=S0:ch1:OPEN:-366.0") !=
         std::string::npos);
  assert(summary.find("S2:ch3:SHORT:0.2") != std::string::npos);
  assert(summary.find("S7:ch8:OPEN:366.0") != std::string::npos);
}

void TestI2cOkStaysFailedOnUnreachableRtdBus() {
  const OnboardConfig config = MakeRtdBusTestConfig();

  // A dedicated FakeI2cBus per test, mutated only before Start() and never
  // touched again from the test thread — SequentRtdLoop is the only thread
  // that reads it afterwards, so there is no cross-thread data race to
  // reason about, and the outcome does not depend on precise timing.
  FakeI2cBus bad_bus;
  bad_bus.SetImage(BlankRtdImage());
  bad_bus.SetOpenFails(true);  // card present in config, unreachable on wire

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &bad_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool ever_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.i2c_ok()) {
      ever_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // A bus that never opens must never be reported OK. This is the half that
  // catches a regression where the rtd_bus_ok_ AND is dropped from i2c_ok_'s
  // formula (with dps/ads disabled that bug makes i2c_ok() true on the very
  // first ReadSnapshot() call, regardless of the RTD card).
  assert(!ever_ok);
}

void TestI2cOkRecoversOnHealthyRtdBus() {
  const OnboardConfig config = MakeRtdBusTestConfig();

  FakeI2cBus good_bus;
  good_bus.SetImage(BlankRtdImage());

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &good_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool became_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.i2c_ok()) {
      became_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // A card that answers cleanly must bring i2c_ok() up. This is the half
  // that catches rtd_bus_ok_ being wired dead (e.g. never set, or the AND
  // term inverted/stuck false) — that bug would time out here instead.
  assert(became_ok);
}

// ---------------------------------------------------------------------------
// resistance_source=sequent_rtd (the shipped default) coverage.
//
// The two tests below are a pair, and only one of them is discriminating on
// its own. Without an explicit "sequent_rtd" branch in ReadSnapshot the value
// falls through to `ina_ != nullptr && ina_->healthy()` — the retired INA3221
// stub, whose healthy_ defaults to true and is never written by anything. That
// fallback therefore reports resistance_ok() == true unconditionally, so it
// happens to agree with the correct branch on a healthy card and disagrees
// only on a dead one. TestSequentRtdResistanceFailsOnUnreachableBus is the
// half that fails if the branch is removed; the healthy half is what proves
// the failing half is not passing for a trivial reason (e.g. a fixture where
// the bus never comes up at all).

// A register image with a distinctive, in-window resistance on every channel:
// 138.5055 Ω is the PT100 CVD resistance at exactly 100 °C, so the matching
// 100.0 °C temperature block passes the card-vs-CVD cross-check (default
// tolerance 2 °C) and every channel comes back valid. It is deliberately not
// the 100.0 Ω that SensorManager seeds sample_resistance_ohm_ with, so a
// snapshot carrying 138.5055 proves the card's own numbers reached the wire
// rather than the constructor's placeholder.
constexpr double kBenchResistanceOhm = 138.5055;
constexpr double kBenchTemperatureC = 100.0;

std::vector<std::uint8_t> LiveRtdImage() {
  std::vector<std::uint8_t> image = BlankRtdImage();
  for (int channel = 0; channel < sequent_rtd::kChannels; ++channel) {
    const float temperature = static_cast<float>(kBenchTemperatureC);
    const float resistance = static_cast<float>(kBenchResistanceOhm);
    std::memcpy(image.data() + sequent_rtd::kRtdVal1 + channel * 4,
                &temperature, sizeof(float));
    std::memcpy(image.data() + sequent_rtd::kRtdRes1 + channel * 4,
                &resistance, sizeof(float));
  }
  return image;
}

OnboardConfig MakeSequentRtdResistanceConfig() {
  OnboardConfig config = MakeRtdBusTestConfig();
  config.sensors.resistance_source = "sequent_rtd";
  return config;
}

void TestSequentRtdResistanceFollowsHealthyBus() {
  const OnboardConfig config = MakeSequentRtdResistanceConfig();

  FakeI2cBus good_bus;
  good_bus.SetImage(LiveRtdImage());

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &good_bus);
  sm.Start();

  // Wait on the card's data reaching the snapshot, not on resistance_ok():
  // the flag is set on the very first ReadSnapshot() and would let the loop
  // exit before SequentRtdLoop's first pass had published anything, leaving
  // the assertions looking at the constructor's placeholder ohms.
  const std::vector<double> heater_duty(6, 0.0);
  SensorSnapshot snap;
  bool published = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    snap = sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (!snap.sample_resistance_ohm.empty() &&
        std::fabs(snap.sample_resistance_ohm[0] - kBenchResistanceOhm) < 0.01) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool resistance_ok = sm.resistance_ok();
  sm.Stop();

  assert(published);
  assert(resistance_ok);
  assert(snap.sample_resistance_ohm.size() == 8);
  for (double ohms : snap.sample_resistance_ohm) {
    assert(std::fabs(ohms - kBenchResistanceOhm) < 0.01);
  }

  // Same values seen through the wire format: RESISTANCE= must carry real
  // numbers, not the "-" placeholder the disabled source emits.
  TelemetryRecord rec;
  rec.seq = 0;
  rec.sensors = snap;
  rec.phase = MissionPhase::kAscent;
  rec.mode = SystemMode::kStandby;
  rec.heater_duty = heater_duty;
  rec.steppers.resize(2);
  const std::string line = SerializeTelemetryDataFrame(rec, "sess-rtd");
  const auto pos = line.find("RESISTANCE=");
  assert(pos != std::string::npos);
  const std::string rest = line.substr(pos, line.find(',', pos) - pos);
  assert(rest.find('-') == std::string::npos);
  assert(rest.find("138.5") != std::string::npos);
}

void TestSequentRtdResistanceFailsOnUnreachableBus() {
  const OnboardConfig config = MakeSequentRtdResistanceConfig();

  FakeI2cBus bad_bus;
  bad_bus.SetImage(LiveRtdImage());
  bad_bus.SetOpenFails(true);  // card configured, unreachable on the wire

  Ina3221Adapter ina;
  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, &ina, &bad_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool ever_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.resistance_ok()) {
      ever_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // A card that never answers must never report its resistance data healthy.
  // Deleting the sequent_rtd branch drops this case onto the INA3221 stub's
  // always-true healthy(), which fails here and only here.
  assert(!ever_ok);
}

// ---------------------------------------------------------------------------
// resistance_source=max31865_click (the v3-shipped default) coverage.
//
// Hand-computed scripted ohms (R = code * reference_ohm / 32768,
// reference_ohm defaults to 470.0): code 16384 (raw 0x8000, MSB=0x80,
// LSB=0x00, fault bit clear) -> 16384*470/32768 = 235.0 ohm exact (16384 is
// exactly half of 32768). Code 8192 (raw 0x4000, MSB=0x40, LSB=0x00) ->
// 8192*470/32768 = 117.5 ohm exact (matches the figure already pinned in
// test_max31865_adapter.cpp). Click 0 (SAMPLE1, /dev/spidev0.1) is scripted
// with 235.0 and click 1 (SAMPLE2, /dev/spidev0.0) with 117.5 so the two
// monitored indices are independently distinguishable in an assertion.
//
// Every SensorManager below is constructed with ina=nullptr (unlike the
// sequent_rtd fixtures above, which intentionally keep it): the "disabled"
// resistance_source branch's flat 0.0 padding is fine either way, but the
// `ina_ != nullptr && ina_->healthy()` fallback that "sequent_rtd" relies on
// its *failing* test to catch (see the comment block above) would instead
// make deleting the max31865_click dispatch branch invisible to the HEALTHY
// test here (Max31865Loop still populates sample_resistance_ohm_ under the
// hood; the INA stub's always-true healthy() would still report
// resistance_ok()==true and the branch-less snapshot would still forward the
// same vector). ina=nullptr routes a dropped branch to the final `else`
// (resistance_ok_=false, vector padded to 0.0) instead, so the healthy test
// below is the one that catches it, matching the plan's mutation list.
//
// Timing choice: MakeMax31865Options() (sensor_manager.cpp) does not
// override Max31865Adapter::Options' settle_ms/conversion_ms, so every
// ReadOneShot() here really sleeps ~75 ms (10 + 65, the datasheet minimums)
// per click -- the plan explicitly sanctions this ("accept the ~75ms ...
// budget deadlines generously: 3s") rather than adding a fourth config key
// or a narrow test-only timing hook. To keep the strict, finite FakeSpiBus
// expectation queue from being drained by a second worker pass mid-test
// (the queue holds exactly one scripted one-shot sequence per click), the
// healthy/saturation fixtures below set max31865_poll_ms to 5 s: Stop() is
// called as soon as the first pass is observed, well inside that window, so
// only one pass ever runs.
constexpr double kClick0ResistanceOhm = 235.0;  // code 16384 @ 470 ohm ref
constexpr double kClick1ResistanceOhm = 117.5;  // code 8192 @ 470 ohm ref

OnboardConfig MakeMax31865TestConfig() {
  OnboardConfig config;
  config.hardware.sample_count = 8;
  config.hardware.heater_count = 6;
  config.runtime.use_simulated_sensors = false;
  config.sensors.dps310_enabled = false;
  config.sensors.ads1115_enabled = false;
  config.sensors.resistance_source = "max31865_click";
  config.sensors.max31865_sample_indices = {0, 4};
  return config;
}

// The exact error string an absent / non-answering click reports, asserted
// on the CHECK wire below. Distinct from every transport error on purpose.
constexpr char kClickNotDetected[] = "CLICK_NOT_DETECTED";

// Every ReadOneShot() now opens with a presence check -- a benign config
// write (VBIAS off, 50 Hz) and a readback of the SAME value, modelling the
// config register the way a real MAX31865 behaves: it latches what was
// written and hands it back. A click that is not on the bus cannot do
// that, which is the whole point (see the absent-click tests below).
void ScriptPresenceCheck(FakeSpiBus* bus) {
  bus->Expect({0x80, 0x01}, {0, 0});        // benign config write
  bus->Expect({0x00, 0}, {0, 0x01});        // ...read straight back
}

void ScriptHealthyOneShot(FakeSpiBus* bus, std::uint8_t msb, std::uint8_t lsb) {
  ScriptPresenceCheck(bus);
  bus->Expect({0x80, 0x81}, {0, 0});        // VBIAS on
  bus->Expect({0x80, 0xA1}, {0, 0});        // 1SHOT
  bus->Expect({0x01, 0, 0}, {0, msb, lsb}); // RTD MSB/LSB, no fault
  bus->Expect({0x80, 0x01}, {0, 0});        // VBIAS off
}

void ScriptSaturatedOneShot(FakeSpiBus* bus, std::uint8_t msb, std::uint8_t lsb,
                            std::uint8_t fault_byte) {
  ScriptPresenceCheck(bus);
  bus->Expect({0x80, 0x81}, {0, 0});             // VBIAS on
  bus->Expect({0x80, 0xA1}, {0, 0});             // 1SHOT
  bus->Expect({0x01, 0, 0}, {0, msb, lsb});      // RTD MSB/LSB, fault bit set
  bus->Expect({0x80, 0x01}, {0, 0});             // VBIAS off
  bus->Expect({0x07, 0}, {0, fault_byte});       // fault status read
  bus->Expect({0x80, 0x03}, {0, 0});             // FAULTCLR write
}

void TestMax31865HealthyClicksPopulateOnlyMonitoredIndices() {
  OnboardConfig config = MakeMax31865TestConfig();
  config.sensors.max31865_poll_ms = 5000;  // see the timing-choice comment above
  // Fix-round 1 Critical 2: the RTD worker must be live in THIS test too,
  // not merely absent (rtd_bus_override left nullptr made this assertion
  // vacuous -- LinuxI2cBus::available() is compile-time false on this host,
  // so SequentRtdLoop never started and could never have written the
  // "unmonitored" indices this test checks below). A dedicated FakeI2cBus,
  // scripted healthy on every channel via the existing LiveRtdImage()
  // helper, makes SequentRtdLoop actually run concurrently with
  // Max31865Loop -- the real flight-hardware scenario Critical 1 was about.
  config.sensors.sequent_rtd_poll_ms = 5;
  FakeI2cBus good_rtd_bus;
  good_rtd_bus.SetImage(LiveRtdImage());

  FakeSpiBus click1_bus;  // click 0, SAMPLE1 -> index 0
  FakeSpiBus click2_bus;  // click 1, SAMPLE2 -> index 4
  ScriptHealthyOneShot(&click1_bus, 0x80, 0x00);  // code 16384 -> 235.0
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);  // code 8192 -> 117.5

  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, /*ina=*/nullptr,
                   &good_rtd_bus, &click1_bus, &click2_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  SensorSnapshot snap;
  bool published = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    snap = sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    // Wait for BOTH the click resistance AND the RTD temperature to have
    // been published at least once, so a passing test proves the injected
    // RTD fake actually ran concurrently rather than the loop exiting
    // before SequentRtdLoop's first pass.
    if (!snap.sample_resistance_ohm.empty() &&
        std::fabs(snap.sample_resistance_ohm[0] - kClick0ResistanceOhm) <
            0.001 &&
        !snap.sample_temp_valid.empty() && snap.sample_temp_valid[0]) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool resistance_ok = sm.resistance_ok();
  sm.Stop();

  assert(published);
  // Bus-level health, not per-channel: both clicks' one-shot conversations
  // succeeded, so resistance_ok() must be true.
  assert(resistance_ok);
  assert(snap.sample_resistance_ohm.size() == 8);
  assert(std::fabs(snap.sample_resistance_ohm[0] - kClick0ResistanceOhm) <
        0.001);
  assert(std::fabs(snap.sample_resistance_ohm[4] - kClick1ResistanceOhm) <
        0.001);
  // Every unmonitored index must stay exactly 0.0 -- Max31865Loop must never
  // write outside the two configured sample_indices entries, AND
  // SequentRtdLoop must not have written its own (different-quantity)
  // element ohms into any of them either -- this is the Critical-1 defect
  // this injected RTD fake exists to make observable.
  for (std::size_t i = 0; i < snap.sample_resistance_ohm.size(); ++i) {
    if (i == 0 || i == 4) continue;
    assert(snap.sample_resistance_ohm[i] == 0.0);
  }
  // Companion assertion (not vacuous): the RTD fake really is live and
  // driving the temperature path, independent of resistance_source.
  assert(snap.sample_temp_valid[0]);
  assert(std::fabs(snap.sample_temps_c[0] - kBenchTemperatureC) < 0.5);
}

void TestMax31865OneClickBusFailureFailsResistanceOk() {
  OnboardConfig config = MakeMax31865TestConfig();
  config.sensors.max31865_poll_ms = 5;

  FakeSpiBus click1_bus;  // click 0 never opens: a genuine bus failure.
  click1_bus.SetOpenFails(true);
  FakeSpiBus click2_bus;  // click 1 healthy -- proves the AND, not just OR.
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);

  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, /*ina=*/nullptr,
                   /*rtd_bus_override=*/nullptr, &click1_bus, &click2_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  bool ever_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.resistance_ok()) {
      ever_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // One click never answering must never report resistance_ok() true, even
  // though the other click is perfectly healthy every pass.
  assert(!ever_ok);
}

void TestMax31865SaturatedReadingKeepsClicksBusOkWhileIndexReadsZero() {
  OnboardConfig config = MakeMax31865TestConfig();
  config.sensors.max31865_poll_ms = 5000;  // see the timing-choice comment above

  FakeSpiBus click1_bus;  // click 0, SAMPLE1 -> index 0, healthy.
  FakeSpiBus click2_bus;  // click 1, SAMPLE2 -> index 4, SATURATED.
  ScriptHealthyOneShot(&click1_bus, 0x80, 0x00);       // code 16384 -> 235.0
  ScriptSaturatedOneShot(&click2_bus, 0x40, 0x01, 0x04); // fault bit set

  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, /*ina=*/nullptr,
                   /*rtd_bus_override=*/nullptr, &click1_bus, &click2_bus);
  sm.Start();

  const std::vector<double> heater_duty(6, 0.0);
  SensorSnapshot snap;
  bool published = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    snap = sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    // click1's write and the saturated click2's write land under the same
    // cache_mu_ critical section within one Max31865Loop pass (see the
    // implementation), so observing index 0 here is proof index 4 has
    // already been written too, saturated or not.
    if (!snap.sample_resistance_ohm.empty() &&
        std::fabs(snap.sample_resistance_ohm[0] - kClick0ResistanceOhm) <
            0.001) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool resistance_ok = sm.resistance_ok();
  sm.Stop();

  assert(published);
  // THE finding-class assertion, both directions in one test: a saturated
  // specimen reading is a VALID measurement (bus healthy, channel
  // out-of-range) so resistance_ok() must stay true here...
  //
  // This is also the anti-regression guard for the absent-click fix below:
  // click 1 answers the presence check and is genuinely on the bus, so
  // "not answering" and "answering with an out-of-range specimen" must
  // stay two different verdicts. Any attempt to close the absent-click
  // hole by keying health off Reading.valid fails right here.
  assert(resistance_ok);
  // ...while the saturated channel's own index carries the wire "not valid"
  // 0.0 convention -- never the diagnostic resistance_ohm value the fault
  // reading still computed internally.
  assert(snap.sample_resistance_ohm[4] == 0.0);
  assert(std::fabs(snap.sample_resistance_ohm[0] - kClick0ResistanceOhm) <
        0.001);
}

// ---------------------------------------------------------------------------
// THE REGRESSION TEST FOR THE PI BRING-UP FINDING.
//
// Observed on real hardware with NO MAX31865 clicks attached: the onboard
// put RESISTANCE_OK on the wire and max31865_1=OK / max31865_2=OK in CHECK
// while r0..r7 all serialised as "-". A silent loss of the science
// instrument -- the one failure that looks exactly like success.
//
// The cause is that SPI cannot detect absence at the transport layer.
// Nothing acknowledges and nothing NAKs: the master clocks bits and samples
// an idle MISO line, so every transfer against an empty socket SUCCEEDS,
// the RTD register reads back 0x0000, the fault bit is clear, and
// ReadOneShot() reported a healthy measurement. clicks_bus_ok_ -- and
// therefore resistance_ok() -- is keyed off exactly that return value.
// (Contrast the neighbours, which self-detect and behaved correctly on the
// same bring-up: the Sequent RTD card is I2C and never ACKed
// (CARD_NOT_DETECTED); the TMC5160 has a VERSION identity byte.)
//
// FakeSpiBus::SetFloatingLevel() models absence -- successful transfers
// clocking back the idle level -- which no scripted expectation can
// express. Both idle levels are covered because both occur in the field.
//
// Mutation target: remove the presence check from Max31865Adapter (or make
// the absent case return true). Both halves below flip: resistance_ok()
// goes true with every channel blank, and CHECK reports max31865_1=OK.
// ---------------------------------------------------------------------------

void AssertAbsentClickFailsResistanceAndCheck(std::uint8_t idle_level) {
  OnboardConfig config = MakeMax31865TestConfig();
  config.sensors.max31865_poll_ms = 5;

  FakeSpiBus click1_bus;  // click 0: NOT FITTED -- MISO idles.
  FakeSpiBus click2_bus;  // click 1: fitted and perfectly healthy.
  click1_bus.SetFloatingLevel(idle_level);

  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, /*ina=*/nullptr,
                   /*rtd_bus_override=*/nullptr, &click1_bus, &click2_bus);

  // --- Half 1: the CHECK surface, driven synchronously (no worker
  // thread), so click2's finite script is consumed exactly once here.
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);
  std::string details;
  assert(!sm.ActiveCheck("MAX31865", &details));
  assert(details.find("max31865_1=FAIL") != std::string::npos);
  assert(details.find(std::string("max31865_1_error=") + kClickNotDetected) !=
         std::string::npos);
  // The fitted click is unaffected: absence is diagnosed per click, and a
  // healthy neighbour is not dragged down with it.
  assert(details.find("max31865_2=OK") != std::string::npos);

  // --- Half 2: the wire health flag. The worker polls click1 forever
  // (absence needs no script), so resistance_ok() must never once read
  // true across the whole window.
  sm.Start();
  const std::vector<double> heater_duty(6, 0.0);
  bool ever_ok = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  while (std::chrono::steady_clock::now() < deadline) {
    sm.ReadSnapshot(MissionPhase::kAscent, heater_duty, 0.1);
    if (sm.resistance_ok()) {
      ever_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sm.Stop();
  // RESISTANCE_FAIL, not RESISTANCE_OK: a click that is not on the bus is
  // not a healthy resistance channel, no matter what the transport says.
  assert(!ever_ok);
}

void TestMax31865AbsentClickFloatingLowFailsResistanceOk() {
  // Floating / pulled-down MISO reads 0x00 -- the level the flight stack
  // actually reported.
  AssertAbsentClickFailsResistanceAndCheck(0x00);
}

void TestMax31865AbsentClickFloatingHighFailsResistanceOk() {
  // Some boards idle the line high; 0xFF must be caught just the same.
  AssertAbsentClickFailsResistanceAndCheck(0xFF);
}

// ---------------------------------------------------------------------------
// Fix-round 1 Important 3 + Important 4: click health surface coverage
// (ComponentSummary()/ActiveCheck("MAX31865")), combined into one three-phase
// test since all three states chain naturally through the same click1_bus.
//
// No sm.Start() anywhere in this test: ActiveCheck()/check_max31865 is a
// synchronous, on-demand conversation independent of the worker poll loop
// (see sensor_manager.cpp), so every phase can be driven directly with no
// background thread to race or to wait out on a deadline.
void TestMax31865ActiveCheckAndComponentSummaryReflectHealthTransitions() {
  OnboardConfig config = MakeMax31865TestConfig();

  FakeSpiBus click1_bus;  // driven through all three phases below.
  FakeSpiBus click2_bus;  // stays healthy throughout -- isolates click1.

  SpiAdapter spi;
  I2cAdapter i2c;
  RtcAdapter rtc;
  SensorManager sm(config, &spi, &i2c, &rtc, /*ina=*/nullptr,
                   /*rtd_bus_override=*/nullptr, &click1_bus, &click2_bus);

  // Phase 1: click1's bus never opens -- a genuine call failure.
  click1_bus.SetOpenFails(true);
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);
  std::string details;
  assert(!sm.ActiveCheck("MAX31865", &details));
  assert(details.find("max31865_1=FAIL") != std::string::npos);
  assert(sm.ComponentSummary().find("max31865_1=FAILED") != std::string::npos);

  // Phase 2: click1 recovers. Important 4's load-bearing proof: health must
  // reflect OK immediately after THIS ActiveCheck() call -- no worker poll
  // has ever run in this test (Start() was never called), so if
  // check_max31865 did not refresh max31865_health_ itself, this would
  // still read FAILED from phase 1.
  click1_bus.SetOpenFails(false);
  ScriptHealthyOneShot(&click1_bus, 0x80, 0x00);
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);
  assert(sm.ActiveCheck("MAX31865", &details));
  assert(details.find("max31865_1=OK") != std::string::npos);
  const std::string recovered = sm.ComponentSummary();
  assert(recovered.find("max31865_1=OK") != std::string::npos);
  assert(recovered.find("max31865_1_error=NONE") != std::string::npos);

  // Phase 3: click1 answers but SATURATED (fault bit set). Important 3's
  // coverage: ComponentSummary must show DEGRADED/OUT_OF_RANGE, while
  // ActiveCheck must still report the CALL itself as OK -- the same
  // call-result-vs-Reading.valid distinction the resistance-dispatch tests
  // above prove, now proven on the on-demand CHECK path too.
  ScriptSaturatedOneShot(&click1_bus, 0x40, 0x01, 0x04);
  ScriptHealthyOneShot(&click2_bus, 0x40, 0x00);
  assert(sm.ActiveCheck("MAX31865", &details));
  assert(details.find("max31865_1=OK") != std::string::npos);
  const std::string saturated_summary = sm.ComponentSummary();
  assert(saturated_summary.find("max31865_1=DEGRADED") != std::string::npos);
  assert(saturated_summary.find("max31865_1_error=OUT_OF_RANGE") !=
        std::string::npos);
}

}  // namespace

int main() {
  TestDisabledResistanceSerializesAsDashes();
  TestDisabledResistanceIgnoresPullNotifications();
  TestMissingSensorsReturnImmediatelyWithInvalidNanValues();
  TestPt100WrapperStillForwards();
  TestHeatedChannelPolicyIgnoresUnheatedSamples();
  TestHeatedChannelPolicyRejectsOutOfRangeMapping();
  TestHeatedChannelPolicyFailsClosedOnEmptyMapping();
  TestRtdStateForValidCountTriState();
  TestOpenHarnessReportsFailedWithChannelDiagnosis();
  TestI2cOkStaysFailedOnUnreachableRtdBus();
  TestI2cOkRecoversOnHealthyRtdBus();
  TestSequentRtdResistanceFollowsHealthyBus();
  TestSequentRtdResistanceFailsOnUnreachableBus();
  TestMax31865HealthyClicksPopulateOnlyMonitoredIndices();
  TestMax31865OneClickBusFailureFailsResistanceOk();
  TestMax31865SaturatedReadingKeepsClicksBusOkWhileIndexReadsZero();
  TestMax31865AbsentClickFloatingLowFailsResistanceOk();
  TestMax31865AbsentClickFloatingHighFailsResistanceOk();
  TestMax31865ActiveCheckAndComponentSummaryReflectHealthTransitions();
  return 0;
}
