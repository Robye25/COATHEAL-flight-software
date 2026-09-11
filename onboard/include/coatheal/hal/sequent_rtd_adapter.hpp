#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "coatheal/hal/i2c_bus.hpp"

namespace coatheal {

// Register offsets for the Sequent Microsystems RTD HAT.
//
// Derived from the vendor enum in SequentMicrosystems/rtd-rpi (src/rtd.h)
// in the same order upstream derives it, rather than hand-typed, so a
// firmware map change is a one-line edit. Note kRtdRes1 is not 4-byte
// aligned; that is expected for byte-addressed memory.
namespace sequent_rtd {

inline constexpr int kChannels   = 8;
inline constexpr int kRtdVal1    = 0;                        //  0  8 x float32
inline constexpr int kDiagTemp   = kRtdVal1 + kChannels * 4; // 32  int8 degC
inline constexpr int kDiag5V     = kDiagTemp + 1;            // 33  uint16 mV
// bytes 35..54 are the I2C_MEM_WDT_* block - never read, never written
inline constexpr int kRevHwMajor = 55;
inline constexpr int kRevHwMinor = 56;
inline constexpr int kRevMajor   = 57;
inline constexpr int kRevMinor   = 58;
inline constexpr int kRtdRes1    = 59;                       // 59  8 x float32
inline constexpr int kRtdReinit  = kRtdRes1 + kChannels * 4; // 91  uint32
inline constexpr int kCardType   = 99;                       // 99  uint8
inline constexpr int kPt1000     = 133;                      // 133 uint8, 0x0f

}  // namespace sequent_rtd

// Callendar-Van Dusen inverse for PT100. Returns false outside the
// supported resistance range. Extracted here so the HAL does not depend
// on SensorManager; SensorManager::Pt100TemperatureFromResistance
// forwards to this.
bool Pt100TemperatureFromOhms(double resistance_ohm, double* temperature_c);

// Why a channel failed validation, in terms a harness technician can act
// on. Diagnosis only: control code keys off channel_valid, never off this.
enum class RtdChannelFault : std::uint8_t {
  kNone,      // channel_valid == true
  kOpen,      // card's ±366 Ω open/full-scale sentinel, above-window, or
              // non-finite: no conducting probe on the terminal block
  kShort,     // resistance below the plausible window (PT100 ~80 Ω at −50 °C)
  kMismatch,  // in-window resistance whose PT100-derived temperature
              // disagrees with the card's own reading (miswired 2/3-wire,
              // wrong sensor, drifting probe)
};

inline const char* ToString(RtdChannelFault fault) {
  switch (fault) {
    case RtdChannelFault::kNone: return "OK";
    case RtdChannelFault::kOpen: return "OPEN";
    case RtdChannelFault::kShort: return "SHORT";
    case RtdChannelFault::kMismatch: return "MISMATCH";
  }
  return "OPEN";
}

// 8-channel PT100/PT1000 acquisition on a Sequent Microsystems stackable
// RTD HAT. Byte-addressed I2C memory at 0x40 + stack.
//
// Read-only by construction: the adapter holds an I2cBus, which has no
// write method, so calibration and watchdog registers cannot be touched.
class SequentRtdAdapter {
 public:
  static constexpr std::size_t kChannelCount = 8;
  // ADDRESS COLLISION: this card owns 0x40..0x47, which covers the retired
  // INA3221's kDefaultAddrA/kDefaultAddrB (0x40/0x41) in ina3221_adapter.hpp.
  // Harmless today because Ina3221Adapter is a stub that never touches the
  // bus, but the two cannot coexist at these addresses. Re-addressing one of
  // them is a prerequisite for ever re-enabling INA3221 acquisition; keep the
  // note there in sync with this one.
  static constexpr int kAddressBase = 0x40;
  static constexpr int kStackMin = 0;
  static constexpr int kStackMax = 7;

  // Deliberately not SensorHardwareConfig: the HAL must not depend on the
  // application config struct, or the seam buys nothing. SensorManager is
  // the only place that knows about both and performs the translation.
  struct Options {
    int stack = 0;
    bool expect_pt1000 = false;
    double resistance_min_ohm = 60.0;
    double resistance_max_ohm = 390.0;
    double crosscheck_tol_c = 2.0;
    // Card channel (1-indexed) supplying each logical sample.
    std::array<std::uint8_t, kChannelCount> channel_map{1, 2, 3, 4, 5, 6, 7, 8};
  };

  struct Identity {
    std::uint8_t card_type = 0;
    std::uint8_t fw_major = 0;
    std::uint8_t fw_minor = 0;
    std::uint8_t hw_major = 0;
    std::uint8_t hw_minor = 0;
    bool pt1000 = false;
  };

  struct Reading {
    std::array<double, kChannelCount> temperature_c{};
    std::array<double, kChannelCount> resistance_ohm{};
    std::array<bool, kChannelCount> channel_valid{};
    // Per-channel diagnosis paired with channel_valid (kNone iff valid).
    std::array<RtdChannelFault, kChannelCount> channel_fault{};
    // Diagnostics only; never used for control. Byte interpretations are
    // inferred from the register map and confirmed at bench bring-up.
    double card_temp_c = 0.0;
    double rail_5v = 0.0;
    std::uint32_t adc_reinit_count = 0;
  };

  SequentRtdAdapter(I2cBus* bus, const Options& options);

  bool Probe(Identity* out, std::string* error);
  bool ReadAll(Reading* out, std::string* error);

  bool burst_mode() const { return burst_mode_; }
  int address() const { return kAddressBase + options_.stack; }
  // Immutable after construction; safe to read from any thread.
  const Options& options() const { return options_; }

 private:
  bool EnsureOpen(std::string* error);
  bool ReadFloatBlock(int base, std::array<double, kChannelCount>* out);
  void ApplyValidation(Reading* out) const;

  I2cBus* bus_ = nullptr;
  Options options_;
  bool burst_mode_ = true;
  bool open_ = false;
};

}  // namespace coatheal
