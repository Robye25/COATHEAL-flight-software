#pragma once

#include <cstddef>
#include <string>

namespace coatheal {

// Opaque, version-independent owner for one libgpiod output line.
struct GpioOutput;

GpioOutput* RequestGpioOutput(const std::string& chip_path,
                              std::size_t offset,
                              const char* consumer,
                              bool initial_value);
bool SetGpioOutput(GpioOutput* output, bool value);
void ReleaseGpioOutput(GpioOutput* output);

bool ReadGpioInputOnce(const std::string& chip_path,
                       std::size_t offset,
                       const char* consumer,
                       bool* value);

}  // namespace coatheal
