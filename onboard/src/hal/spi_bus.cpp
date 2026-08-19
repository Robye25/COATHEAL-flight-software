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

  wire_mode_ = mode;
  if (no_cs) wire_mode_ = static_cast<std::uint8_t>(wire_mode_ | SPI_NO_CS);
  bits_per_word_ = 8;
  speed_hz_ = speed_hz;

  // Open-time application doubles as a validation of the requested
  // settings: a node that rejects them here must not look openable.
  // ApplyBusSettings() replays exactly these three ioctls before every
  // transfer, because another opener of the same node can clobber them at
  // any time (see spi_bus.hpp).
  if (!ApplyBusSettings()) {
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

bool LinuxSpiBus::ApplyBusSettings() {
#if COATHEAL_HAS_LINUX_SPI
  if (fd_ < 0) return false;

  // Three cheap ioctls (no bus traffic, no conversion): they only rewrite
  // the kernel's per-node struct spi_device fields. Cost is negligible next
  // to the SPI_IOC_MESSAGE that follows, and paying it every transfer is
  // what makes a transfer self-consistent when motors (mode 3 | SPI_NO_CS)
  // and a click (mode 1, native CE0) share /dev/spidev0.0.
  std::uint8_t wire_mode = wire_mode_;
  if (::ioctl(fd_, SPI_IOC_WR_MODE, &wire_mode) < 0) return false;

  std::uint8_t bits_per_word = bits_per_word_;
  if (::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) return false;

  std::uint32_t speed_hz = speed_hz_;
  if (::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) return false;

  return true;
#else
  return false;
#endif
}

bool LinuxSpiBus::TransferData(const std::uint8_t* tx, std::uint8_t* rx,
                               std::size_t len) {
#if COATHEAL_HAS_LINUX_SPI
  if (fd_ < 0 || tx == nullptr || rx == nullptr) return false;

  struct spi_ioc_transfer xfer;
  std::memset(&xfer, 0, sizeof(xfer));
  xfer.tx_buf = reinterpret_cast<std::uintptr_t>(tx);
  xfer.rx_buf = reinterpret_cast<std::uintptr_t>(rx);
  xfer.len = static_cast<__u32>(len);
  // Belt-and-braces: pin the per-message speed/bits too, so even a message
  // that somehow raced the ioctls above still clocks at this opener's rate.
  xfer.speed_hz = speed_hz_;
  xfer.bits_per_word = bits_per_word_;

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
