# Sequent RTD HAT Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace both existing sample-temperature acquisition paths (`rtd_click_max31865` over SPI, `daq132m_modbus` over RS485) with a Sequent Microsystems 8-channel RTD HAT on I2C.

**Architecture:** A new `SequentRtdAdapter` owns the card's register conversation behind a read-only `I2cBus` transport seam, making it unit-testable with no hardware attached. `SensorManager` keeps only a polling worker plus its existing cache and health bookkeeping, matching the current `DpsLoop`/`AdsLoop` shape. Both legacy acquisition paths and their 23 config keys are deleted.

**Tech Stack:** C++17, CMake, plain `assert`-based unit tests with `int main()` (no gtest in this repo). Python 3 + `unittest` for the ground station. Linux `/dev/i2c-1` via `open`/`ioctl(I2C_SLAVE)`/`write`/`read`.

**Spec:** `docs/superpowers/specs/2026-08-17-sequent-rtd-migration-design.md`

## Global Constraints

- **Card addressing:** `address = 0x40 + stack`, `stack` in `[0, 7]`. Base constant `SLAVE_OWN_ADDRESS_BASE = 0x40`.
- **Channels are 1-indexed on the card.** Temperature at `kRtdVal1 + 4 * (channel - 1)`; resistance at `kRtdRes1 + 4 * (channel - 1)`.
- **Values are native-endian IEEE-754 float32, copied by `memcpy`, with no scaling divisor.** Both the Pi and the x86 dev host are little-endian.
- **Sensor type:** register `kPt1000` (offset 133), one byte, `0x0f & value`; `0` = PT100, `1` = PT1000. Card-wide, not per-channel.
- **Card type:** register `kCardType` (offset 99), one byte. `>= 1` means hardware >= 5.0 and software sensor-type selection is supported. `< 1` means sensor type cannot be verified.
- **The flight software never writes to the card.** No calibration writes, no sensor-type writes, no `I2C_MEM_WDT_*` access. This is enforced at the type level: `I2cBus` has no write method.
- **Existing platform guard:** `COATHEAL_HAS_LINUX_SENSOR_IO` is `1` when `__linux__ && __has_include(<linux/i2c-dev.h>)`, else `0`. New I2C code must compile on non-Linux hosts.
- **Test style:** no test framework. Free functions with `assert`, called from `int main()` returning `0`. Register each executable in `tests/CMakeLists.txt` with `add_executable` + `target_link_libraries(... PRIVATE coatheal_onboard_core)` + `add_test`.
- **Build:** `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.

---

### Task 1: Read-only I2C transport seam

**Files:**
- Create: `onboard/include/coatheal/hal/i2c_bus.hpp`
- Create: `onboard/src/hal/i2c_bus.cpp`
- Create: `tests/unit/fake_i2c_bus.hpp`
- Create: `tests/unit/test_i2c_bus.cpp`
- Modify: `onboard/CMakeLists.txt:28` (add source to `coatheal_onboard_core`)
- Modify: `tests/CMakeLists.txt` (append new executable)

**Interfaces:**
- Consumes: nothing.
- Produces: `coatheal::I2cBus` (abstract, methods `bool Open(int)`, `bool ReadRegisters(std::uint8_t, std::uint8_t*, std::size_t)`, `void Close()`, `bool available() const`); `coatheal::LinuxI2cBus`; test double `FakeI2cBus` with `void SetImage(std::vector<std::uint8_t>)`, `void FailNextReads(int)`, `void SetMaxReadLength(std::size_t)`, `int open_count() const`, `int address() const`.

**Design note for the implementer:** the interface deliberately has **no write method**. The design forbids all writes to this card (calibration and watchdog registers included), and omitting the method makes that unrepresentable rather than merely documented. Do not add one "for symmetry."

- [ ] **Step 1: Write the failing test**

Create `tests/unit/fake_i2c_bus.hpp`:

```cpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "coatheal/hal/i2c_bus.hpp"

namespace coatheal {

// Serves a canned register image so register conversations can be tested
// with no hardware. Mirrors the LinuxI2cBus contract: a read starting past
// the end of the image fails, and a read running off the end is short and
// therefore also fails.
class FakeI2cBus : public I2cBus {
 public:
  void SetImage(std::vector<std::uint8_t> image) { image_ = std::move(image); }
  void FailNextReads(int count) { fail_reads_ = count; }
  void SetMaxReadLength(std::size_t max_len) { max_read_len_ = max_len; }
  void SetOpenFails(bool value) { open_fails_ = value; }
  int open_count() const { return open_count_; }
  int address() const { return address_; }
  std::size_t last_read_length() const { return last_read_len_; }

  bool Open(int address) override {
    ++open_count_;
    address_ = address;
    return !open_fails_;
  }

  bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                     std::size_t size) override {
    last_read_len_ = size;
    if (fail_reads_ > 0) {
      --fail_reads_;
      return false;
    }
    if (size > max_read_len_) return false;
    if (static_cast<std::size_t>(reg) + size > image_.size()) return false;
    std::copy(image_.begin() + reg, image_.begin() + reg + size, data);
    return true;
  }

  void Close() override {}
  bool available() const override { return true; }

 private:
  std::vector<std::uint8_t> image_;
  int fail_reads_ = 0;
  std::size_t max_read_len_ = 32;
  bool open_fails_ = false;
  int open_count_ = 0;
  int address_ = -1;
  std::size_t last_read_len_ = 0;
};

}  // namespace coatheal
```

Create `tests/unit/test_i2c_bus.cpp`:

```cpp
// Contract tests for the I2C transport seam and its test double.

#include <cassert>
#include <cstdint>
#include <vector>

#include "coatheal/hal/i2c_bus.hpp"
#include "fake_i2c_bus.hpp"

using namespace coatheal;

namespace {

std::vector<std::uint8_t> Ramp(std::size_t size) {
  std::vector<std::uint8_t> image(size);
  for (std::size_t i = 0; i < size; ++i) {
    image[i] = static_cast<std::uint8_t>(i);
  }
  return image;
}

void TestFakeServesRegisterWindow() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  assert(bus.Open(0x40));
  assert(bus.address() == 0x40);

  std::uint8_t buf[4] = {0, 0, 0, 0};
  assert(bus.ReadRegisters(59, buf, 4));
  assert(buf[0] == 59 && buf[1] == 60 && buf[2] == 61 && buf[3] == 62);
}

void TestFakeRejectsReadPastEndOfImage() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(40));

  std::uint8_t buf[8] = {};
  assert(!bus.ReadRegisters(36, buf, 8));
}

void TestFakeHonoursMaxReadLength() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  bus.SetMaxReadLength(4);

  std::uint8_t big[32] = {};
  assert(!bus.ReadRegisters(0, big, 32));

  std::uint8_t small[4] = {};
  assert(bus.ReadRegisters(0, small, 4));
}

void TestFakeInjectsReadFailures() {
  FakeI2cBus bus;
  bus.SetImage(Ramp(140));
  bus.FailNextReads(2);

  std::uint8_t buf[1] = {};
  assert(!bus.ReadRegisters(0, buf, 1));
  assert(!bus.ReadRegisters(0, buf, 1));
  assert(bus.ReadRegisters(0, buf, 1));
}

void TestLinuxBusReportsAvailabilityWithoutCrashing() {
  // On a non-Linux build host available() is false and Open() must fail
  // cleanly rather than trap.
  LinuxI2cBus bus;
  if (!bus.available()) {
    assert(!bus.Open(0x40));
  }
  bus.Close();
}

}  // namespace

int main() {
  TestFakeServesRegisterWindow();
  TestFakeRejectsReadPastEndOfImage();
  TestFakeHonoursMaxReadLength();
  TestFakeInjectsReadFailures();
  TestLinuxBusReportsAvailabilityWithoutCrashing();
  return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build && cmake --build build --target coatheal_i2c_bus_tests`
Expected: FAIL — `coatheal/hal/i2c_bus.hpp: No such file or directory`, and CMake errors that target `coatheal_i2c_bus_tests` does not exist.

- [ ] **Step 3: Write the header**

Create `onboard/include/coatheal/hal/i2c_bus.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace coatheal {

// Read-only transport seam for byte-addressed I2C register devices.
//
// There is deliberately no write method. The Sequent RTD card's calibration
// and watchdog registers must never be written by flight software, and
// omitting writes from the seam makes that unrepresentable rather than
// merely documented.
class I2cBus {
 public:
  virtual ~I2cBus() = default;

  // Opens the bus and selects `address` as the active slave.
  virtual bool Open(int address) = 0;

  // Writes the register pointer, then reads `size` bytes into `data`.
  // Returns false on a short read.
  virtual bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                             std::size_t size) = 0;

  virtual void Close() = 0;

  // False on build hosts with no Linux I2C support, letting callers report
  // DISABLED rather than FAILED.
  virtual bool available() const = 0;
};

class LinuxI2cBus : public I2cBus {
 public:
  explicit LinuxI2cBus(const char* device = "/dev/i2c-1");
  ~LinuxI2cBus() override;

  LinuxI2cBus(const LinuxI2cBus&) = delete;
  LinuxI2cBus& operator=(const LinuxI2cBus&) = delete;

  bool Open(int address) override;
  bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                     std::size_t size) override;
  void Close() override;
  bool available() const override;

 private:
  const char* device_;
  int fd_ = -1;
};

}  // namespace coatheal
```

- [ ] **Step 4: Write the implementation**

Create `onboard/src/hal/i2c_bus.cpp`:

```cpp
#include "coatheal/hal/i2c_bus.hpp"

#if defined(__linux__) && __has_include(<linux/i2c-dev.h>)
#define COATHEAL_HAS_LINUX_I2C 1
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#define COATHEAL_HAS_LINUX_I2C 0
#endif

namespace coatheal {

LinuxI2cBus::LinuxI2cBus(const char* device) : device_(device) {}

LinuxI2cBus::~LinuxI2cBus() { Close(); }

bool LinuxI2cBus::available() const { return COATHEAL_HAS_LINUX_I2C != 0; }

bool LinuxI2cBus::Open(int address) {
#if COATHEAL_HAS_LINUX_I2C
  Close();
  fd_ = ::open(device_, O_RDWR);
  if (fd_ < 0) return false;
  if (::ioctl(fd_, I2C_SLAVE, address) < 0) {
    Close();
    return false;
  }
  return true;
#else
  (void)address;
  return false;
#endif
}

bool LinuxI2cBus::ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                                std::size_t size) {
#if COATHEAL_HAS_LINUX_I2C
  if (fd_ < 0 || data == nullptr) return false;
  if (::write(fd_, &reg, 1) != 1) return false;
  return ::read(fd_, data, size) == static_cast<ssize_t>(size);
#else
  (void)reg;
  (void)data;
  (void)size;
  return false;
#endif
}

