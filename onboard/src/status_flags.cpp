#include "coatheal/status_flags.hpp"

#include <sstream>

namespace coatheal {

std::string ToStatusBitfield(const StatusFlags& flags) {
  std::ostringstream oss;
  oss << (flags.sd_ok ? "SD_OK" : "SD_FAIL") << '|'
      << (flags.usb_ok ? "USB_OK" : "USB_FAIL") << '|'
      << (flags.i2c_ok ? "I2C_OK" : "I2C_FAIL") << '|'
      << (flags.spi_ok ? "SPI_OK" : "SPI_FAIL") << '|'
      << (flags.link_ok ? "LINK_OK" : "LINK_FAIL");
  return oss.str();
}

}  // namespace coatheal
