#pragma once

#include <string>

namespace coatheal {

struct StatusFlags {
  bool sd_ok = true;
  bool usb_ok = true;
  bool i2c_ok = true;
  bool spi_ok = true;
  bool link_ok = true;
};

std::string ToStatusBitfield(const StatusFlags& flags);

}  // namespace coatheal