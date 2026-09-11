#include "coatheal/config.hpp"
#include "coatheal/system_controller.hpp"

#include <csignal>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
#ifndef _WIN32
  // A peer that resets a TCP connection makes the next write on that socket
  // raise SIGPIPE, whose default action terminates the process. The flight
  // software must survive a ground station going away mid-write: ignore the
  // signal so send() reports EPIPE and the socket is closed like any other
  // failure. (The socket code also passes MSG_NOSIGNAL where available;
  // this is the belt to that pair of braces.)
  std::signal(SIGPIPE, SIG_IGN);
#endif

  std::string config_path = "config/onboard.example.ini";
  bool check_config = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--check-config") {
      check_config = true;
    } else if (arg == "--help") {
      std::cout << "Usage: coatheal_onboard [--config <path>] [--check-config]\n";
      return 0;
    }
  }

  coatheal::OnboardConfig config;
  std::string error;
  if (!coatheal::LoadConfigFromIni(config_path, &config, &error)) {
    std::cerr << "Failed to load config: " << error << std::endl;
    return 1;
  }
  if (check_config) {
    std::cout << "Config OK: " << config_path << '\n';
    return 0;
  }

  coatheal::SystemController controller(config);
  if (!controller.Initialize(&error)) {
    std::cerr << "Failed to initialize system controller: " << error << std::endl;
    return 1;
  }

  return controller.Run();
}