void LinuxI2cBus::Close() {
#if COATHEAL_HAS_LINUX_I2C
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

}  // namespace coatheal
```

- [ ] **Step 5: Wire up the build**

In `onboard/CMakeLists.txt`, add `src/hal/i2c_bus.cpp` to the `coatheal_onboard_core` source list, immediately after `src/hal/i2c_adapter.cpp` (currently line 28).

Append to `tests/CMakeLists.txt`:

```cmake
# I2C transport seam contract tests.
add_executable(coatheal_i2c_bus_tests
  unit/test_i2c_bus.cpp
)

target_link_libraries(coatheal_i2c_bus_tests PRIVATE coatheal_onboard_core)

target_include_directories(coatheal_i2c_bus_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/unit
)

add_test(NAME coatheal_i2c_bus_tests COMMAND coatheal_i2c_bus_tests)
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build -R coatheal_i2c_bus_tests --output-on-failure`
Expected: PASS, 1 test.

- [ ] **Step 7: Commit**

```bash
git add onboard/include/coatheal/hal/i2c_bus.hpp onboard/src/hal/i2c_bus.cpp \
        tests/unit/fake_i2c_bus.hpp tests/unit/test_i2c_bus.cpp \
        onboard/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add read-only I2C transport seam"
```

---

### Task 2: Sequent RTD register map and Probe

**Files:**
- Create: `onboard/include/coatheal/hal/sequent_rtd_adapter.hpp`
- Create: `onboard/src/hal/sequent_rtd_adapter.cpp`
- Create: `tests/unit/test_sequent_rtd_adapter.cpp`
- Modify: `onboard/CMakeLists.txt` (add source)
- Modify: `tests/CMakeLists.txt` (append executable)

**Interfaces:**
- Consumes: `coatheal::I2cBus`, `coatheal::FakeI2cBus` from Task 1.
- Produces: `coatheal::SequentRtdAdapter` with nested `Options`, `Identity`, `Reading`; constructor `SequentRtdAdapter(I2cBus* bus, const Options& options)`; `bool Probe(Identity* out, std::string* error)`. Register constants in namespace `coatheal::sequent_rtd`.

**Register offsets are derived from the vendor enum, not measured.** Write them as a `constexpr` chain in the same derivation order upstream uses, so a firmware map change is a one-line edit. `kRtdRes1 = 59` is not 4-byte aligned; that is expected for byte-addressed memory.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_sequent_rtd_adapter.cpp`:

```cpp
// Sequent Microsystems RTD HAT register-conversation tests.
// Runs with no hardware attached via FakeI2cBus.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "coatheal/hal/sequent_rtd_adapter.hpp"
#include "fake_i2c_bus.hpp"

using namespace coatheal;

namespace {

// Builds a 140-byte register image with sane defaults.
std::vector<std::uint8_t> BlankImage() {
  std::vector<std::uint8_t> image(140, 0);
  image[sequent_rtd::kCardType] = 7;   // hardware >= 5.0
  image[sequent_rtd::kRevMajor] = 1;
  image[sequent_rtd::kRevMinor] = 5;
  image[sequent_rtd::kRevHwMajor] = 7;
  image[sequent_rtd::kRevHwMinor] = 0;
  image[sequent_rtd::kPt1000] = 0;     // PT100
  return image;
}

void PutFloat(std::vector<std::uint8_t>* image, int offset, float value) {
  std::memcpy(image->data() + offset, &value, sizeof(float));
}

void TestRegisterOffsetsMatchVendorDerivation() {
  assert(sequent_rtd::kRtdVal1 == 0);
  assert(sequent_rtd::kDiagTemp == 32);
  assert(sequent_rtd::kDiag5V == 33);
  assert(sequent_rtd::kRevHwMajor == 55);
  assert(sequent_rtd::kRevMajor == 57);
  assert(sequent_rtd::kRtdRes1 == 59);
  assert(sequent_rtd::kRtdReinit == 91);
  assert(sequent_rtd::kCardType == 99);
  assert(sequent_rtd::kPt1000 == 133);
}

void TestStackAddressArithmetic() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  SequentRtdAdapter::Options options;
  options.stack = 3;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(bus.address() == 0x43);
}

void TestStackOutOfRangeRejected() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  for (int stack : {-1, 8, 99}) {
    SequentRtdAdapter::Options options;
    options.stack = stack;
    SequentRtdAdapter adapter(&bus, options);

    SequentRtdAdapter::Identity id;
    std::string error;
    assert(!adapter.Probe(&id, &error));
    assert(error.find("STACK") != std::string::npos);
  }
}

void TestProbeReadsIdentity() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.card_type == 7);
  assert(id.fw_major == 1);
  assert(id.fw_minor == 5);
  assert(id.hw_major == 7);
  assert(!id.pt1000);
}

void TestProbeFailsWhenCardAbsent() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.FailNextReads(1);  // revision read fails, as doBoardInit detects

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("NOT_DETECTED") != std::string::npos);
}

void TestProbeFailsWhenOpenFails() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.SetOpenFails(true);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("BUS_OPEN") != std::string::npos);
}

void TestProbeRejectsSensorTypeMismatch() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 1;  // card set to PT1000

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = false;  // but we wired PT100 probes
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("SENSOR_TYPE_MISMATCH") != std::string::npos);
}

void TestProbeAcceptsMatchingPt1000() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 1;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = true;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.pt1000);
}

void TestSensorTypeMasksLowNibble() {
  // Vendor reads sensor type as `0x0f & buff`; high bits are not ours.
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kPt1000] = 0xF1;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter::Options options;
  options.expect_pt1000 = true;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Identity id;
  std::string error;
  assert(adapter.Probe(&id, &error));
  assert(id.pt1000);
}

void TestProbeReportsUnverifiableSensorTypeOnOldHardware() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kCardType] = 0;  // hardware < 5.0

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Identity id;
  std::string error;
  assert(!adapter.Probe(&id, &error));
  assert(error.find("SENSOR_TYPE_UNVERIFIABLE") != std::string::npos);
}

}  // namespace

int main() {
  TestRegisterOffsetsMatchVendorDerivation();
  TestStackAddressArithmetic();
  TestStackOutOfRangeRejected();
  TestProbeReadsIdentity();
  TestProbeFailsWhenCardAbsent();
  TestProbeFailsWhenOpenFails();
  TestProbeRejectsSensorTypeMismatch();
  TestProbeAcceptsMatchingPt1000();
  TestSensorTypeMasksLowNibble();
  TestProbeReportsUnverifiableSensorTypeOnOldHardware();
  return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build && cmake --build build --target coatheal_sequent_rtd_tests`
Expected: FAIL — `coatheal/hal/sequent_rtd_adapter.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `onboard/include/coatheal/hal/sequent_rtd_adapter.hpp`:

```cpp
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

// 8-channel PT100/PT1000 acquisition on a Sequent Microsystems stackable
// RTD HAT. Byte-addressed I2C memory at 0x40 + stack.
//
// Read-only by construction: the adapter holds an I2cBus, which has no
// write method, so calibration and watchdog registers cannot be touched.
class SequentRtdAdapter {
 public:
  static constexpr std::size_t kChannelCount = 8;
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

 private:
  bool EnsureOpen(std::string* error);
  bool ReadFloatBlock(int base, std::array<double, kChannelCount>* out);

  I2cBus* bus_ = nullptr;
  Options options_;
  bool burst_mode_ = true;
  bool open_ = false;
};

}  // namespace coatheal
```

- [ ] **Step 4: Implement Probe**

Create `onboard/src/hal/sequent_rtd_adapter.cpp` with the constructor, `EnsureOpen`, and `Probe`. Leave `ReadAll` and `ReadFloatBlock` for Task 3 — define them returning `false` with error `"NOT_IMPLEMENTED"` so the file links.

```cpp
#include "coatheal/hal/sequent_rtd_adapter.hpp"

#include <cstring>

namespace coatheal {

namespace {

void SetError(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
}

}  // namespace

SequentRtdAdapter::SequentRtdAdapter(I2cBus* bus, const Options& options)
    : bus_(bus), options_(options) {}

bool SequentRtdAdapter::EnsureOpen(std::string* error) {
  if (bus_ == nullptr) {
    SetError(error, "NO_BUS");
    return false;
  }
  if (options_.stack < kStackMin || options_.stack > kStackMax) {
    SetError(error, "STACK_OUT_OF_RANGE");
    return false;
  }
  if (open_) return true;
  if (!bus_->Open(address())) {
    SetError(error, "BUS_OPEN_FAILED");
    return false;
  }
  open_ = true;
  return true;
}

bool SequentRtdAdapter::Probe(Identity* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "NULL_OUT");
    return false;
  }
  if (!EnsureOpen(error)) return false;

  // Presence is proven exactly as the vendor's doBoardInit does: read the
  // firmware revision and require success.
  std::uint8_t rev[2] = {0, 0};
  if (!bus_->ReadRegisters(sequent_rtd::kRevMajor, rev, sizeof(rev))) {
    SetError(error, "CARD_NOT_DETECTED");
    open_ = false;
    return false;
  }
  out->fw_major = rev[0];
  out->fw_minor = rev[1];

  std::uint8_t hw[2] = {0, 0};
  if (!bus_->ReadRegisters(sequent_rtd::kRevHwMajor, hw, sizeof(hw))) {
    SetError(error, "HW_REVISION_READ_FAILED");
    return false;
  }
  out->hw_major = hw[0];
  out->hw_minor = hw[1];

  std::uint8_t card_type = 0;
  if (!bus_->ReadRegisters(sequent_rtd::kCardType, &card_type, 1)) {
    SetError(error, "CARD_TYPE_READ_FAILED");
    return false;
  }
  out->card_type = card_type;

  // Vendor gates sensor-type access on card type >= 1 ("Available only for
  // hardware version >= 5.0"). Below that we cannot confirm the card is
  // configured for the probes actually wired to it, so refuse rather than
  // read plausibly-wrong temperatures.
  if (card_type < 1) {
    SetError(error, "SENSOR_TYPE_UNVERIFIABLE_OLD_HARDWARE");
    return false;
  }

  std::uint8_t sensor = 0;
  if (!bus_->ReadRegisters(sequent_rtd::kPt1000, &sensor, 1)) {
    SetError(error, "SENSOR_TYPE_READ_FAILED");
    return false;
  }
  out->pt1000 = (sensor & 0x0FU) != 0U;

  if (out->pt1000 != options_.expect_pt1000) {
    SetError(error, "SENSOR_TYPE_MISMATCH");
    return false;
  }

  SetError(error, "");
  return true;
}

