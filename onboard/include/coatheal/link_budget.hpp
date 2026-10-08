#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace coatheal {

// On-wire byte model of docs/link-budget.md: the Ethernet frame (minimum 60
// B) plus 24 B for preamble/SFD, FCS and inter-frame gap.
namespace wire {
constexpr std::uint32_t kFrameOverhead = 24;
constexpr std::uint32_t kTcpHeaders = 66;  // Ethernet 14 + IPv4 20 + TCP 32 (timestamps)
constexpr std::uint32_t kUdpHeaders = 42;  // Ethernet 14 + IPv4 20 + UDP 8
constexpr std::uint32_t kMinFrame = 60;
// The E-Link port runs at this MTU while the hard cap is installed
// (scripts/link_cap.sh), so that no frame is larger than the shaper's bucket.
constexpr std::uint32_t kLinkMtu = 576;
constexpr std::uint32_t kMss = kLinkMtu - 52;  // 524: IPv4 20 + TCP 32 (timestamps)
constexpr std::uint32_t kPureAck = kTcpHeaders + kFrameOverhead;  // 90, also FIN
constexpr std::uint32_t kSyn = 74 + kFrameOverhead;               // 98, also SYN-ACK
constexpr std::uint32_t kReset = kMinFrame + kFrameOverhead;      // 84

std::uint32_t TcpSegmentCount(std::size_t payload);
// Every segment needed to carry `payload` bytes, headers included.
std::uint32_t TcpBytes(std::size_t payload);
std::uint32_t UdpBytes(std::size_t payload);

// Linux never retransmits a segment sooner than 200 ms plus the smoothed
// round trip after sending it (TCP_RTO_MIN; a tail-loss probe for a lone
// segment waits at least as long). Whatever is not acknowledged within this
// deadline (plus that round trip: AckDeadline) gets its connection reset
// before the kernel could retransmit it, so no retransmission ever reaches the
// E-Link and none has to be budgeted.
constexpr std::chrono::milliseconds kAckDeadline{180};

// After a reset, answers the peer sent before the reset reached it may still
// arrive, and our kernel answers each one with a reset of its own. Charges
// covering those stay open this long plus one round trip.
constexpr std::chrono::milliseconds kAbortTail{500};

// What Linux reports about a TCP socket (TCP_INFO): segments sent and not yet
// acknowledged, the smoothed round trip, and the segments received so far with
// how many of them carried data.
struct TcpState {
  std::uint32_t unacked = 0;
  std::chrono::microseconds srtt{0};
  std::uint32_t segments_in = 0;
  std::uint32_t data_segments_in = 0;
};
// False where the kernel does not report all of it; callers then assume the
// costlier case.
bool ReadTcpState(int fd, TcpState* state);

// kAckDeadline plus the socket's smoothed round trip (at most 200 ms of it):
// still short of the first moment Linux could retransmit.
std::chrono::milliseconds AckDeadline(int fd);

// Whether the peer has acknowledged everything written on `fd`, waiting up to
// `timeout` (Linux TCP_INFO). Elsewhere the write is taken as delivered.
bool WaitAllAcknowledged(int fd, std::chrono::milliseconds timeout);
}  // namespace wire

// Shares of the 24 kbps cap, in bytes that may appear in any 1-second window.
constexpr std::uint32_t kLinkCapBytes = 3000;
constexpr std::uint32_t kOnboardShareBytes = 1600;
constexpr std::uint32_t kGroundShareBytes = 1150;
constexpr std::uint32_t kUnaccountedBytes = 250;

// Hard cap (docs/link-budget.md, "Hard cap"). The kernel of each side shapes
// everything that side sends on the E-Link port with a token bucket
// (scripts/link_cap.sh), so that no bug, retransmission or other program can
// put more on the wire. A bucket of `burst` bytes refilled at `rate` bytes per
// second passes at most burst + rate bytes in any 1-second window:
//   onboard 1 000 + 800 = 1 800 B, ground 700 + 500 = 1 200 B, 3 000 B in all.
constexpr std::uint32_t kOnboardEgressBurstBytes = 1000;
constexpr std::uint32_t kOnboardEgressRateBytesPerS = 800;
constexpr std::uint32_t kGroundEgressBurstBytes = 700;
constexpr std::uint32_t kGroundEgressRateBytesPerS = 500;
// The ledger's model of that bucket refills this much slower than the kernel's,
// which covers clock drift and the few frames nobody schedules (ARP).
constexpr std::uint32_t kEgressModelRatePercent = 97;

// Whether the kernel shaper of the hard cap is on this machine's E-Link port,
// from the state scripts/link_cap.sh leaves behind: "on:<port>", "off", or
// "stale:<port>" when the port no longer runs at the capped MTU (the shaper
// was installed, and something has since reset the port). Reported by STATUS.
std::string LinkCapState(const std::string& state_file = "");

// Highest first. A sender never takes bytes a more urgent waiter is queued for.
enum class LinkPriority : int {
  kCommandReply = 0,
  kLive = 1,
  kDiscovery = 2,
  kReplay = 3,
};

