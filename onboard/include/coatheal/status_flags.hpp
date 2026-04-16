#pragma once

#include <string>

namespace coatheal {

struct StatusFlags {
  bool sd_ok = true;
  bool usb_ok = true;
  bool i2c_ok = true;
  bool spi_ok = true;
  bool link_ok = true;
  bool t_ambient_ok = true;
  bool p_ambient_ok = true;
  bool uniformity_ok = true;
  bool overtemp_ok = true;
  bool energy_ok = true;
  bool rs485_ok = true;              // RS-485 bus healthy (stepper link, etc.)
  bool heater_inhibited = false;     // true while a pull cycle is active
                                     // (Agent D's scheduler sets this)
};

std::string ToStatusBitfield(const StatusFlags& flags);

}  // namespace coatheal