bool SequentRtdAdapter::ReadFloatBlock(int, std::array<double, kChannelCount>*) {
  return false;  // Task 3
}

bool SequentRtdAdapter::ReadAll(Reading*, std::string* error) {
  SetError(error, "NOT_IMPLEMENTED");
  return false;  // Task 3
}

}  // namespace coatheal
```

- [ ] **Step 5: Wire up the build**

Add `src/hal/sequent_rtd_adapter.cpp` to `coatheal_onboard_core` in `onboard/CMakeLists.txt`, after `src/hal/i2c_bus.cpp`.

Append to `tests/CMakeLists.txt`:

```cmake
# Sequent RTD HAT register-conversation tests. No hardware required.
add_executable(coatheal_sequent_rtd_tests
  unit/test_sequent_rtd_adapter.cpp
)

target_link_libraries(coatheal_sequent_rtd_tests PRIVATE coatheal_onboard_core)

target_include_directories(coatheal_sequent_rtd_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/unit
)

add_test(NAME coatheal_sequent_rtd_tests COMMAND coatheal_sequent_rtd_tests)
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R coatheal_sequent_rtd_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add onboard/include/coatheal/hal/sequent_rtd_adapter.hpp \
        onboard/src/hal/sequent_rtd_adapter.cpp \
        tests/unit/test_sequent_rtd_adapter.cpp \
        onboard/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add Sequent RTD register map and probe"
```

---

### Task 3: ReadAll — float32 decode, channel map, burst fallback

**Files:**
- Modify: `onboard/src/hal/sequent_rtd_adapter.cpp` (replace the Task 2 stubs)
- Modify: `tests/unit/test_sequent_rtd_adapter.cpp` (append tests + `main()` calls)

**Interfaces:**
- Consumes: `SequentRtdAdapter::Options`, `Reading`, register constants from Task 2.
- Produces: working `bool ReadAll(Reading*, std::string*)` populating `temperature_c`, `resistance_ohm`, `card_temp_c`, `rail_5v`, `adc_reinit_count`. `channel_valid` stays all-`true` until Task 4.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_sequent_rtd_adapter.cpp`, before the closing `}  // namespace`:

```cpp
std::vector<std::uint8_t> ImageWithChannels(const float* temps,
                                            const float* resistances) {
  std::vector<std::uint8_t> image = BlankImage();
  for (int i = 0; i < sequent_rtd::kChannels; ++i) {
    PutFloat(&image, sequent_rtd::kRtdVal1 + 4 * i, temps[i]);
    PutFloat(&image, sequent_rtd::kRtdRes1 + 4 * i, resistances[i]);
  }
  return image;
}

void TestReadAllDecodesFloat32Channels() {
  const float temps[8] = {0.0f, 10.5f, -40.25f, 85.0f,
                          21.0f, 22.0f, 23.0f, 24.0f};
  const float res[8] = {100.0f, 104.1f, 84.27f, 132.8f,
                        108.2f, 108.6f, 109.0f, 109.4f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  for (int i = 0; i < 8; ++i) {
    assert(std::fabs(reading.temperature_c[i] - temps[i]) < 1e-4);
    assert(std::fabs(reading.resistance_ohm[i] - res[i]) < 1e-4);
  }
}

void TestChannelMapRemapsLogicalSamples() {
  // Card channel 8 is dead; remap logical sample 0 onto card channel 3.
  const float temps[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  const float res[8] = {101.f, 102.f, 103.f, 104.f, 105.f, 106.f, 107.f, 108.f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter::Options options;
  options.channel_map = {3, 2, 1, 4, 5, 6, 7, 8};
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  // Logical 0 <- card channel 3 (1-indexed) == temps[2].
  assert(std::fabs(reading.temperature_c[0] - 3.0) < 1e-4);
  assert(std::fabs(reading.temperature_c[1] - 2.0) < 1e-4);
  assert(std::fabs(reading.temperature_c[2] - 1.0) < 1e-4);
  assert(std::fabs(reading.resistance_ohm[0] - 103.0) < 1e-4);
}

void TestFallbackMatchesBurstResults() {
  const float temps[8] = {0.0f, 10.5f, -40.25f, 85.0f,
                          21.0f, 22.0f, 23.0f, 24.0f};
  const float res[8] = {100.0f, 104.1f, 84.27f, 132.8f,
                        108.2f, 108.6f, 109.0f, 109.4f};
  const std::vector<std::uint8_t> image = ImageWithChannels(temps, res);

  FakeI2cBus burst_bus;
  burst_bus.SetImage(image);
  SequentRtdAdapter burst(&burst_bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading burst_reading;
  std::string error;
  assert(burst.ReadAll(&burst_reading, &error));
  assert(burst.burst_mode());

  FakeI2cBus slow_bus;
  slow_bus.SetImage(image);
  slow_bus.SetMaxReadLength(4);  // firmware refuses long reads
  SequentRtdAdapter slow(&slow_bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading slow_reading;
  assert(slow.ReadAll(&slow_reading, &error));
  assert(!slow.burst_mode());

  for (int i = 0; i < 8; ++i) {
    assert(std::fabs(burst_reading.temperature_c[i] -
                     slow_reading.temperature_c[i]) < 1e-9);
    assert(std::fabs(burst_reading.resistance_ohm[i] -
                     slow_reading.resistance_ohm[i]) < 1e-9);
  }
}

void TestFallbackLatchesOnce() {
  const float temps[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const float res[8] = {101, 102, 103, 104, 105, 106, 107, 108};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));
  bus.SetMaxReadLength(4);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;

  assert(adapter.ReadAll(&reading, &error));
  assert(!adapter.burst_mode());
  // Second poll must not re-attempt the 32-byte burst.
  assert(adapter.ReadAll(&reading, &error));
  assert(bus.last_read_length() <= 4);
}

void TestReadAllDecodesDiagnostics() {
  std::vector<std::uint8_t> image = BlankImage();
  image[sequent_rtd::kDiagTemp] = static_cast<std::uint8_t>(
      static_cast<std::int8_t>(-12));            // -12 degC
  image[sequent_rtd::kDiag5V] = 0x88;            // 5000 mV little-endian
  image[sequent_rtd::kDiag5V + 1] = 0x13;
  image[sequent_rtd::kRtdReinit] = 0x05;         // 5 re-inits
  image[sequent_rtd::kRtdReinit + 1] = 0x00;
  image[sequent_rtd::kRtdReinit + 2] = 0x00;
  image[sequent_rtd::kRtdReinit + 3] = 0x00;

  FakeI2cBus bus;
  bus.SetImage(image);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  assert(std::fabs(reading.card_temp_c - (-12.0)) < 1e-9);
  assert(std::fabs(reading.rail_5v - 5.0) < 1e-6);
  assert(reading.adc_reinit_count == 5U);
}

void TestReadAllFailsWhenBusFails() {
  FakeI2cBus bus;
  bus.SetImage(BlankImage());
  bus.FailNextReads(100);

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(!adapter.ReadAll(&reading, &error));
  assert(!error.empty());
}
```

Add `#include <cmath>` to the test includes, and add these calls to `main()`:

```cpp
  TestReadAllDecodesFloat32Channels();
  TestChannelMapRemapsLogicalSamples();
  TestFallbackMatchesBurstResults();
  TestFallbackLatchesOnce();
  TestReadAllDecodesDiagnostics();
  TestReadAllFailsWhenBusFails();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target coatheal_sequent_rtd_tests && ./build/tests/coatheal_sequent_rtd_tests`
Expected: FAIL — assertion in `TestReadAllDecodesFloat32Channels` because `ReadAll` returns `false` (`NOT_IMPLEMENTED`).

- [ ] **Step 3: Implement ReadFloatBlock and ReadAll**

Replace the two stubs in `onboard/src/hal/sequent_rtd_adapter.cpp`:

```cpp
namespace {

double FloatAt(const std::uint8_t* bytes) {
  float value = 0.0f;
  std::memcpy(&value, bytes, sizeof(float));
  return static_cast<double>(value);
}

}  // namespace

// Reads eight consecutive float32 values starting at `base`, then applies
// the 1-indexed channel map. Tries one 32-byte burst first for a
// time-coherent snapshot across channels; if the firmware refuses reads
// longer than four bytes, latches into per-channel mode permanently.
bool SequentRtdAdapter::ReadFloatBlock(int base,
                                       std::array<double, kChannelCount>* out) {
  std::uint8_t raw[kChannelCount * 4] = {};

  if (burst_mode_) {
    if (bus_->ReadRegisters(static_cast<std::uint8_t>(base), raw, sizeof(raw))) {
      for (std::size_t i = 0; i < kChannelCount; ++i) {
        const std::uint8_t channel = options_.channel_map[i];
        (*out)[i] = FloatAt(raw + 4 * (channel - 1));
      }
      return true;
    }
    burst_mode_ = false;
  }

  for (std::size_t i = 0; i < kChannelCount; ++i) {
    const std::uint8_t channel = options_.channel_map[i];
    std::uint8_t bytes[4] = {};
    const int offset = base + 4 * (static_cast<int>(channel) - 1);
    if (!bus_->ReadRegisters(static_cast<std::uint8_t>(offset), bytes,
                             sizeof(bytes))) {
      return false;
    }
    (*out)[i] = FloatAt(bytes);
  }
  return true;
}

bool SequentRtdAdapter::ReadAll(Reading* out, std::string* error) {
  if (out == nullptr) {
    SetError(error, "NULL_OUT");
    return false;
  }
  if (!EnsureOpen(error)) return false;

  if (!ReadFloatBlock(sequent_rtd::kRtdVal1, &out->temperature_c)) {
    SetError(error, "TEMPERATURE_READ_FAILED");
    open_ = false;
    return false;
  }
  if (!ReadFloatBlock(sequent_rtd::kRtdRes1, &out->resistance_ohm)) {
    SetError(error, "RESISTANCE_READ_FAILED");
    open_ = false;
    return false;
  }

  // Diagnostics only. Byte interpretations are inferred from the register
  // map; a wrong guess degrades a log line, never a control value.
  std::uint8_t diag[3] = {};
  if (bus_->ReadRegisters(sequent_rtd::kDiagTemp, diag, sizeof(diag))) {
    out->card_temp_c = static_cast<double>(static_cast<std::int8_t>(diag[0]));
    const std::uint16_t millivolts =
        static_cast<std::uint16_t>(diag[1]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(diag[2]) << 8U);
    out->rail_5v = static_cast<double>(millivolts) / 1000.0;
  }

  std::uint8_t reinit[4] = {};
  if (bus_->ReadRegisters(sequent_rtd::kRtdReinit, reinit, sizeof(reinit))) {
    out->adc_reinit_count = static_cast<std::uint32_t>(reinit[0]) |
                            (static_cast<std::uint32_t>(reinit[1]) << 8U) |
                            (static_cast<std::uint32_t>(reinit[2]) << 16U) |
                            (static_cast<std::uint32_t>(reinit[3]) << 24U);
  }

  out->channel_valid.fill(true);  // narrowed in Task 4
  SetError(error, "");
  return true;
}
```

