// REV B safety interlocks (Agent D).
//
// These tests enforce the heater↔motor mutex from the 2-motor + pull-cycle
// design:
//   1. MotionLock is strictly one-holder-at-a-time.
//   2. Release by a non-holder is a no-op (cannot be spoofed).
//   3. Concurrency: exactly one of two racing threads wins the lock.
//   4. HeaterScheduler + MotionLock: active lock forces ALL duties to zero.
//   5. Lock release restores normal scheduling.
//
// Paranoid mindset: the tests treat "any non-zero duty while the lock is
// held" as a catastrophic bug. A future well-meaning refactor that changes
// the interlock to a "clamp to 20 %" soft limit must trip test 4.
#include <atomic>
#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/heater_scheduler.hpp"
#include "coatheal/motion_lock.hpp"

namespace {

void TestMotionLockBasic() {
  coatheal::MotionLock lock;
  assert(!lock.is_active());
  assert(lock.holder() == -1);

  assert(lock.TryAcquire(0));
  assert(lock.is_active());
  assert(lock.holder() == 0);

  // A second motor cannot barge in while motor 0 holds it.
  assert(!lock.TryAcquire(1));
  assert(lock.holder() == 0);

  // Re-acquire by the same motor without a release MUST also fail — the
  // contract says "records motor_id iff lock was free". Paranoid: a buggy
  // caller must not be able to double-lock and confuse Release semantics.
  assert(!lock.TryAcquire(0));
  assert(lock.holder() == 0);

  lock.Release(0);
  assert(!lock.is_active());
  assert(lock.holder() == -1);

  assert(lock.TryAcquire(1));
  assert(lock.holder() == 1);
  lock.Release(1);
}

void TestReleaseByNonHolderIsNoOp() {
  coatheal::MotionLock lock;
  assert(lock.TryAcquire(0));
  assert(lock.is_active());

  // Adversarial caller tries to spoof a release as motor 1. Must not
  // clear the lock, otherwise motor 0 would start heating while still
  // mid-pull.
  lock.Release(1);
  assert(lock.is_active());
  assert(lock.holder() == 0);

  // Also try -1 (the "no holder" sentinel): must not clear the lock.
  lock.Release(-1);
  assert(lock.is_active());
  assert(lock.holder() == 0);

  // Real holder can still release.
  lock.Release(0);
  assert(!lock.is_active());
}

void TestMotionLockConcurrency() {
  for (int trial = 0; trial < 100; ++trial) {
    coatheal::MotionLock lock;
    std::atomic<int> successes{0};
    std::atomic<int> winner{-1};
    std::atomic<bool> start{false};

    auto worker = [&](int id) {
      while (!start.load(std::memory_order_acquire)) {
        // spin until start signal for tighter contention
      }
      if (lock.TryAcquire(id)) {
        successes.fetch_add(1, std::memory_order_relaxed);
        winner.store(id, std::memory_order_relaxed);
      } else {
        // Loser MUST see the lock as active (somebody holds it).
        assert(lock.is_active());
      }
    };

    std::thread t0(worker, 0);
    std::thread t1(worker, 1);
    start.store(true, std::memory_order_release);
    t0.join();
    t1.join();

    assert(successes.load() == 1);
    const int w = winner.load();
    assert(w == 0 || w == 1);
    assert(lock.is_active());
    assert(lock.holder() == w);
    lock.Release(w);
    assert(!lock.is_active());
  }
}

coatheal::PowerConfig MakePower() {
  coatheal::PowerConfig p;
  p.max_active_heaters = 3;
  p.heater_nominal_w = 30.0;  // "3 heaters @ 30 W" from the spec
  p.max_thermal_w = 150.0;    // generous; we want the request to pass through
  p.energy_budget_wh = 0.0;   // disable budget for this test
  return p;
}

void TestSchedulerInhibitedWhenLocked() {
  const auto power = MakePower();
  coatheal::MotionLock lock;
  coatheal::HeaterScheduler sched(power, /*electronics_heater_index=*/9, &lock);

  // 10 channels, request 3 heaters at full duty (full 30 W each).
  std::vector<double> requested(10, 0.0);
  requested[0] = 1.0;
  requested[1] = 1.0;
  requested[2] = 1.0;

  // --- Baseline: no lock held. Scheduler should pass through the 3 heaters.
  assert(!sched.heater_inhibited());
  auto out = sched.Schedule(requested, /*deprioritize_electronics=*/true, 1.0);
  assert(out.size() == requested.size());
  int active = 0;
  double total_w = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] > 1e-9) {
      ++active;
    }
    total_w += out[i] * power.heater_nominal_w;
  }
  assert(active == 3);
  assert(std::fabs(total_w - 90.0) < 1e-6);  // 3 * 30 W
  assert(!sched.heater_inhibited());
  assert(!sched.last_inhibited());

  // --- Acquire the lock for motor 0. EVERY duty must be zero.
  assert(lock.TryAcquire(0));
  auto inhibited = sched.Schedule(requested, true, 1.0);
  assert(inhibited.size() == requested.size());
  for (std::size_t i = 0; i < inhibited.size(); ++i) {
    // PARANOID ASSERT: the interlock is binary. Any non-zero duty while
    // a motor is pulling is a potential melt event. This test MUST fail
    // if someone later softens the interlock to a 20 % clamp.
    assert(inhibited[i] == 0.0);
  }
  assert(sched.heater_inhibited());
  assert(sched.last_inhibited());

  // Also check the scheduler does not silently let a "cheeky" tiny request
  // through: even a 0.001 duty is zeroed while the lock is held.
  std::vector<double> sneaky(10, 0.001);
  auto sneaky_out = sched.Schedule(sneaky, true, 1.0);
  for (double d : sneaky_out) {
    assert(d == 0.0);
  }
  assert(sched.heater_inhibited());

  // --- Release the lock. Next Schedule() must return to normal behavior.
  lock.Release(0);
  assert(!lock.is_active());
  auto resumed = sched.Schedule(requested, true, 1.0);
  int active_after = 0;
  double total_after = 0.0;
  for (std::size_t i = 0; i < resumed.size(); ++i) {
    if (resumed[i] > 1e-9) {
      ++active_after;
    }
    total_after += resumed[i] * power.heater_nominal_w;
  }
  assert(active_after == 3);
  assert(std::fabs(total_after - 90.0) < 1e-6);
  assert(!sched.heater_inhibited());
  assert(!sched.last_inhibited());
}

