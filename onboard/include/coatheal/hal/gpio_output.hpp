#pragma once

#include <cstddef>
#include <string>

namespace coatheal {

// Opaque, version-independent owner for one libgpiod output line.
struct GpioOutput;

// Internal pull requested alongside an output line. Schematic v4 fits no
// external pull resistors to the heater inputs or the TMC5160 CS/EN lines,
// so the only thing that defines a line's level while nobody drives it
// (service stopped, crashed, or restarting) is the SoC's pull register.
// That register survives a libgpiod release -- the bcm2835 pinctrl driver
// only reverts the function to input -- so a pull requested here keeps the
// line at its safe level until the next power cycle. The power-on window
// before the service ever ran is covered by the config.txt `gpio=` block
// deploy_onboard.sh installs (docs/hardware.md, "Boot-time GPIO states").
enum class GpioBias {
  kAsIs,      // leave the pull register alone (pre-2026-09 behaviour)
  kPullUp,
  kPullDown,
};

// Requests `offset` as an output driving `initial_value`. A bias other than
// kAsIs is best effort: a kernel or libgpiod without bias support gets the
// line claimed without it (a warning is logged once per call) rather than
// no line at all, because losing the pull is a boot-window nuisance while
// losing the output is a heater or motor that cannot be commanded.
GpioOutput* RequestGpioOutput(const std::string& chip_path,
                              std::size_t offset,
                              const char* consumer,
                              bool initial_value,
                              GpioBias bias = GpioBias::kAsIs);
bool SetGpioOutput(GpioOutput* output, bool value);
void ReleaseGpioOutput(GpioOutput* output);

bool ReadGpioInputOnce(const std::string& chip_path,
                       std::size_t offset,
                       const char* consumer,
                       bool* value);

}  // namespace coatheal