Add `#include <cmath>` and `#include <cstdint>` to the implementation file if not already present.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R coatheal_sequent_rtd_tests --output-on-failure`
Expected: PASS, all 16 test functions.

- [ ] **Step 5: Commit**

```bash
git add onboard/src/hal/sequent_rtd_adapter.cpp tests/unit/test_sequent_rtd_adapter.cpp
git commit -m "feat: decode Sequent RTD channels with burst fallback"
```

---

### Task 4: Per-channel validation

**Files:**
- Modify: `onboard/src/hal/sequent_rtd_adapter.cpp`
- Modify: `onboard/include/coatheal/hal/sequent_rtd_adapter.hpp` (declare the helper)
- Modify: `tests/unit/test_sequent_rtd_adapter.cpp`

**Interfaces:**
- Consumes: `Reading::temperature_c`, `Reading::resistance_ohm`, `Options::resistance_min_ohm/max_ohm/crosscheck_tol_c`.
- Produces: populated `Reading::channel_valid`; free function `bool coatheal::Pt100TemperatureFromOhms(double ohms, double* temp_c)` in the adapter's header, extracted so the adapter does not depend on `SensorManager`.

**Important:** the existing cross-check helper lives on `SensorManager` as `SensorManager::Pt100TemperatureFromResistance`. The HAL must not depend on `SensorManager`. Move the implementation into `sequent_rtd_adapter.cpp` as a free function `Pt100TemperatureFromOhms`, and in Task 6 make `SensorManager::Pt100TemperatureFromResistance` forward to it so existing tests at `tests/unit/test_sensor_manager_rev_c.cpp` keep passing unchanged.

The existing implementation returns `false` outside its valid range — preserve that. It is verified by these existing assertions, which must continue to hold: `R=100.0 -> 0 degC (+/-0.05)`, `R=138.5055 -> 100 degC (+/-0.1)`, `R=80.306 -> -50 degC (+/-0.2)`, `R=1000.0 -> false`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_sequent_rtd_adapter.cpp`:

```cpp
void TestNanAndInfChannelsMarkedInvalid() {
  float temps[8] = {20, 20, 20, 20, 20, 20, 20, 20};
  float res[8] = {107.79f, 107.79f, 107.79f, 107.79f,
                  107.79f, 107.79f, 107.79f, 107.79f};
  temps[2] = std::numeric_limits<float>::quiet_NaN();
  res[5] = std::numeric_limits<float>::infinity();

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  assert(reading.channel_valid[0]);
  assert(!reading.channel_valid[2]);
  assert(!reading.channel_valid[5]);
}

void TestOpenAndShortSensorsMarkedInvalid() {
  // Channel 1 open (resistance far high), channel 4 shorted (near zero).
  float temps[8] = {20, 20, 20, 20, 20, 20, 20, 20};
  float res[8] = {107.79f, 5000.0f, 107.79f, 107.79f,
                  0.2f, 107.79f, 107.79f, 107.79f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter adapter(&bus, SequentRtdAdapter::Options{});
  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  assert(reading.channel_valid[0]);
  assert(!reading.channel_valid[1]);
  assert(!reading.channel_valid[4]);
}

void TestCrosscheckMismatchMarkedInvalid() {
  // Channel 3's reported temperature disagrees with its own resistance:
  // 107.79 ohm is ~20 degC, but the card claims 60 degC.
  float temps[8] = {20, 20, 20, 60.0f, 20, 20, 20, 20};
  float res[8] = {107.79f, 107.79f, 107.79f, 107.79f,
                  107.79f, 107.79f, 107.79f, 107.79f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter::Options options;
  options.crosscheck_tol_c = 2.0;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));

  assert(reading.channel_valid[0]);
  assert(!reading.channel_valid[3]);
}

void TestCrosscheckToleranceIsRespected() {
  // 107.79 ohm is ~20 degC; a 1.5 degC disagreement is inside a 2.0 tol.
  float temps[8] = {21.5f, 20, 20, 20, 20, 20, 20, 20};
  float res[8] = {107.79f, 107.79f, 107.79f, 107.79f,
                  107.79f, 107.79f, 107.79f, 107.79f};

  FakeI2cBus bus;
  bus.SetImage(ImageWithChannels(temps, res));

  SequentRtdAdapter::Options options;
  options.crosscheck_tol_c = 2.0;
  SequentRtdAdapter adapter(&bus, options);

  SequentRtdAdapter::Reading reading;
  std::string error;
  assert(adapter.ReadAll(&reading, &error));
  assert(reading.channel_valid[0]);
}

void TestPt100ConversionMatchesLegacyBehaviour() {
  double temp = 0.0;
  assert(Pt100TemperatureFromOhms(100.0, &temp));
  assert(std::fabs(temp) < 0.05);
  assert(Pt100TemperatureFromOhms(138.5055, &temp));
  assert(std::fabs(temp - 100.0) < 0.1);
  assert(Pt100TemperatureFromOhms(80.306, &temp));
  assert(std::fabs(temp - (-50.0)) < 0.2);
  assert(!Pt100TemperatureFromOhms(1000.0, &temp));
}
```

Add `#include <limits>` to the test includes, and register the five new functions in `main()`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target coatheal_sequent_rtd_tests`
Expected: FAIL to compile — `Pt100TemperatureFromOhms` is not declared.

- [ ] **Step 3: Declare the helper in the header**

Add to `onboard/include/coatheal/hal/sequent_rtd_adapter.hpp`, inside `namespace coatheal` and before the class:

```cpp
// Callendar-Van Dusen inverse for PT100. Returns false outside the
// supported resistance range. Extracted here so the HAL does not depend
// on SensorManager; SensorManager::Pt100TemperatureFromResistance
// forwards to this.
bool Pt100TemperatureFromOhms(double resistance_ohm, double* temperature_c);
```

Add to the private section of the class:

```cpp
  void ApplyValidation(Reading* out) const;
```

- [ ] **Step 4: Implement**

Move the body of `SensorManager::Pt100TemperatureFromResistance` from `onboard/src/sensor_manager.cpp` into `onboard/src/hal/sequent_rtd_adapter.cpp` as `Pt100TemperatureFromOhms`, preserving its logic and range check exactly. Then add:

```cpp
void SequentRtdAdapter::ApplyValidation(Reading* out) const {
  for (std::size_t i = 0; i < kChannelCount; ++i) {
    const double temp = out->temperature_c[i];
    const double ohms = out->resistance_ohm[i];

    if (!std::isfinite(temp) || !std::isfinite(ohms)) {
      out->channel_valid[i] = false;
      continue;
    }
    if (ohms < options_.resistance_min_ohm ||
        ohms > options_.resistance_max_ohm) {
      out->channel_valid[i] = false;
      continue;
    }
    double derived = 0.0;
    if (!Pt100TemperatureFromOhms(ohms, &derived) ||
        std::fabs(derived - temp) > options_.crosscheck_tol_c) {
      out->channel_valid[i] = false;
      continue;
    }
    out->channel_valid[i] = true;
  }
}
```

In `ReadAll`, replace `out->channel_valid.fill(true);` with `ApplyValidation(out);`.

Add `#include <cmath>` if not already present.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R coatheal_sequent_rtd_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add onboard/include/coatheal/hal/sequent_rtd_adapter.hpp \
        onboard/src/hal/sequent_rtd_adapter.cpp \
        tests/unit/test_sequent_rtd_adapter.cpp
git commit -m "feat: validate Sequent RTD channels against raw resistance"
```

---

### Task 5: Add the new config keys

**Files:**
- Modify: `onboard/include/coatheal/config.hpp:116-160` (`SensorHardwareConfig`)
- Modify: `onboard/src/config.cpp` (parse block near `:374`, validation near `:658-720`)
- Modify: `tests/unit/test_suite.cpp` (config assertions near `:382`, `:474`)

**Interfaces:**
- Consumes: nothing.
- Produces: `SensorHardwareConfig` fields `sequent_rtd_stack` (`int`), `sequent_rtd_channels` (`std::vector<std::size_t>`), `sequent_rtd_poll_ms` (`int`), `sequent_rtd_expect_sensor_type` (`std::string`), `sequent_rtd_resistance_min_ohm` (`double`), `sequent_rtd_resistance_max_ohm` (`double`), `sequent_rtd_crosscheck_tol_c` (`double`).

This task is **additive only** — the legacy keys stay so the tree keeps compiling. They are removed in Task 8.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_suite.cpp` a new test function and register it in `main()`:

```cpp
void TestSequentRtdConfigDefaultsAndParsing() {
  OnboardConfig defaults;
  assert(defaults.sensors.sequent_rtd_stack == 0);
  assert(defaults.sensors.sequent_rtd_poll_ms == 1000);
  assert(defaults.sensors.sequent_rtd_expect_sensor_type == "pt100");
  assert(defaults.sensors.sequent_rtd_channels.size() == 8);
  assert(defaults.sensors.sequent_rtd_channels[0] == 1);
  assert(defaults.sensors.sequent_rtd_channels[7] == 8);

  const std::string path = WriteTempConfig(
      "sensor.sequent_rtd_stack=2\n"
      "sensor.sequent_rtd_channels=3,2,1,4,5,6,7,8\n"
      "sensor.sequent_rtd_poll_ms=500\n"
      "sensor.sequent_rtd_expect_sensor_type=pt1000\n"
      "sensor.sequent_rtd_resistance_min_ohm=70.0\n"
      "sensor.sequent_rtd_resistance_max_ohm=380.0\n"
      "sensor.sequent_rtd_crosscheck_tol_c=1.5\n");

  OnboardConfig cfg;
  std::string error;
  assert(LoadConfigFromIni(path, &cfg, &error));
  assert(cfg.sensors.sequent_rtd_stack == 2);
  assert(cfg.sensors.sequent_rtd_channels[0] == 3);
  assert(cfg.sensors.sequent_rtd_poll_ms == 500);
  assert(cfg.sensors.sequent_rtd_expect_sensor_type == "pt1000");
  assert(std::fabs(cfg.sensors.sequent_rtd_crosscheck_tol_c - 1.5) < 1e-9);
}

void TestSequentRtdConfigRejectsBadValues() {
  struct Case { const char* body; const char* fragment; };
  const Case cases[] = {
    {"sensor.sequent_rtd_stack=8\n", "sequent_rtd_stack"},
    {"sensor.sequent_rtd_channels=1,2,3\n", "sequent_rtd_channels"},
    {"sensor.sequent_rtd_channels=1,1,3,4,5,6,7,8\n", "sequent_rtd_channels"},
    {"sensor.sequent_rtd_channels=0,2,3,4,5,6,7,8\n", "sequent_rtd_channels"},
    {"sensor.sequent_rtd_channels=9,2,3,4,5,6,7,8\n", "sequent_rtd_channels"},
    {"sensor.sequent_rtd_expect_sensor_type=pt500\n", "expect_sensor_type"},
    {"sensor.sequent_rtd_resistance_min_ohm=400.0\n", "resistance"},
  };
  for (const Case& c : cases) {
    const std::string path = WriteTempConfig(c.body);
    OnboardConfig cfg;
    std::string error;
    assert(!LoadConfigFromIni(path, &cfg, &error));
    assert(error.find(c.fragment) != std::string::npos);
  }
}
```

