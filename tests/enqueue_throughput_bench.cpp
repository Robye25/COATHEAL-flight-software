// Agent B — telemetry enqueue throughput bench.
//
// Drives TelemetryQueue::Enqueue() at a fixed rate for a fixed wall-clock
// window and reports per-call latency (avg / p50 / p99 / max). Frames are
// sized to the production DATA line (~300 bytes) so fsync cost is realistic.
//
// Threshold: enqueue p99 < 50 ms. If that fails, the tick-loop would block
// on the queue fsync and we'd need to batch writes.

#include "coatheal/telemetry_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  double hz = 50.0;
  double seconds = 60.0;
  std::string queue_dir = "/tmp/coatheal_perf/enqueue_bench";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--hz" && i + 1 < argc) hz = std::stod(argv[++i]);
    else if (a == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
    else if (a == "--queue-dir" && i + 1 < argc) queue_dir = argv[++i];
  }

  std::filesystem::remove_all(queue_dir);

  coatheal::TelemetryQueue q(queue_dir, /*retention_hours=*/24.0,
                             /*max_bytes=*/static_cast<std::uint64_t>(1) << 30);
  std::string err;
  if (!q.Initialize(&err)) {
    std::fprintf(stderr, "Initialize failed: %s\n", err.c_str());
    return 1;
  }

  const auto period = std::chrono::nanoseconds(
      static_cast<long long>(1.0e9 / hz));
  const std::size_t total = static_cast<std::size_t>(hz * seconds);
  std::vector<double> latencies_us;
  latencies_us.reserve(total);

  // ~300-byte payload (matches production DATA frame shape).
  const std::string payload(300, 'X');

  auto start = std::chrono::steady_clock::now();
  auto next = start;
  for (std::size_t i = 0; i < total; ++i) {
    coatheal::QueuedTelemetryFrame f;
    f.queued_epoch_s = 1700000000 + static_cast<std::int64_t>(i);
    f.session_id = "BENCH";
    f.seq = i;
    f.frame = payload;
    auto t0 = std::chrono::steady_clock::now();
    if (!q.Enqueue(f, &err)) {
      std::fprintf(stderr, "Enqueue failed at %zu: %s\n", i, err.c_str());
      return 2;
    }
    auto t1 = std::chrono::steady_clock::now();
    latencies_us.push_back(static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    next += period;
    std::this_thread::sleep_until(next);
  }

  auto sorted = latencies_us;
  std::sort(sorted.begin(), sorted.end());
  auto pick = [&](double q) {
    const std::size_t idx = std::min(
        sorted.size() - 1, static_cast<std::size_t>(sorted.size() * q));
    return sorted[idx];
  };
  double sum = 0.0;
  for (double x : latencies_us) sum += x;
  const double avg = sum / static_cast<double>(latencies_us.size());

  std::printf(
      "[enqueue] n=%zu hz=%.1f secs=%.1f "
      "avg_us=%.1f p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f\n",
      latencies_us.size(), hz, seconds,
      avg, pick(0.50), pick(0.90), pick(0.99), sorted.back());

  constexpr double kThresholdUs = 50'000.0;  // 50 ms
  if (pick(0.99) >= kThresholdUs) {
    std::fprintf(stderr, "[enqueue] FAIL: p99 %.1f us >= %.1f us\n",
                 pick(0.99), kThresholdUs);
    return 3;
  }
  return 0;
}
