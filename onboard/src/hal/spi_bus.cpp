#include "coatheal/hal/spi_bus.hpp"

#if defined(__linux__) && __has_include(<linux/spi/spidev.h>)
#define COATHEAL_HAS_LINUX_SPI 1
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#else
#define COATHEAL_HAS_LINUX_SPI 0
#endif

namespace coatheal {

LinuxSpiBus::LinuxSpiBus() = default;

LinuxSpiBus::~LinuxSpiBus() { Close(); }

bool LinuxSpiBus::available() const { return COATHEAL_HAS_LINUX_SPI != 0; }

bool LinuxSpiBus::Open(const std::string& device, std::uint8_t mode,
                       std::uint32_t speed_hz, bool no_cs) {
#if COATHEAL_HAS_LINUX_SPI
  Close();
  fd_ = ::open(device.c_str(), O_RDWR);
  if (fd_ < 0) return false;

  std::uint8_t wire_mode = mode;
  if (no_cs) wire_mode = static_cast<std::uint8_t>(wire_mode | SPI_NO_CS);
  if (::ioctl(fd_, SPI_IOC_WR_MODE, &wire_mode) < 0) {
    Close();
    return false;
  }

  std::uint8_t bits_per_word = 8;
  if (::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) {
    Close();
    return false;
  }

  if (::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
    Close();
    return false;
  }

  return true;
#else
  (void)device;
  (void)mode;
  (void)speed_hz;
  (void)no_cs;
  return false;
#endif
}

bool LinuxSpiBus::Transfer(const std::uint8_t* tx, std::uint8_t* rx,
                           std::size_t len) {
#if COATHEAL_HAS_LINUX_SPI
  if (fd_ < 0 || tx == nullptr || rx == nullptr) return false;

  struct spi_ioc_transfer xfer;
  std::memset(&xfer, 0, sizeof(xfer));
  xfer.tx_buf = reinterpret_cast<std::uintptr_t>(tx);
  xfer.rx_buf = reinterpret_cast<std::uintptr_t>(rx);
  xfer.len = static_cast<__u32>(len);

  return ::ioctl(fd_, SPI_IOC_MESSAGE(1), &xfer) >= 0;
#else
  (void)tx;
  (void)rx;
  (void)len;
  return false;
#endif
}

void LinuxSpiBus::Close() {
#if COATHEAL_HAS_LINUX_SPI
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

}  // namespace coatheal
