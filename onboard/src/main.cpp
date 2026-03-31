#include "coatheal/config.hpp"
#include "coatheal/system_controller.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  std::string config_path = "config/onboard.example.ini";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--help") {
      std::cout << "Usage: coatheal_onboard [--config <path>]\n";
      return 0;
    }
  }

  coatheal::OnboardConfig config;
  std::string error;
  if (!coatheal::LoadConfigFromIni(config_path, &config, &error)) {
    std::cerr << "Failed to load config: " << error << std::endl;
    return 1;
  }

  coatheal::SystemController controller(config);
  if (!controller.Initialize(&error)) {
    std::cerr << "Failed to initialize system controller: " << error << std::endl;
    return 1;
  }

  return controller.Run();
}