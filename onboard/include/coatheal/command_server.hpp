#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

#include "coatheal/link_budget.hpp"

namespace coatheal {

class CommandServer {
 public:
  using Handler = std::function<std::string(const std::string&,
                                            const std::string&)>;

  explicit CommandServer(int port);
  ~CommandServer();

  CommandServer(const CommandServer&) = delete;
  CommandServer& operator=(const CommandServer&) = delete;

  // The onboard share of the E-Link budget. Replies to remote peers are
  // paced through it (docs/link-budget.md); loopback peers never touch the
  // E-Link and are answered at once. Set before Start().
  void SetLinkBudget(LinkBudget* budget) { budget_ = budget; }
  // Tests only: pace loopback peers too, so the paced path runs on loopback.
  void SetPaceLoopbackForTesting(bool pace) { pace_loopback_ = pace; }

  bool Start(Handler handler, std::string* error);
  void Stop();

 private:
  void RunLoop();
  void HandleClient(int client_fd, const std::string& peer_ip);
  // Sends `response` in budget-sized chunks. False when the budget never
  // made room in time or a chunk went unacknowledged (the connection is then
  // reset rather than closed, so nothing more is retransmitted).
  bool SendPacedReply(int client_fd, const std::string& response);
  void CloseListenSocket();

  LinkBudget* budget_ = nullptr;
  bool pace_loopback_ = false;
  int port_ = 0;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread thread_;
  Handler handler_;
};

}  // namespace coatheal
