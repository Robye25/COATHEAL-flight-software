#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace coatheal {

class CommandServer {
 public:
  using Handler = std::function<std::string(const std::string&)>;

  explicit CommandServer(int port);
  ~CommandServer();

  CommandServer(const CommandServer&) = delete;
  CommandServer& operator=(const CommandServer&) = delete;

  bool Start(Handler handler, std::string* error);
  void Stop();

 private:
  void RunLoop();
  void HandleClient(int client_fd);
  void CloseListenSocket();

  int port_ = 0;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread thread_;
  Handler handler_;
};

}  // namespace coatheal