`WriteTempConfig(body)` must write a **complete valid** config — reuse the existing helper in `test_suite.cpp` that emits the baseline INI (the one producing `hardware.sample_count=8` at `:380`) and append `body` to it. If that helper is not already factored out, extract it in this step.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target coatheal_unit_tests`
Expected: FAIL to compile — `sequent_rtd_stack` is not a member of `SensorHardwareConfig`.

- [ ] **Step 3: Add the config fields**

In `onboard/include/coatheal/config.hpp`, inside `SensorHardwareConfig`, after the `rtd_click_*` block (line 149):

```cpp
  // Sequent Microsystems 8-channel RTD HAT. Sole sample-temperature source.
  int sequent_rtd_stack = 0;                     // 0..7 -> I2C 0x40..0x47
  std::vector<std::size_t> sequent_rtd_channels; // card channel per sample
  int sequent_rtd_poll_ms = 1000;
  std::string sequent_rtd_expect_sensor_type = "pt100";
  double sequent_rtd_resistance_min_ohm = 60.0;
  double sequent_rtd_resistance_max_ohm = 390.0;
  double sequent_rtd_crosscheck_tol_c = 2.0;
```

In `onboard/src/config.cpp`, in the `OnboardConfig::OnboardConfig()` body near line 78:

```cpp
  sensors.sequent_rtd_channels = {1, 2, 3, 4, 5, 6, 7, 8};