void TestSchedulerNullLockIsAlwaysFree() {
  // Contract says a null MotionLock* behaves as "always free". This is the
  // bench-mode path and also what Agent B's test stub will rely on.
  const auto power = MakePower();
  coatheal::HeaterScheduler sched(power, 9, nullptr);
  std::vector<double> requested(10, 1.0);
  auto out = sched.Schedule(requested, true, 1.0);
  int active = 0;
  for (double d : out) if (d > 1e-9) ++active;
  assert(active == 3);  // max_active_heaters
  assert(!sched.heater_inhibited());
}

// Extra paranoid test: even with an extreme request (all 10 heaters at
// 100 %), the scheduler must emit all zeros while the lock is held. This
// defends against a future "clamp instead of zero" regression.
void TestSchedulerZeroIsZero_NoSoftClamp() {
  const auto power = MakePower();
  coatheal::MotionLock lock;
  coatheal::HeaterScheduler sched(power, 9, &lock);

  assert(lock.TryAcquire(1));
  std::vector<double> requested(10, 1.0);
  auto out = sched.Schedule(requested, false, 1.0);
  // Sum of duties must be exactly zero — not small, not 20 %, ZERO.
  double sum = 0.0;
  for (double d : out) sum += d;
  assert(sum == 0.0);
  for (double d : out) {
    assert(d == 0.0);
  }
  assert(sched.heater_inhibited());
  lock.Release(1);
}

}  // namespace

int main() {
  TestMotionLockBasic();
  TestReleaseByNonHolderIsNoOp();
  TestMotionLockConcurrency();
  TestSchedulerInhibitedWhenLocked();
  TestSchedulerNullLockIsAlwaysFree();
  TestSchedulerZeroIsZero_NoSoftClamp();
  std::cout << "test_safety_rev_b: OK\n";
  return 0;
}
