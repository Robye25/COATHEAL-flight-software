#include "coatheal/link_budget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <linux/tcp.h>  // glibc's struct tcp_info stops before tcpi_segs_in
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#endif

namespace coatheal {

namespace wire {

std::uint32_t TcpSegmentCount(std::size_t payload) {
  if (payload == 0) return 1;
  return static_cast<std::uint32_t>((payload + kMss - 1) / kMss);
}

std::uint32_t TcpBytes(std::size_t payload) {
  return TcpSegmentCount(payload) * (kTcpHeaders + kFrameOverhead) +
         static_cast<std::uint32_t>(payload);
}

std::uint32_t UdpBytes(std::size_t payload) {
  return std::max<std::uint32_t>(kMinFrame, kUdpHeaders + static_cast<std::uint32_t>(payload)) +
         kFrameOverhead;
}

bool WaitAllAcknowledged(int fd, std::chrono::milliseconds timeout) {
#if defined(__linux__)
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    tcp_info info{};
    socklen_t len = sizeof(info);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) return false;
    if (info.tcpi_unacked == 0) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
#else
  (void)fd;
  (void)timeout;
  return true;
#endif
}

bool ReadTcpState(int fd, TcpState* state) {
#if defined(__linux__)
  tcp_info info{};
  socklen_t len = sizeof(info);
  if (state == nullptr || fd < 0 || getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
      len < offsetof(tcp_info, tcpi_data_segs_in) + sizeof(info.tcpi_data_segs_in)) {
    return false;
  }
  state->unacked = info.tcpi_unacked;
  state->srtt = std::chrono::microseconds(info.tcpi_rtt);
  state->segments_in = info.tcpi_segs_in;
  state->data_segments_in = info.tcpi_data_segs_in;
  return true;
#else
  (void)fd;
  (void)state;
  return false;
#endif
}

std::chrono::milliseconds AckDeadline(int fd) {
  TcpState state;
  if (!ReadTcpState(fd, &state)) return kAckDeadline;
  const auto srtt = std::chrono::duration_cast<std::chrono::milliseconds>(state.srtt);
  return kAckDeadline + std::min(srtt, std::chrono::milliseconds(200));
}

}  // namespace wire

std::string LinkCapState(const std::string& state_file) {
  std::string path = state_file;
  if (path.empty()) {
    const char* env = std::getenv("COATHEAL_LINK_CAP_STATE");
    path = (env != nullptr && *env != '\0') ? env : "/run/coatheal-link-cap.state";
  }
  std::ifstream state(path);
  std::string word, port;
  if (!(state >> word >> port) || word != "on") return "off";
  // Ports are named by the kernel: letters, digits, '.', '_', '-'.
  for (const char c : port) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
      return "off";
    }
  }
  std::ifstream mtu_file("/sys/class/net/" + port + "/mtu");
  std::uint32_t mtu = 0;
  if (!(mtu_file >> mtu) || mtu > wire::kLinkMtu) return "stale:" + port;
  return "on:" + port;
}

LinkBudget::LinkBudget(std::uint32_t share_bytes, Clock::duration window, NowFn now)
    : share_(share_bytes), window_(window), now_(std::move(now)) {
  if (!now_) now_ = [] { return Clock::now(); };
}

LinkBudget::Clock::time_point LinkBudget::Now() const { return now_(); }

bool LinkBudget::TryCharge(std::uint32_t bytes, LinkPriority priority, std::uint32_t tx_bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  return AdmitLocked(bytes, priority, now_(), /*open=*/false, nullptr, tx_bytes);
}

bool LinkBudget::TryHold(std::uint32_t bytes, LinkPriority priority, Ticket* ticket,
                         std::uint32_t tx_bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  return AdmitLocked(bytes, priority, now_(), /*open=*/true, ticket, tx_bytes);
}

bool LinkBudget::WaitHold(std::uint32_t bytes, LinkPriority priority,
                          Clock::time_point deadline, Ticket* ticket, std::uint32_t tx_bytes) {
  std::unique_lock<std::mutex> lock(mu_);
  // A request larger than the whole share can never be admitted.
  if (bytes > share_) return false;
  waiting_.push_back(static_cast<int>(priority));
  bool admitted = false;
  for (;;) {
    const Clock::time_point now = now_();
    // Leave the waiter list before the admission check so this waiter does
    // not count as "more urgent" against itself.
    waiting_.erase(std::find(waiting_.begin(), waiting_.end(), static_cast<int>(priority)));
    if (AdmitLocked(bytes, priority, now, /*open=*/true, ticket, tx_bytes)) {
      admitted = true;
      break;
    }
    if (now >= deadline) break;
    waiting_.push_back(static_cast<int>(priority));
    // Short slices keep a fake clock (tests) and the real one equally live:
    // the next expiry is re-evaluated through now_() on every wake.
    cv_.wait_for(lock, std::chrono::milliseconds(10));
  }
  cv_.notify_all();
  return admitted;
}

