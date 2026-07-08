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
  bool rs485_ok = true;              // DAQ132M Modbus RTU path healthy
  bool pwm_ok = true;                // GPIO/PWM output backend healthy
  bool stepper_ok = true;            // motor driver backends healthy
  bool sample_temp_ok = true;        // DAQ/PT100 sample-temperature path healthy
  bool simulated = false;            // explicit bench/simulation data path active
  bool sequence_paused = false;      // at least one bend sequence paused/faulted
  bool heater_inhibited = false;     // true while MotionLock is held
  bool resistance_ok = true;         // compatibility resistance field/source
                                     // healthy or intentionally disabled
};

std::string ToStatusBitfield(const StatusFlags& flags);

}  // namespace coatheal
