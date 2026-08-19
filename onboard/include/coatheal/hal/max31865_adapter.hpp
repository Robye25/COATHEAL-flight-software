#pragma once

#include <cstdint>
#include <string>

#include "coatheal/hal/spi_bus.hpp"

namespace coatheal {

// MAX31865 RTD-to-digital converter driver for the two MikroE RTD Click
// boards (schematic v3: SPI0 CE1=SAMPLE1, CE0=SAMPLE2), each measuring one
// coating specimen's resistance via a 4-wire Kelvin connection. Clean-room
// driver over the SpiBus seam -- no vendor library.
//
// The specimen's resistance range is UNTESTED. This adapter's whole job is
// to characterise that range safely: saturation and faults are first-class
// outputs, never averaged, clamped, or otherwise turned into a
// plausible-looking number. See ReadOneShot().
//
// Register map (MAX31865 datasheet): config 0x00 (write = addr|0x80), RTD
// MSB/LSB 0x01/0x02 (one 3-byte auto-increment read), fault status 0x07.
// Config bits: VBIAS bit7, conversion-mode-auto bit6 (unused here -- this
// adapter only ever runs one-shot conversions), 1SHOT bit5
// (self-clearing), 3WIRE bit4 (always 0 below: schematic v3 wires these
// boards 4-wire Kelvin), FAULTCLR bit1, 50HZ bit0 (always 1 below: reject
// 50 Hz mains noise).
class Max31865Adapter {
 public:
  struct Options {
    std::string spi_device;
    double reference_ohm = 470.0;
    std::uint32_t spi_speed_hz = 500000;
    // One-shot timing floors. Datasheet minimums: settle >=10 ms after
    // VBIAS goes high before starting a conversion; wait >=65 ms for the
    // conversion itself to finish. Both are plain std::this_thread sleeps
    // (this adapter runs on a worker thread, not a control loop), and both
    // are overridable so unit tests don't burn 75+ ms of real wall time
    // per ReadOneShot() call -- production call sites must leave these at
    // the datasheet-minimum defaults below.
    int settle_ms = 10;
    int conversion_ms = 65;
  };

  struct Reading {
    // Computed from the raw code unconditionally, even when saturated or
    // faulted -- diagnostic value only. NEVER trust this unless `valid`
    // is true: the whole point of this adapter is that an out-of-range
    // specimen must characterise as invalid, not as a plausible ohm value.
    double resistance_ohm = 0.0;
    bool valid = false;
    bool out_of_range = false;
    // Raw MAX31865 fault status register (0x07) contents, captured only
    // when `out_of_range` is true; 0 otherwise (including when the fault
    // register read itself failed -- diagnostics-only, never gates
    // `out_of_range`).
    std::uint8_t fault_bits = 0;
  };

  // `bus` is not owned: the caller (production: one LinuxSpiBus per click;
  // tests: a FakeSpiBus) keeps it alive for the adapter's lifetime and is
  // responsible for its storage.
  Max31865Adapter(SpiBus* bus, const Options& options);

  // Config-register write + readback, confirming the click answers on the
  // bus. Writes a deliberately benign value (VBIAS=0, 1SHOT=0, 50HZ=1):
  // nothing in it can self-clear (1SHOT) or drift out from under a plain
  // equality check (VBIAS), so a mismatched readback is unambiguously the
  // click disagreeing, not a probe artifact.
  //
  // ReadOneShot() performs this same check itself before every conversion
  // (see below), so production code does not have to remember to call
  // this. It stays public as the standalone presence question.
  //
  // Error on a mismatched readback: "CLICK_NOT_DETECTED" -- distinct from
  // every transport error string ("NO_BUS", "BUS_OPEN_FAILED",
  // "CONFIG_WRITE_FAILED", "CONFIG_READ_FAILED"), because a click that is
  // absent and a bus that is broken need different responses on the
  // ground.
  bool Probe(std::string* error);

  // Full one-shot conversion, in this exact order: PRESENCE CHECK (the
  // Probe() write+readback above) -> VBIAS on -> settle -> 1SHOT -> wait
  // -> read RTD MSB/LSB (0x01/0x02) -> VBIAS off (limits specimen
  // self-heating). Fault bit set (RTD LSB bit0) OR code >=
  // kNearFullScaleCode -> `out->valid=false, out->out_of_range=true`; the
  // fault status register (0x07) is then read and reported in
  // `out->fault_bits`, and FAULTCLR is issued. `out->resistance_ohm` is
  // always computed from the raw code for diagnostics, but must never be
  // trusted when `out->valid` is false -- that is the safety property this
  // whole adapter exists to provide.
  //
  // On a transport failure after VBIAS has been turned on (the 1SHOT write
  // or the RTD read), the adapter makes a best-effort attempt to write
  // VBIAS off before returning false: bias left on with no conversion in
  // progress just self-heats the specimen for no reason, and specimen
  // safety takes priority over a clean error return. A failure on the
  // VBIAS-on write itself needs no such recovery -- the write never
  // reached the chip, so its bias state is whatever it already was (this
  // adapter always leaves VBIAS off at the end of every successful cycle).
  //
  // The leading presence check is load-bearing and is the reason this
  // returns false at all against unpopulated hardware. SPI has no
  // acknowledgement: an ABSENT click does not fail a transfer, it simply
  // leaves MISO at its idle level (0x00 floating/pulled low, 0xFF pulled
  // high), so without the check every step below "succeeds", the RTD code
  // reads 0x0000, the fault bit is clear, and this function reports a
  // healthy 0-ohm measurement of a click that is not there. Callers key
  // bus health off this return value, so that silently reported
  // RESISTANCE_OK with every channel blank. Errors are distinct:
  // "CLICK_NOT_DETECTED" for absent/not answering, transport strings for
  // a genuine I/O failure.
  //
  // A PRESENT click reading a saturated or out-of-range specimen is a
  // different thing entirely and still returns TRUE -- bus healthy,
  // channel invalid (`out->valid=false, out->out_of_range=true`). Health
  // and channel validity are separate outputs on purpose; do not merge
  // them.
  bool ReadOneShot(Reading* out, std::string* error);

  // R = code * reference_ohm / 32768 (code: the RTD register's 15-bit
  // value, MAX31865 datasheet). Pure conversion -- no threshold or fault
  // logic; ReadOneShot() applies those separately after calling this.
  static double CodeToOhms(std::uint16_t code, double reference_ohm);

  // Near-full-scale threshold in the RTD ADC's 15-bit code space
  // (0..32767): a code this close to the top rail cannot be trusted as a
  // real resistance measurement even without the fault bit set.
  static constexpr std::uint16_t kNearFullScaleCode = 32760;

 private:
  bool EnsureOpen(std::string* error);
  // Config write + readback, shared by Probe() and ReadOneShot(). Assumes
  // the bus is already open.
  bool VerifyPresence(std::string* error);
  bool WriteConfig(std::uint8_t value);
  bool ReadRegister(std::uint8_t addr, std::uint8_t* value);
  bool ReadRtdCode(std::uint8_t* msb, std::uint8_t* lsb);

  SpiBus* bus_;
  Options options_;
  bool open_ = false;
};

}  // namespace coatheal
