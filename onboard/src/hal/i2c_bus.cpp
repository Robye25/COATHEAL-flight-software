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
