#pragma once

#include <mutex>
#include <string>

namespace coatheal {

// Process-wide SPI bus lock. The Rev C hardware uses software chip-selects on
// shared SPI0 devices, so unrelated drivers must serialize transfers.
std::mutex& SpiBusMutex(const std::string& spi_device);

}  // namespace coatheal