// Sliding-window ledger for one share of the link budget.
//
// A charge is placed before its bytes can reach the wire and counts from then
// until one window after it is released; an open hold counts for as long as
// it is open. Nothing is admitted unless everything counting now plus the new
// charge fits the share. As long as every byte a charge covers is emitted
// between placement and release, every charge emitting inside a given window
// on the wire is still counting at that window's end -- so no window on the
// wire can carry more than the share. Refund only bytes that provably never
// reached the wire (a retransmission allowance whose ACK came first).
//
// With SetEgressShaper() the ledger also models the kernel shaper on our own
// E-Link port: every charge names the bytes our side sends for it (`tx_bytes`)
// and is admitted only while the modelled bucket holds them. What the ledger
// lets out therefore never has to wait in the shaper, and the shaper is left
// to stop what the ledger does not know about.
class LinkBudget {
 public:
  using Clock = std::chrono::steady_clock;
  using NowFn = std::function<Clock::time_point()>;

  struct Ticket {
    std::uint64_t id = 0;
  };

  explicit LinkBudget(std::uint32_t share_bytes,
                      Clock::duration window = std::chrono::seconds(1),
                      NowFn now = nullptr);

  LinkBudget(const LinkBudget&) = delete;
  LinkBudget& operator=(const LinkBudget&) = delete;

  // Charge `bytes` for one window from now, if they fit and nothing more
  // urgent is waiting. Charges nothing and returns false otherwise.
  // `tx_bytes` of them are sent by our side (see SetEgressShaper).
  bool TryCharge(std::uint32_t bytes, LinkPriority priority, std::uint32_t tx_bytes = 0);
  // Same admission rule; the bytes count until Release() plus one window.
  bool TryHold(std::uint32_t bytes, LinkPriority priority, Ticket* ticket,
               std::uint32_t tx_bytes = 0);
  // Waits (priority order) until the hold fits or `deadline` passes.
  bool WaitHold(std::uint32_t bytes, LinkPriority priority,
                Clock::time_point deadline, Ticket* ticket, std::uint32_t tx_bytes = 0);
  // Closes a hold at `when` (default now; later is fine, earlier is not): the
  // bytes keep counting for one window after it.
  void Release(const Ticket& ticket);
  void ReleaseAt(const Ticket& ticket, Clock::time_point when);
  // Closes `bytes` of an open hold at `when` and leaves the rest open: a
  // frame's own segment is on the wire the moment send() returns, while the
  // ACK it provokes is still to come.
  void ReleasePart(const Ticket& ticket, std::uint32_t bytes, Clock::time_point when);
  // Removes bytes from a charge that never reached the wire.
  void Refund(const Ticket& ticket, std::uint32_t bytes);

  // Models a kernel token bucket of `burst_bytes`, refilled at
  // `rate_bytes_per_s`, on our E-Link port. From then on a charge is admitted
  // only while the bucket holds its `tx_bytes` (a charge larger than the
  // bucket: only while the bucket is full). Without it nothing is modelled.
  void SetEgressShaper(std::uint32_t burst_bytes, std::uint32_t rate_bytes_per_s);
  // Bytes our side sends that no charge waited for (the kernel's part of a
  // command exchange the ground station opened, a reset). The bucket may go
  // into debt, by at most one bucket; later charges wait until it is paid
  // back.
  void DebitEgress(std::uint32_t bytes);
  // Gives back `tx_bytes` of a charge that were never sent.
  void RefundEgress(std::uint32_t bytes);
  // Tokens in the modelled bucket now (negative while in debt); 0 when
  // nothing is modelled.
  std::int64_t EgressTokens() const;
  bool egress_shaped() const { return egress_burst_ > 0; }

  std::uint32_t InWindow() const;
  std::uint32_t share_bytes() const { return share_; }
  Clock::time_point Now() const;
  // Wakes waiters so they re-check (used by tests that drive a fake clock).
  void Notify();

 private:
  struct Entry {
    std::uint64_t id = 0;
    Clock::time_point end;  // time_point::max() while open
    std::uint32_t bytes = 0;
  };

  std::uint32_t InWindowLocked(Clock::time_point now) const;
  bool AdmitLocked(std::uint32_t bytes, LinkPriority priority,
                   Clock::time_point now, bool open, Ticket* ticket, std::uint32_t tx_bytes);
  bool MoreUrgentWaitingLocked(LinkPriority priority) const;
  void RefillEgressLocked(Clock::time_point now) const;

  const std::uint32_t share_;
  const Clock::duration window_;
  NowFn now_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  // Pruned lazily whenever the window is summed.
  mutable std::deque<Entry> entries_;
  std::vector<int> waiting_;  // priorities of blocked WaitHold calls
  std::uint64_t next_id_ = 1;
  // Model of the kernel shaper (0 burst: none). Tokens are bytes.
  double egress_burst_ = 0.0;
  double egress_rate_ = 0.0;
  mutable double egress_tokens_ = 0.0;
  mutable Clock::time_point egress_stamp_{};
};

}  // namespace coatheal