void LinkBudget::Release(const Ticket& ticket) { ReleaseAt(ticket, now_()); }

void LinkBudget::ReleaseAt(const Ticket& ticket, Clock::time_point when) {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& entry : entries_) {
    if (entry.id == ticket.id) {
      entry.end = when;
      break;
    }
  }
  cv_.notify_all();
}

void LinkBudget::ReleasePart(const Ticket& ticket, std::uint32_t bytes,
                             Clock::time_point when) {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& entry : entries_) {
    if (entry.id == ticket.id) {
      const std::uint32_t part = std::min(bytes, entry.bytes);
      entry.bytes -= part;
      Entry closed;
      closed.id = next_id_++;
      closed.end = when;
      closed.bytes = part;
      entries_.push_back(closed);
      break;
    }
  }
  cv_.notify_all();
}

void LinkBudget::Refund(const Ticket& ticket, std::uint32_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& entry : entries_) {
    if (entry.id == ticket.id) {
      entry.bytes -= std::min(bytes, entry.bytes);
      break;
    }
  }
  cv_.notify_all();
}

std::uint32_t LinkBudget::InWindow() const {
  std::lock_guard<std::mutex> lock(mu_);
  return InWindowLocked(now_());
}

void LinkBudget::Notify() { cv_.notify_all(); }

std::uint32_t LinkBudget::InWindowLocked(Clock::time_point now) const {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& entry) {
                                  return entry.end != Clock::time_point::max() &&
                                         entry.end + window_ <= now;
                                }),
                 entries_.end());
  std::uint64_t sum = 0;
  for (const Entry& entry : entries_) sum += entry.bytes;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, UINT32_MAX));
}

bool LinkBudget::MoreUrgentWaitingLocked(LinkPriority priority) const {
  return std::any_of(waiting_.begin(), waiting_.end(),
                     [&](int waiter) { return waiter < static_cast<int>(priority); });
}

void LinkBudget::SetEgressShaper(std::uint32_t burst_bytes, std::uint32_t rate_bytes_per_s) {
  std::lock_guard<std::mutex> lock(mu_);
  egress_burst_ = burst_bytes;
  egress_rate_ = rate_bytes_per_s;
  egress_tokens_ = burst_bytes;
  egress_stamp_ = now_();
  cv_.notify_all();
}

void LinkBudget::RefillEgressLocked(Clock::time_point now) const {
  if (now > egress_stamp_) {
    egress_tokens_ = std::min(
        egress_burst_,
        egress_tokens_ + egress_rate_ * std::chrono::duration<double>(now - egress_stamp_).count());
    egress_stamp_ = now;
  }
}

void LinkBudget::DebitEgress(std::uint32_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  if (egress_burst_ <= 0.0) return;
  RefillEgressLocked(now_());
  // The kernel's bucket is never emptier than empty: what does not fit waits
  // in its queue (at most a second's worth) or is dropped. The model's debt
  // is bounded the same way, or a burst nobody scheduled -- a port scan of
  // the command port -- would hold our own traffic back long after the
  // kernel has let it go.
  egress_tokens_ = std::max(egress_tokens_ - bytes, -egress_burst_);
}

void LinkBudget::RefundEgress(std::uint32_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  if (egress_burst_ <= 0.0) return;
  RefillEgressLocked(now_());
  egress_tokens_ = std::min(egress_burst_, egress_tokens_ + bytes);
  cv_.notify_all();
}

std::int64_t LinkBudget::EgressTokens() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (egress_burst_ <= 0.0) return 0;
  RefillEgressLocked(now_());
  return static_cast<std::int64_t>(std::floor(egress_tokens_));
}

bool LinkBudget::AdmitLocked(std::uint32_t bytes, LinkPriority priority,
                             Clock::time_point now, bool open, Ticket* ticket,
                             std::uint32_t tx_bytes) {
  if (bytes > share_) return false;
  if (MoreUrgentWaitingLocked(priority)) return false;
  if (InWindowLocked(now) + static_cast<std::uint64_t>(bytes) > share_) return false;
  if (egress_burst_ > 0.0 && tx_bytes > 0) {
    RefillEgressLocked(now);
    // A charge larger than the bucket goes when the bucket is full, and
    // leaves it in debt.
    if (egress_tokens_ < std::min<double>(tx_bytes, egress_burst_)) return false;
    egress_tokens_ -= tx_bytes;
  }
  Entry entry;
  entry.id = next_id_++;
  entry.end = open ? Clock::time_point::max() : now;
  entry.bytes = bytes;
  entries_.push_back(entry);
  if (ticket != nullptr) ticket->id = entry.id;
  return true;
}

}  // namespace coatheal
