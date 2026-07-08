#include "coatheal/status_flags.hpp"

#include <sstream>

namespace coatheal {

std::string ToStatusBitfield(const StatusFlags& flags) {
  std::ostringstream oss;
  oss << (flags.sd_ok ? "SD_OK" : "SD_FAIL") << '|'
      << (flags.usb_ok ? "USB_OK" : "USB_FAIL") << '|'
      << (flags.i2c_ok ? "I2C_OK" : "I2C_FAIL") << '|'
      << (flags.spi_ok ? "SPI_OK" : "SPI_FAIL") << '|'
      << (flags.link_ok ? "LINK_OK" : "LINK_FAIL") << '|'
      << (flags.t_ambient_ok ? "T_AMBIENT_OK" : "T_AMBIENT_FAIL") << '|'
      << (flags.p_ambient_ok ? "P_AMBIENT_OK" : "P_AMBIENT_FAIL") << '|'
      << (flags.uniformity_ok ? "UNIFORMITY_OK" : "UNIFORMITY_FAIL") << '|'
      << (flags.overtemp_ok ? "OVERTEMP_OK" : "OVERTEMP_FAIL") << '|'
      << (flags.energy_ok ? "ENERGY_OK" : "ENERGY_FAIL") << '|'
      << (flags.rs485_ok ? "RS485_OK" : "RS485_FAIL") << '|'
      << (flags.pwm_ok ? "PWM_OK" : "PWM_FAIL") << '|'
      << (flags.stepper_ok ? "STEPPER_OK" : "STEPPER_FAIL") << '|'
      << (flags.sample_temp_ok ? "SAMPLE_TEMP_OK" : "SAMPLE_TEMP_FAIL") << '|'
      << (flags.simulated ? "SIMULATED" : "REAL_SENSORS") << '|'
      << (flags.sequence_paused ? "SEQ_PAUSED" : "SEQ_READY") << '|'
      << (flags.heater_inhibited ? "HEATER_INHIBITED" : "HEATER_ACTIVE") << '|'
      << (flags.resistance_ok ? "RESISTANCE_OK" : "RESISTANCE_FAIL");
  return oss.str();
}

}  // namespace coatheal
