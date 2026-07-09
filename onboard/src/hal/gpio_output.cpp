#include "coatheal/hal/gpio_output.hpp"

#include <memory>

#ifdef COATHEAL_HAS_LIBGPIOD
#include <gpiod.h>
#endif

namespace coatheal {

struct GpioOutput {
#ifdef COATHEAL_HAS_LIBGPIOD
  gpiod_chip* chip = nullptr;
#ifdef COATHEAL_LIBGPIOD_V2
  gpiod_line_request* request = nullptr;
  unsigned int offset = 0;
#else
  gpiod_line* line = nullptr;
#endif
#endif
};

GpioOutput* RequestGpioOutput(const std::string& chip_path,
                              std::size_t offset,
                              const char* consumer,
                              bool initial_value) {
#ifdef COATHEAL_HAS_LIBGPIOD
  auto output = std::make_unique<GpioOutput>();
  output->chip = gpiod_chip_open(chip_path.c_str());
  if (output->chip == nullptr) {
    return nullptr;
  }

#ifdef COATHEAL_LIBGPIOD_V2
  auto* settings = gpiod_line_settings_new();
  auto* line_config = gpiod_line_config_new();
  auto* request_config = gpiod_request_config_new();
  if (settings == nullptr || line_config == nullptr || request_config == nullptr) {
    if (request_config != nullptr) {
      gpiod_request_config_free(request_config);
    }
    if (line_config != nullptr) {
      gpiod_line_config_free(line_config);
    }
    if (settings != nullptr) {
      gpiod_line_settings_free(settings);
    }
    gpiod_chip_close(output->chip);
    return nullptr;
  }

  const auto initial = initial_value ? GPIOD_LINE_VALUE_ACTIVE
                                     : GPIOD_LINE_VALUE_INACTIVE;
  const unsigned int line_offset = static_cast<unsigned int>(offset);
  const bool configured =
      gpiod_line_settings_set_direction(
          settings, GPIOD_LINE_DIRECTION_OUTPUT) == 0 &&
      gpiod_line_settings_set_output_value(settings, initial) == 0 &&
      gpiod_line_config_add_line_settings(
          line_config, &line_offset, 1, settings) == 0;
  gpiod_request_config_set_consumer(request_config, consumer);
  if (configured) {
    output->request =
        gpiod_chip_request_lines(output->chip, request_config, line_config);
    output->offset = line_offset;
  }

  gpiod_request_config_free(request_config);
  gpiod_line_config_free(line_config);
  gpiod_line_settings_free(settings);
  if (output->request == nullptr) {
    gpiod_chip_close(output->chip);
    return nullptr;
  }
#else
  output->line =
      gpiod_chip_get_line(output->chip, static_cast<unsigned int>(offset));
  if (output->line == nullptr ||
      gpiod_line_request_output(output->line, consumer,
                                initial_value ? 1 : 0) < 0) {
    gpiod_chip_close(output->chip);
    return nullptr;
  }
#endif

  return output.release();
#else
  (void)chip_path;
  (void)offset;
  (void)consumer;
  (void)initial_value;
  return nullptr;
#endif
}

bool SetGpioOutput(GpioOutput* output, bool value) {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (output == nullptr) {
    return false;
  }
#ifdef COATHEAL_LIBGPIOD_V2
  return output->request != nullptr &&
         gpiod_line_request_set_value(
             output->request, output->offset,
             value ? GPIOD_LINE_VALUE_ACTIVE
                   : GPIOD_LINE_VALUE_INACTIVE) == 0;
#else
  return output->line != nullptr &&
         gpiod_line_set_value(output->line, value ? 1 : 0) == 0;
#endif
#else
  (void)output;
  (void)value;
  return false;
#endif
}

void ReleaseGpioOutput(GpioOutput* output) {
  if (output == nullptr) {
    return;
  }
#ifdef COATHEAL_HAS_LIBGPIOD
#ifdef COATHEAL_LIBGPIOD_V2
  if (output->request != nullptr) {
    gpiod_line_request_release(output->request);
  }
#else
  if (output->line != nullptr) {
    gpiod_line_release(output->line);
  }
#endif
  if (output->chip != nullptr) {
    gpiod_chip_close(output->chip);
  }
#endif
  delete output;
}

bool ReadGpioInputOnce(const std::string& chip_path,
                       std::size_t offset,
                       const char* consumer,
                       bool* value) {
#ifdef COATHEAL_HAS_LIBGPIOD
  if (value == nullptr) return false;
  gpiod_chip* chip = gpiod_chip_open(chip_path.c_str());
  if (chip == nullptr) return false;
#ifdef COATHEAL_LIBGPIOD_V2
  auto* settings = gpiod_line_settings_new();
  auto* line_config = gpiod_line_config_new();
  auto* request_config = gpiod_request_config_new();
  if (settings == nullptr || line_config == nullptr || request_config == nullptr) {
    if (request_config != nullptr) gpiod_request_config_free(request_config);
    if (line_config != nullptr) gpiod_line_config_free(line_config);
    if (settings != nullptr) gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return false;
  }
  const unsigned int line_offset = static_cast<unsigned int>(offset);
  const bool configured =
      gpiod_line_settings_set_direction(
          settings, GPIOD_LINE_DIRECTION_INPUT) == 0 &&
      gpiod_line_config_add_line_settings(
          line_config, &line_offset, 1, settings) == 0;
  gpiod_request_config_set_consumer(request_config, consumer);
  gpiod_line_request* request = nullptr;
  if (configured) {
    request = gpiod_chip_request_lines(chip, request_config, line_config);
  }
  gpiod_request_config_free(request_config);
  gpiod_line_config_free(line_config);
  gpiod_line_settings_free(settings);
  if (request == nullptr) {
    gpiod_chip_close(chip);
    return false;
  }
  const int read_value = static_cast<int>(
      gpiod_line_request_get_value(request, line_offset));
  gpiod_line_request_release(request);
  gpiod_chip_close(chip);
  if (read_value < 0) return false;
  *value = read_value == static_cast<int>(GPIOD_LINE_VALUE_ACTIVE);
  return true;
#else
  gpiod_line* line = gpiod_chip_get_line(chip, static_cast<unsigned int>(offset));
  if (line == nullptr || gpiod_line_request_input(line, consumer) < 0) {
    gpiod_chip_close(chip);
    return false;
  }
  const int read_value = gpiod_line_get_value(line);
  gpiod_line_release(line);
  gpiod_chip_close(chip);
  if (read_value < 0) return false;
  *value = read_value != 0;
  return true;
#endif
#else
  (void)chip_path;
  (void)offset;
  (void)consumer;
  (void)value;
  return false;
#endif
}

}  // namespace coatheal
