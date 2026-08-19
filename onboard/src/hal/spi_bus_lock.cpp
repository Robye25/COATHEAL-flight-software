#include "coatheal/hal/spi_bus_lock.hpp"

#include <atomic>
#include <cctype>
#include <map>

namespace coatheal {

namespace internal {

struct SpiControllerLock {
  std::mutex mu;
  std::atomic<std::uint64_t> acquisitions{0};
};

}  // namespace internal

namespace {

internal::SpiControllerLock& Controller(const std::string& spi_device) {
  static std::mutex registry_mu;
  static std::map<std::string, internal::SpiControllerLock> controllers;
  const std::string key = SpiControllerKey(spi_device);
  std::lock_guard<std::mutex> guard(registry_mu);
  return controllers[key];
}

}  // namespace

std::string SpiControllerKey(const std::string& spi_device) {
  // "/dev/spidev<bus>.<chipselect>" -> "spi<bus>". The chip-select suffix
  // is deliberately discarded: it selects which chip answers, not which
  // wires are used, and the wires are what the mutex protects.
  static const std::string kMarker = "spidev";
  const std::size_t marker = spi_device.rfind(kMarker);
  if (marker != std::string::npos) {
    const std::size_t bus_begin = marker + kMarker.size();
    std::size_t cursor = bus_begin;
    while (cursor < spi_device.size() &&
           std::isdigit(static_cast<unsigned char>(spi_device[cursor])) != 0) {
      ++cursor;
    }
    if (cursor > bus_begin && cursor < spi_device.size() &&
        spi_device[cursor] == '.') {
      return "spi" + spi_device.substr(bus_begin, cursor - bus_begin);
    }
  }
  // Not a spidev path we recognise (a test double's name, a future
  // transport). Serialise it against itself only, and prefix so it can
  // never collide with a canonical "spiN" key.
  return "raw:" + spi_device;
}

std::mutex& SpiBusMutex(const std::string& spi_device) {
  return Controller(spi_device).mu;
}

SpiBusLock::SpiBusLock(const std::string& spi_device)
    : controller_(Controller(spi_device)) {
  controller_.mu.lock();
  // Counted after the acquisition succeeds, so the delta a test observes is
  // "holds taken", not "holds attempted".
  controller_.acquisitions.fetch_add(1, std::memory_order_relaxed);
}

SpiBusLock::~SpiBusLock() { controller_.mu.unlock(); }

std::uint64_t SpiBusLockAcquireCount(const std::string& spi_device) {
  return Controller(spi_device).acquisitions.load(std::memory_order_relaxed);
}

}  // namespace coatheal