```

- [ ] **Step 4: Add the parse cases**

In `onboard/src/config.cpp`, in the key dispatch chain immediately after the `sensor.daq132m_enabled_channels` case (line 374):

```cpp
    } else if (key == "sensor.sequent_rtd_stack") {
      if (!parse_int(key, value, &config->sensors.sequent_rtd_stack, line_no)) return false;
    } else if (key == "sensor.sequent_rtd_channels") {
      if (!ParseSizeList(value, &config->sensors.sequent_rtd_channels)) {
        if (error != nullptr) *error = "invalid sensor.sequent_rtd_channels";
        return false;
      }
    } else if (key == "sensor.sequent_rtd_poll_ms") {
      if (!parse_int(key, value, &config->sensors.sequent_rtd_poll_ms, line_no)) return false;
    } else if (key == "sensor.sequent_rtd_expect_sensor_type") {
      config->sensors.sequent_rtd_expect_sensor_type = value;
    } else if (key == "sensor.sequent_rtd_resistance_min_ohm") {
      if (!parse_double(key, value, &config->sensors.sequent_rtd_resistance_min_ohm, line_no)) return false;
    } else if (key == "sensor.sequent_rtd_resistance_max_ohm") {
      if (!parse_double(key, value, &config->sensors.sequent_rtd_resistance_max_ohm, line_no)) return false;
    } else if (key == "sensor.sequent_rtd_crosscheck_tol_c") {
      if (!parse_double(key, value, &config->sensors.sequent_rtd_crosscheck_tol_c, line_no)) return false;
```

- [ ] **Step 5: Add the validation rules**

In `onboard/src/config.cpp`, in the post-parse validation block (after line 720):

```cpp
  if (config->sensors.sequent_rtd_stack < 0 ||
      config->sensors.sequent_rtd_stack > 7) {
    if (error != nullptr) {
      *error = "sensor.sequent_rtd_stack must be 0..7 (I2C 0x40..0x47)";
    }
    return false;
  }
  if (config->sensors.sequent_rtd_channels.size() !=
      config->hardware.sample_count) {
    if (error != nullptr) {
      *error = "sensor.sequent_rtd_channels must have hardware.sample_count "
               "entries";
    }
    return false;
  }
  {
    std::set<std::size_t> seen;
    for (const std::size_t channel : config->sensors.sequent_rtd_channels) {
      if (channel < 1 || channel > 8) {
        if (error != nullptr) {
          *error = "sensor.sequent_rtd_channels entries must be 1..8";
        }
        return false;
      }
      if (!seen.insert(channel).second) {
        if (error != nullptr) {
          *error = "sensor.sequent_rtd_channels contains duplicates";
        }
        return false;
      }
    }
  }
  if (config->sensors.sequent_rtd_expect_sensor_type != "pt100" &&
      config->sensors.sequent_rtd_expect_sensor_type != "pt1000") {
    if (error != nullptr) {
      *error = "sensor.sequent_rtd_expect_sensor_type must be pt100 or pt1000";
    }
    return false;
  }
  if (config->sensors.sequent_rtd_resistance_min_ohm >=
      config->sensors.sequent_rtd_resistance_max_ohm) {
    if (error != nullptr) {
      *error = "sensor.sequent_rtd_resistance_min_ohm must be below "
               "sensor.sequent_rtd_resistance_max_ohm";
    }
    return false;
  }
```

Add `#include <set>` to `config.cpp` if not already present.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R coatheal_unit_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add onboard/include/coatheal/config.hpp onboard/src/config.cpp tests/unit/test_suite.cpp
git commit -m "feat: add Sequent RTD configuration keys"
```

---

### Task 6: SensorManager integration and validity policy

**Files:**
- Modify: `onboard/include/coatheal/sensor_manager.hpp`
- Modify: `onboard/src/sensor_manager.cpp`
- Modify: `tests/unit/test_sensor_manager_rev_c.cpp`

**Interfaces:**
- Consumes: `SequentRtdAdapter`, `LinuxI2cBus`, `Pt100TemperatureFromOhms`, config fields from Task 5.
- Produces: `SensorManager::SequentRtdLoop()`; `SensorManager::rtd_ok()`; `SensorManager::Pt100TemperatureFromResistance` retained as a forwarding wrapper. Removes `Max31865CodeToResistance`, `ReadRtdClickMax31865`, `RtdClickLoop`, `AppendRtdClickDiagnostics`, `ReadDaq132m`, `DaqLoop`.

**Behaviour change to be explicit about:** `sample_temp_ok_` currently uses `std::any_of` over all cached samples (`sensor_manager.cpp`, both loops). It becomes: **true only when every channel referenced by `config_.heaters.temperature_channels` is valid and not stale.** This is strictly stricter. Samples 6 and 7 are pulled but unheated, so their loss must not gate the thermal path.

**Also note:** `RtdClickLoop` currently sleeps on `config_.sensors.daq132m_poll_ms` — a latent bug, since RTD Click had no poll key. The replacement uses `config_.sensors.sequent_rtd_poll_ms`.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_sensor_manager_rev_c.cpp`:

```cpp
void TestPt100WrapperStillForwards() {
  double temp = 0.0;
  assert(SensorManager::Pt100TemperatureFromResistance(100.0, &temp));
  assert(std::fabs(temp) < 0.05);
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
}
```

Register all three in `main()`. Delete the existing `TestMax31865Pt100Conversion` call and its `Max31865CodeToResistance` assertion (the first two lines of that function), keeping its `Pt100TemperatureFromResistance` assertions — they now live in `TestPt100WrapperStillForwards`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target coatheal_sensor_manager_rev_c_tests`
Expected: FAIL to compile — `HeatedChannelsValid` is not a member of `SensorManager`.

- [ ] **Step 3: Update the header**

In `onboard/include/coatheal/sensor_manager.hpp`:

- Replace `#include "coatheal/hal/spi_adapter.hpp"` usage for RTD purposes by adding `#include "coatheal/hal/i2c_bus.hpp"` and `#include "coatheal/hal/sequent_rtd_adapter.hpp"`. Keep the `SpiAdapter` parameter in the constructor — the steppers still use it.
- Delete declarations: `ReadDaq132m`, `ReadRtdClickMax31865`, `DaqLoop`, `RtdClickLoop`, `AppendRtdClickDiagnostics`, `Max31865CodeToResistance`, and the `RtdClickDiagnostics` struct plus the `rtd_diag_` member.
- Delete members `daq_thread_`, `daq_health_`, `daq_io_mu_`, `resolved_daq_device_`.
- **Leave `rs485_ok_` and `rs485_ok()` in place for now.** They are consumed by `system_controller.cpp:639` and `:1005` and serialised into the STATUS wire field; removing them here breaks the build. Task 7 removes them together with their wire field. Pin the assignment at `sensor_manager.cpp:185` to `rs485_ok_ = false;` in the meantime.
- Add:

```cpp
  static bool HeatedChannelsValid(const OnboardConfig& config,
                                  const std::vector<bool>& channel_valid);

 private:
  void SequentRtdLoop();

  LinuxI2cBus rtd_bus_;
  SequentRtdAdapter rtd_;
  SequentRtdAdapter::Identity rtd_identity_;
  bool rtd_probed_ = false;
  std::thread rtd_thread_;
  ComponentHealth rtd_health_;
  mutable std::mutex rtd_io_mu_;
```

The existing public accessors (`t_ambient_ok`, `p_ambient_ok`, `resistance_ok`, `i2c_ok`, `rs485_ok`, `sample_temp_ok`, `uv_ok`) keep their current signatures; `ComponentSummary` and `ActiveCheck` keep theirs too. Only their bodies change.

- [ ] **Step 4: Delete the legacy implementations**

In `onboard/src/sensor_manager.cpp`, delete the bodies of `ReadDaq132m` (113 lines), `ReadRtdClickMax31865` (143), `DaqLoop` (71), `RtdClickLoop` (42), `AppendRtdClickDiagnostics` (20), and `Max31865CodeToResistance` (4). Also delete `ModbusCrc`, `BaudConstant`, `DiscoverSerialDevice`, and the `COATHEAL_HAS_MAX31865_IO` guard block plus its `<linux/spi/spidev.h>` and `<termios.h>` includes — all become unreferenced.

Replace `SensorManager::Pt100TemperatureFromResistance` with a forwarding wrapper:

```cpp
bool SensorManager::Pt100TemperatureFromResistance(double resistance_ohm,
                                                   double* temperature_c) {
  return Pt100TemperatureFromOhms(resistance_ohm, temperature_c);
}
```

- [ ] **Step 5: Implement the policy helper and the worker**

Add to `onboard/src/sensor_manager.cpp`:

```cpp
// sample_temp_ok_ gates the thermal path, so it tracks only the channels a
// heater actually controls. Samples 6 and 7 are pulled but unheated: losing
// one is a data-quality event, not a safety event. Fails closed on a
// mapping that points past the end of the sample vector.
bool SensorManager::HeatedChannelsValid(
    const OnboardConfig& config, const std::vector<bool>& channel_valid) {
  if (config.heaters.temperature_channels.empty()) return false;
  for (const std::size_t channel : config.heaters.temperature_channels) {
    if (channel >= channel_valid.size()) return false;
    if (!channel_valid[channel]) return false;
  }
  return true;
}

void SensorManager::SequentRtdLoop() {
  while (running_.load()) {
    SequentRtdAdapter::Reading reading;
    std::string error = "NO_RESPONSE";
    bool ok = false;
    {
      std::lock_guard<std::mutex> io_lock(rtd_io_mu_);
      if (!rtd_probed_) {
        rtd_probed_ = rtd_.Probe(&rtd_identity_, &error);
      }
      if (rtd_probed_) {
        ok = rtd_.ReadAll(&reading, &error);
        if (!ok) rtd_probed_ = false;  // re-probe on the next pass
      }
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(cache_mu_);
      std::vector<bool> channel_valid(sample_cache_.size(), false);
      std::size_t valid_count = 0;

      if (ok) {
        for (std::size_t i = 0;
             i < sample_cache_.size() &&
             i < SequentRtdAdapter::kChannelCount; ++i) {
          if (reading.channel_valid[i]) {
            sample_cache_[i] = {reading.temperature_c[i], true, true, now};
            sample_resistance_ohm_[i] = reading.resistance_ohm[i];
            channel_valid[i] = true;
            ++valid_count;
          } else {
            sample_cache_[i].valid = false;
          }
        }
        rtd_health_.state = valid_count == sample_cache_.size()
                                ? ComponentState::kOk
                                : ComponentState::kDegraded;
        rtd_health_.error = valid_count == 0 ? "NO_VALID_CHANNELS"
                                             : (valid_count < sample_cache_.size()
                                                    ? "PARTIAL_CHANNELS"
                                                    : "NONE");
        rtd_health_.last_success_age_ms = 0;
      } else {
        bool any_previous = false;
        std::chrono::steady_clock::time_point newest{};
        for (auto& sample : sample_cache_) {
          sample.valid = false;
          if (sample.has_value &&
              (!any_previous || sample.last_success > newest)) {
            newest = sample.last_success;
            any_previous = true;
          }
        }
        rtd_health_.state = FailedState(any_previous, newest);
        rtd_health_.error = error.empty() ? "NO_RESPONSE" : error;
        rtd_health_.last_success_age_ms = AgeMs(newest, any_previous);
      }

      // Staleness applies on top of per-channel validity.
      for (std::size_t i = 0; i < sample_cache_.size(); ++i) {
        const std::int64_t age =
            AgeMs(sample_cache_[i].last_success, sample_cache_[i].has_value);
        if (age < 0 || age >= config_.sensors.stale_after_ms) {
          channel_valid[i] = false;
        }
      }

      i2c_ok_ = ok;
      resistance_ok_ = ok;
      sample_temp_ok_ = HeatedChannelsValid(config_, channel_valid);
    }
    if (WaitForPoll(config_.sensors.sequent_rtd_poll_ms)) break;
  }
}
```

In the constructor initialiser list, build the adapter options from config:

```cpp
      rtd_bus_(),
      rtd_(&rtd_bus_, MakeSequentOptions(config)),
```

with a file-local helper:

```cpp
namespace {

SequentRtdAdapter::Options MakeSequentOptions(const OnboardConfig& config) {
  SequentRtdAdapter::Options options;
  options.stack = config.sensors.sequent_rtd_stack;
  options.expect_pt1000 =
      config.sensors.sequent_rtd_expect_sensor_type == "pt1000";
  options.resistance_min_ohm = config.sensors.sequent_rtd_resistance_min_ohm;
  options.resistance_max_ohm = config.sensors.sequent_rtd_resistance_max_ohm;
  options.crosscheck_tol_c = config.sensors.sequent_rtd_crosscheck_tol_c;
  for (std::size_t i = 0;
       i < options.channel_map.size() &&
       i < config.sensors.sequent_rtd_channels.size(); ++i) {
    options.channel_map[i] =
        static_cast<std::uint8_t>(config.sensors.sequent_rtd_channels[i]);
  }
  return options;
}

}  // namespace
```

In `Start()`, replace the `daq132m_enabled` and `rtd_click_enabled` thread launches with:

```cpp
  rtd_thread_ = std::thread(&SensorManager::SequentRtdLoop, this);
```

In `Stop()`, join `rtd_thread_` and drop the `daq_thread_` join. In `ComponentSummary()` and `ActiveCheck()`, replace the RTD Click and DAQ sections with one Sequent section reporting `rtd_identity_` (card type, firmware, hardware revision, sensor type), `rtd_.address()`, `rtd_.burst_mode()`, and the last `card_temp_c` / `rail_5v` / `adc_reinit_count`.

- [ ] **Step 6: Run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS across all registered tests.

- [ ] **Step 7: Commit**

```bash
git add onboard/include/coatheal/sensor_manager.hpp onboard/src/sensor_manager.cpp \
        tests/unit/test_sensor_manager_rev_c.cpp
git commit -m "feat: acquire sample temperatures from the Sequent RTD card"
```

---

### Task 7: Telemetry struct and wire protocol

**Files:**
- Modify: `onboard/include/coatheal/telemetry.hpp:31-38`
- Modify: `onboard/src/telemetry.cpp:109-112`
- Modify: `onboard/include/coatheal/status_flags.hpp:18`
- Modify: `onboard/src/status_flags.cpp:19`
- Modify: `onboard/src/system_controller.cpp:639, 1005`
- Modify: `onboard/src/sensor_manager.cpp:185, 936`
- Modify: `tests/unit/test_telemetry_rev_c.cpp:42`
- Modify: `docs/protocol.md`

**Interfaces:**
- Consumes: `SensorSnapshot` from Task 6.
- Produces: `SensorSnapshot::sequent_rtd` (`ComponentHealth`) replacing `daq132m` and `rtd_click`; wire token `SEQUENT_RTD` replacing `DAQ132M` and `RTD_CLICK`; `StatusFlags` without `rs485_ok`.

**This is a breaking wire change on two fields, not one.** Onboard and ground station must deploy together.

**Second wire change, not identified in the spec.** `StatusFlags::rs485_ok` (`status_flags.hpp:18`, commented "DAQ132M Modbus RTU path healthy") is serialised into the STATUS field as `RS485_OK`/`RS485_FAIL` at `status_flags.cpp:19`, populated from `SensorManager::rs485_ok()` at `system_controller.cpp:639` and `:1005`, and displayed by the ground station. The RS485 hardware leaves with the DAQ-132M, so the flag becomes permanently meaningless. It is removed here rather than pinned to a constant, because a status flag that is always `OK` is worse than no flag — it reads as a working check. `SensorManager` lines 185 and 936 also assign `rs485_ok_` and must be cleaned up.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_telemetry_rev_c.cpp`:

```cpp
void TestComponentStateUsesSequentRtdToken() {
  TelemetryRecord record;
  record.sensors.sample_temps_c.assign(8, 20.0);
  record.sensors.sample_temp_valid.assign(8, true);
  record.sensors.sample_temp_age_ms.assign(8, 0);
  record.sensors.sample_resistance_ohm.assign(8, 107.79);
  record.sensors.dps310.state = ComponentState::kOk;
  record.sensors.ads1115.state = ComponentState::kOk;
  record.sensors.sequent_rtd.state = ComponentState::kDegraded;
  record.heater_duty.assign(6, 0.0);
  record.steppers.resize(2);

  const std::string frame = SerializeTelemetryDataFrame(record, "sess-1");
  assert(frame.find("SEQUENT_RTD:DEGRADED") != std::string::npos);
  assert(frame.find("RTD_CLICK") == std::string::npos);
  assert(frame.find("DAQ132M") == std::string::npos);
}

void TestStatusFlagsDropRs485() {
  // RS485 hardware leaves with the DAQ-132M. A flag that can only ever
  // read OK is worse than no flag, so it must be gone from the wire.
  StatusFlags flags;
  const std::string encoded = SerializeStatusFlags(flags);
  assert(encoded.find("RS485") == std::string::npos);
}
```

Register both in `main()`. Delete the `r.status.rs485_ok = true;` assignment at `tests/unit/test_telemetry_rev_c.cpp:42`.

If the status-flag serializer has a different free-function name than `SerializeStatusFlags`, use the name declared in `onboard/include/coatheal/status_flags.hpp` — the assertion is what matters.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target coatheal_telemetry_rev_c_tests`
Expected: FAIL to compile — `sequent_rtd` is not a member of `SensorSnapshot`.

- [ ] **Step 3: Update the struct**

In `onboard/include/coatheal/telemetry.hpp`, replace lines 33-34:

```cpp
  ComponentHealth daq132m;
  ComponentHealth rtd_click;
```

with:

```cpp
  ComponentHealth sequent_rtd;
```

Update the comment on `sample_resistance_ohm` (lines 36-37) — resistance is now genuinely measured:

```cpp
  // Per-channel raw RTD resistance from the Sequent card, used for
  // open/short detection and the temperature cross-check.
  std::vector<double> sample_resistance_ohm;
```

- [ ] **Step 4: Update the serializer**

In `onboard/src/telemetry.cpp`, replace lines 111-112:

```cpp
      << "|DAQ132M:" << ToString(record.sensors.daq132m.state)
      << "|RTD_CLICK:" << ToString(record.sensors.rtd_click.state)
```

with:

```cpp
      << "|SEQUENT_RTD:" << ToString(record.sensors.sequent_rtd.state)
```

Then fix the assignments in `sensor_manager.cpp`'s `ReadSnapshot` to populate `snapshot.sequent_rtd` from `rtd_health_`.

- [ ] **Step 5: Remove the RS485 status flag**

Delete `bool rs485_ok = true;` from `StatusFlags` (`status_flags.hpp:18`) and its serialisation term at `status_flags.cpp:19`, including the trailing `'|'` separator so the encoding stays well-formed. Delete `record.status.rs485_ok = ...` at `system_controller.cpp:639` and the `";rs485_ok="` term at `:1005`. In `sensor_manager.cpp`, delete the `rs485_ok_` assignment at line 185 and remove `rs485_ok_` from the combined assignment at line 936 (`i2c_ok_ = rs485_ok_ = sample_temp_ok_ = uv_ok_ = true;` becomes `i2c_ok_ = sample_temp_ok_ = uv_ok_ = true;`).

Also remove the `self._row("rs485", "RS-485")` line from `ground-station/app/gui/panels_info.py:118` — it is covered here rather than in Task 9 because it is part of the same wire change.

- [ ] **Step 6: Update the protocol doc**

In `docs/protocol.md`, replace the `DAQ132M` and `RTD_CLICK` entries in the `COMPONENT_STATE` field description with a single `SEQUENT_RTD`, delete the `RS485_OK`/`RS485_FAIL` entry from the STATUS field description, and add a note that both are breaking changes requiring simultaneous deployment of onboard and ground station.

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add onboard/include/coatheal/telemetry.hpp onboard/src/telemetry.cpp \
        onboard/include/coatheal/status_flags.hpp onboard/src/status_flags.cpp \
        onboard/src/system_controller.cpp onboard/src/sensor_manager.cpp \
        ground-station/app/gui/panels_info.py \
        tests/unit/test_telemetry_rev_c.cpp docs/protocol.md
git commit -m "feat!: replace DAQ132M/RTD_CLICK with SEQUENT_RTD and drop RS485 flag"
```

---

### Task 8: Remove the 23 legacy config keys

**Files:**
- Modify: `onboard/include/coatheal/config.hpp:116-160`
- Modify: `onboard/src/config.cpp`
- Modify: `config/onboard.example.ini:63-92`
- Modify: `config/onboard.debug.ini:75-104`
- Modify: `tests/unit/test_suite.cpp:382, 474`

**Interfaces:**
- Consumes: Task 6 must have removed all uses of these fields.
- Produces: `SensorHardwareConfig` free of `rtd_click_*`, `daq132m_*`, and `sample_temperature_source`.

- [ ] **Step 1: Write the failing test**

In `tests/unit/test_suite.cpp`, add:

```cpp
void TestLegacySensorKeysAreRejected() {
  const char* legacy[] = {
    "sensor.sample_temperature_source=rtd_click_max31865\n",
    "sensor.rtd_click_enabled=true\n",
    "sensor.rtd_click_spi_device=/dev/spidev0.0\n",
    "sensor.daq132m_enabled=true\n",
    "sensor.daq132m_device=/dev/ttyUSB0\n",
  };
  for (const char* body : legacy) {
    const std::string path = WriteTempConfig(body);
    OnboardConfig cfg;
    std::string error;
    // Unknown keys must be rejected loudly, not silently ignored, so a
    // stale deployed INI cannot boot with the operator believing it applied.
    assert(!LoadConfigFromIni(path, &cfg, &error));
  }
}
```

Register it in `main()`. Delete the existing assertion at `:474` (`cfg.sensors.sample_temperature_source == "rtd_click_max31865"`) and the `daq132m` assertions that follow it, plus the emitted legacy keys at `:382-384`.

**Verified:** `LoadConfigFromIni` already rejects unknown keys at `onboard/src/config.cpp:519-522` ("Reject unknown keys so final-BOM configuration drift fails at startup"), so deleting the parse branches is sufficient to make this test pass. No extra rejection list is needed.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target coatheal_unit_tests && ./build/tests/coatheal_unit_tests`
Expected: FAIL — legacy keys still parse successfully.

- [ ] **Step 3: Delete the fields**

In `onboard/include/coatheal/config.hpp`, delete from `SensorHardwareConfig`: `daq132m_enabled`, `daq132m_auto_discover`, `daq132m_poll_ms`, `sample_temperature_source`, `daq132m_device`, `daq132m_baud`, `daq132m_parity`, `daq132m_data_bits`, `daq132m_stop_bits`, `daq132m_slave_id`, `daq132m_function_code`, `daq132m_register_base`, `daq132m_register_count`, `daq132m_c_per_count`, `daq132m_c_offset`, `daq132m_enabled_channels`, `rtd_click_enabled`, `rtd_click_spi_device`, `rtd_click_cs_line`, `rtd_click_drdy_line`, `rtd_click_wires`, `rtd_click_sample_channel`, `rtd_click_reference_ohm`, `rtd_click_filter_hz`, `rtd_click_spi_speed_hz`.

Change `resistance_source` default from `"disabled"` to `"sequent_rtd"`.

- [ ] **Step 4: Delete the parse and validation cases**

In `onboard/src/config.cpp`: delete the corresponding `else if` branches, the `sensors.daq132m_enabled_channels` default at line 78, and the validation blocks at lines 659-661 (`sample_source_ok`), 709-715 (rtd_click), and 716-722 (daq132m). Update the `resistance_source` validation to accept `"sequent_rtd"`.

- [ ] **Step 5: Update the INI files**

In `config/onboard.example.ini`, delete lines 63-92 and replace with:

```ini
sensor.stale_after_ms=3000

# Sequent Microsystems 8-channel RTD HAT (I2C 0x40 + stack).
sensor.sequent_rtd_stack=0
sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0
sensor.resistance_source=sequent_rtd
```

Apply the equivalent edit to `config/onboard.debug.ini` lines 75-104, keeping that file's existing `stale_after_ms` value.

- [ ] **Step 6: Run the full suite**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add onboard/include/coatheal/config.hpp onboard/src/config.cpp \
        config/onboard.example.ini config/onboard.debug.ini tests/unit/test_suite.cpp
git commit -m "refactor!: remove RTD Click and DAQ-132M configuration keys"
```

---

### Task 9: Ground station token rename

**Files:**
- Modify: `ground-station/app/gui/panels_info.py:123, 220`
- Modify: `ground-station/tests/test_protocol.py:58-70, 171-192`
- Modify: `ground-station/tests/test_gui_smoke.py:59`

**Interfaces:**
- Consumes: the `SEQUENT_RTD` wire token from Task 7.
- Produces: GUI rows and tests keyed on `SEQUENT_RTD`.

`ground-station/app/protocol.py` parses `COMPONENT_STATE` generically (splitting on `|` then `:` at lines 204-210) and needs **no change** — only the hardcoded display lists do.

- [ ] **Step 1: Update the tests first**

In `ground-station/tests/test_protocol.py`, replace both occurrences of
`"DAQ132M:<state>|RTD_CLICK:<state>"` in the sample frames (lines 58-59 and 69-70) with a single `"SEQUENT_RTD:<state>"` token, update the assertions at lines 171-172 and 192 to read `pkt.component_state["SEQUENT_RTD"]`, and delete the now-meaningless `component_state["DAQ132M"]` assertion. Rename `RTD_CLICK_S1_DATA` to `SEQUENT_RTD_DATA` and `test_parse_single_rtd_click_pt100_on_s1` to `test_parse_sequent_rtd_component_state`.

In `ground-station/tests/test_gui_smoke.py:59`, change `self.assertIn("RTD_CLICK", label_texts)` to `self.assertIn("SEQUENT_RTD", label_texts)`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd ground-station && python -m unittest discover -s tests -v`
Expected: FAIL — `KeyError: 'SEQUENT_RTD'` and the GUI label assertion fails.

- [ ] **Step 3: Update the GUI**

In `ground-station/app/gui/panels_info.py`, at both line 123 and line 220, change:

```python
            "DPS310", "ADS1115", "DAQ132M", "RTD_CLICK", "MOTOR0", "MOTOR1", "PWM"
```

to:

```python
            "DPS310", "ADS1115", "SEQUENT_RTD", "MOTOR0", "MOTOR1", "PWM"
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd ground-station && python -m unittest discover -s tests -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ground-station/app/gui/panels_info.py ground-station/tests/test_protocol.py \
        ground-station/tests/test_gui_smoke.py
git commit -m "feat!: display SEQUENT_RTD component state on the ground station"
```

---

### Task 10: Setup and probe scripts

**Files:**
- Modify: `scripts/hardware_setup.py:47-51, 188-198, 363-367`
- Modify: `scripts/spi_probe.py`
- Modify: `ground-station/tests/test_hardware_setup.py:40-46, 84-117`

**Interfaces:**
- Consumes: config key names from Tasks 5 and 8.
- Produces: `migrate_config` that drops the 23 retired keys and injects the 7 new defaults; an I2C presence check replacing the SPI RTD probe.

- [ ] **Step 1: Update the tests first**

In `ground-station/tests/test_hardware_setup.py`:

- Replace `test_validate_candidate_detects_rtd_drdy_conflict` (lines 40-46) with a test that `sensor.sequent_rtd_stack=8` is rejected and that a duplicate entry in `sensor.sequent_rtd_channels` is rejected.
- In `test_migrate_config_removes_stale_keys_and_forces_rtd_tmc2240` (lines 84-117), replace the legacy input keys with the same legacy keys as *input* and assert they are **absent** from the migrated output, and that `sensor.sequent_rtd_stack` is present with value `"0"` and `sensor.sequent_rtd_channels` with `"1,2,3,4,5,6,7,8"`.

```python
    def test_migrate_config_drops_retired_sensor_keys(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "old.ini"
            source.write_text(
                "sensor.sample_temperature_source=rtd_click_max31865\n"
                "sensor.rtd_click_enabled=true\n"
                "sensor.rtd_click_drdy_line=25\n"
                "sensor.daq132m_enabled=false\n"
                "sensor.daq132m_device=/dev/ttyUSB0\n",
                encoding="utf-8",
            )
            values = migrated_values(source)
            for retired in (
                "sensor.sample_temperature_source",
                "sensor.rtd_click_enabled",
                "sensor.rtd_click_drdy_line",
                "sensor.daq132m_enabled",
                "sensor.daq132m_device",
            ):
                self.assertNotIn(retired, values)
            self.assertEqual(values["sensor.sequent_rtd_stack"], "0")
            self.assertEqual(
                values["sensor.sequent_rtd_channels"], "1,2,3,4,5,6,7,8")
```

Use the existing helper in that file for reading migrated values; if none exists, add `migrated_values(source)` wrapping the module's existing `_candidate_from_existing` + `_load_config` pair.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd ground-station && python -m unittest tests.test_hardware_setup -v`
Expected: FAIL — `KeyError: 'sensor.sequent_rtd_stack'`.

- [ ] **Step 3: Update hardware_setup.py defaults**

Replace lines 49-51:

```python
    "sensor.sample_temperature_source": "rtd_click_max31865",
    "sensor.daq132m_enabled": "false",
    "sensor.rtd_click_enabled": "true",
```

with:

```python
    "sensor.sequent_rtd_stack": "0",
    "sensor.sequent_rtd_channels": "1,2,3,4,5,6,7,8",
    "sensor.sequent_rtd_poll_ms": "1000",
    "sensor.sequent_rtd_expect_sensor_type": "pt100",
    "sensor.sequent_rtd_resistance_min_ohm": "60.0",
    "sensor.sequent_rtd_resistance_max_ohm": "390.0",
    "sensor.sequent_rtd_crosscheck_tol_c": "2.0",
    "sensor.resistance_source": "sequent_rtd",
```

Add a module-level retired-key set and filter it in `_candidate_from_existing`:

```python
RETIRED_SENSOR_KEYS = frozenset({
    "sensor.sample_temperature_source",
    "sensor.daq132m_enabled", "sensor.daq132m_auto_discover",
    "sensor.daq132m_poll_ms", "sensor.daq132m_device", "sensor.daq132m_baud",
    "sensor.daq132m_parity", "sensor.daq132m_data_bits",
    "sensor.daq132m_stop_bits", "sensor.daq132m_slave_id",
    "sensor.daq132m_function_code", "sensor.daq132m_register_base",
    "sensor.daq132m_register_count", "sensor.daq132m_c_per_count",
    "sensor.daq132m_c_offset", "sensor.daq132m_enabled_channels",
    "sensor.rtd_click_enabled", "sensor.rtd_click_spi_device",
    "sensor.rtd_click_cs_line", "sensor.rtd_click_drdy_line",
    "sensor.rtd_click_wires", "sensor.rtd_click_sample_channel",
    "sensor.rtd_click_reference_ohm", "sensor.rtd_click_filter_hz",
    "sensor.rtd_click_spi_speed_hz",
})
```

- [ ] **Step 4: Update validation**

Replace the `sample_temperature_source` checks at lines 190-198 with:

```python
    stack = values.get("sensor.sequent_rtd_stack")
    try:
        stack_int = int(stack)
    except (TypeError, ValueError):
        errors.append("sensor.sequent_rtd_stack must be an integer 0..7")
    else:
        if not 0 <= stack_int <= 7:
            errors.append("sensor.sequent_rtd_stack must be 0..7")

    raw_channels = values.get("sensor.sequent_rtd_channels", "")
    channels = [c.strip() for c in raw_channels.split(",") if c.strip()]
    if len(channels) != 8:
        errors.append("sensor.sequent_rtd_channels must list 8 channels")
    elif len(set(channels)) != len(channels):
        errors.append("sensor.sequent_rtd_channels contains duplicates")
    elif any(not c.isdigit() or not 1 <= int(c) <= 8 for c in channels):
        errors.append("sensor.sequent_rtd_channels entries must be 1..8")

    if values.get("sensor.sequent_rtd_expect_sensor_type") not in {
            "pt100", "pt1000"}:
        errors.append("sensor.sequent_rtd_expect_sensor_type must be "
                      "pt100 or pt1000")
```

Remove `sensor.rtd_click_drdy_line` and `sensor.rtd_click_spi_device` from the reported key list at lines 363-367 and add the new `sensor.sequent_rtd_*` keys.

- [ ] **Step 5: Update spi_probe.py**

Delete the MAX31865 probing path. Add an I2C presence check that reads one byte at offset 57 (`kRevMajor`) from `0x40 + stack` and prints the firmware revision, mirroring `doBoardInit`:

```python
def probe_sequent_rtd(stack: int = 0) -> int:
    """Read firmware revision from the Sequent RTD card, as doBoardInit does."""
    address = 0x40 + stack
    try:
        with open("/dev/i2c-1", "r+b", buffering=0) as bus:
            fcntl.ioctl(bus, I2C_SLAVE, address)
            bus.write(bytes([57]))          # REVISION_MAJOR_MEM_ADD
            data = bus.read(2)
    except OSError as exc:
        print(f"    sequent rtd stack={stack} addr=0x{address:02x}: {exc}")
        return 1
    print(f"    sequent rtd stack={stack} addr=0x{address:02x} "
          f"fw={data[0]}.{data[1]}")
    return 0
```

Add `import fcntl` and `I2C_SLAVE = 0x0703` at module scope. Keep the TMC2240 SPI probe untouched — the steppers still use SPI.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd ground-station && python -m unittest discover -s tests -v`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add scripts/hardware_setup.py scripts/spi_probe.py \
        ground-station/tests/test_hardware_setup.py
git commit -m "feat: migrate setup and probe scripts to the Sequent RTD card"
```

---

### Task 11: Documentation and bench bring-up gate

**Files:**
- Delete: `docs/rev-c-rtd-click-plug-and-play.md`
- Create: `docs/sequent-rtd-bring-up.md`
- Modify: `docs/hardware.md:107-134`, `docs/configuration.md:63-67`, `docs/architecture.md`, `docs/onboard.md:196`, `docs/ground-station.md`, `docs/manual-operations.md`, `docs/component-configuration-and-bring-up.md:143-185`, `docs/rev-c-installation-and-hardware-setup.md:382-386`, `docs/rev-c-instruction-manual.md`, `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the bring-up procedure that gates trusting the derived register map.

- [ ] **Step 1: Write the bring-up guide**

Create `docs/sequent-rtd-bring-up.md` covering, in order:

1. **DIP switch stack addressing** — ID0/ID1/ID2 select level 0-7; address is `0x40 + stack`. Confirm with `i2cdetect -y 1`.
2. **Register map verification (blocking gate).** Before trusting any reading, dump bytes 0-104 with a known precision 100 Ω resistor on card channel 1 and confirm: a float32 near `100.0` at offset 59, a float32 near `0.0` at offset 0, plausible firmware bytes at 57-58, and `card_type >= 1` at offset 99. Record the observed `card_type` value in this document. **Do not proceed if the offsets disagree** — the constants in `sequent_rtd_adapter.hpp` are derived from vendor source, not measured.
3. **Burst-read confirmation.** Read 32 bytes at offset 0 in one transaction and confirm it matches eight consecutive 4-byte reads. If it does not, the adapter falls back automatically; record that `burst_mode` reports `false` in `ComponentSummary`.
4. **Diagnostic byte confirmation.** Check that offset 32 reads a plausible die temperature and offsets 33-34 a value near 5000 mV. These are diagnostics only; a wrong interpretation degrades a log line, never a control value.
5. **Sensor type check** — confirm offset 133 masks to `0` for PT100 and that `sensor.sequent_rtd_expect_sensor_type` matches.
6. **Calibration** — the bench-only vendor CLI procedure: `rtd <stack> cal <channel> 0` with the input shorted, then `rtd <stack> cal <channel> 100` with a precision 100 Ω resistor; `rtd <stack> calrst <channel>` to restore factory values. Note that flight software never writes these registers.
7. **Freed pins** — GPIO 16 and GPIO 25 are released by the RTD Click removal and are unassigned; SPI is now used only by the TMC2240 steppers.

- [ ] **Step 2: Update the remaining docs**

Replace every `sensor.sample_temperature_source`, `rtd_click_*`, and `daq132m_*` reference with the Sequent equivalents. Specifically:

- `docs/hardware.md:107-134` — replace both INI examples with the single Sequent block; update the sensor table.
- `docs/configuration.md:65-67` — delete the `sample_temperature_source` row; add rows for the seven new keys.
- `docs/onboard.md:196` — `Ina3221Adapter` row now reads "Retired stub; addresses 0x40/0x41 are reserved by the Sequent RTD card and must not be re-enabled without re-addressing."
- `docs/component-configuration-and-bring-up.md:143-185` — replace both source variants with one Sequent section linking to `sequent-rtd-bring-up.md`.
- `docs/rev-c-installation-and-hardware-setup.md:382-386` and `docs/rev-c-instruction-manual.md` — update the config excerpts and the wiring description.
- `docs/architecture.md`, `docs/ground-station.md`, `docs/manual-operations.md`, `README.md` — update component names and the `SEQUENT_RTD` token.

Delete `docs/rev-c-rtd-click-plug-and-play.md` and replace every inbound link to it with `docs/sequent-rtd-bring-up.md`.

- [ ] **Step 3: Verify no stale references remain**

Run: `grep -rin "rtd_click\|max31865\|daq132m\|sample_temperature_source" --include=*.md --include=*.ini --include=*.py --include=*.cpp --include=*.hpp . | grep -v docs/superpowers/`
Expected: no output.

- [ ] **Step 4: Run the full suite one final time**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure && cd ground-station && python -m unittest discover -s tests`
Expected: PASS on both.

- [ ] **Step 5: Commit**

```bash
git add docs README.md
git rm docs/rev-c-rtd-click-plug-and-play.md
git commit -m "docs: replace RTD Click guidance with Sequent RTD bring-up"
```

---

## Deviations from the spec

Recorded here so review can accept or reject them explicitly:

1. **Spec §9 lists "Probe refusing an unexpected card type" as a test.** The vendor source exposes no enumeration of valid `RTD_CARD_TYPE` values, only a `card < 1` gate meaning "hardware >= 5.0 required." Inventing a magic constant would be worse than not gating. Task 2 therefore gates on `card_type >= 1` (which is what upstream actually checks) and records the observed value; Task 11 step 2 captures the real value at bench so a tighter gate can be added later if wanted.
2. **`Pt100TemperatureFromResistance` moves to the HAL** as the free function `Pt100TemperatureFromOhms`, with `SensorManager::Pt100TemperatureFromResistance` kept as a forwarding wrapper. The spec says the helper is "retained"; it is, but it had to move so the adapter does not depend on `SensorManager`.
3. **Diagnostic byte interpretations** (`kDiagTemp` as `int8` °C, `kDiag5V` as `uint16` mV) are inferred from the register map's implied widths, not from vendor code. They are diagnostics-only by design, so a wrong guess cannot affect control. Task 11 step 4 confirms them.
4. **The spec missed a second wire-protocol change.** `StatusFlags::rs485_ok` is emitted as `RS485_OK`/`RS485_FAIL` in the STATUS field and is explicitly the DAQ-132M Modbus health flag (`status_flags.hpp:18`). Deleting the DAQ-132M leaves it permanently meaningless, so Task 7 removes it from the wire as well. The spec's §7 describes only the `COMPONENT_STATE` change; the deploy-together constraint it states covers this too, but the field list was incomplete.
5. **Spec §9 lists `test_safety_rev_c.cpp` among the files needing updates for the new validity policy.** It does not reference `sample_temp_ok`, `rs485_ok`, or `resistance_ok`, so no change is required there. The validity-policy tests live in `test_sensor_manager_rev_c.cpp` (Task 6) instead.
