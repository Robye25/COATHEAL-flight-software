#pragma once

#include <mutex>

namespace coatheal {

// MotionLock is a safety interlock shared between the stepper motion layer
// (Agent B) and the heater scheduler (Agent D).
//
// Contract (REV B, 2-motor + pull-cycle design):
//   * Only one motor may hold the lock at a time. Pull cycles for two
//     motors MUST serialise; any second attempt fails fast.
//   * While the lock is held, all heater duties MUST be clamped to zero
//     by the scheduler — not reduced, not proportionally scaled, ZERO.
//     A melt-through sample on a pull cycle is not an acceptable failure
//     mode.
//
// Thread-safety: all public methods are safe to call concurrently. holder()
// and is_active() return a stable snapshot taken under the mutex.
class MotionLock {
 public:
  MotionLock() = default;

  // Attempts to acquire the lock for `motor_id`. Returns true iff the lock
  // was free and is now held by `motor_id`. Returns false if any motor
  // already holds the lock (including the same motor_id — callers must not
  // re-acquire without releasing).
  bool TryAcquire(int motor_id);

  // Releases the lock iff `motor_id` is the current holder. No-op otherwise.
  // Intentionally silent about mismatched release attempts: the paranoid
  // view is that an adversarial caller trying to unlock someone else's
  // motion MUST NOT succeed, but must not crash the flight software either.
  void Release(int motor_id);

  // Returns the current holder's motor_id, or -1 if the lock is free.
  int holder() const;

  // Returns true iff a motor currently holds the lock.
  bool is_active() const;

 private:
  mutable std::mutex mu_;
  int holder_{-1};
};

}  // namespace coatheal
