#pragma once

#include <mutex>

namespace coatheal {

// Mutual-exclusion interlock between the two pulling motors.
//
// REV-B requirement: only one motor may pull at a time, and the heater
// scheduler must not drive duty while the lock is held (Agent D enforces the
// heater side of the interlock). The stepper channels call TryAcquire() before
// beginning a pull cycle and Release() when they are done (either at the end
// of the cycle or on Stop()/fault).
//
// This header intentionally ships a trivial single-mutex stub so stepper unit
// tests link and exercise the acquire/release contract end-to-end. Agent D is
// expected to replace the implementation (not the API) with the full cross-
// subsystem interlock — including the heater-duty gate — in a follow-up.
class MotionLock {
 public:
  // Attempt to acquire the lock on behalf of `motor_id`. Returns true on
  // success (the caller now owns the lock and must Release() it), false if a
  // different motor already holds it. Re-acquire by the same holder is
  // idempotent and returns true.
  bool TryAcquire(int motor_id);

  // Release the lock. No-op if `motor_id` does not currently hold it — the
  // stub is tolerant so that error paths can call Release() unconditionally.
  void Release(int motor_id);

  // Current holder, or -1 if free.
  int holder() const;

 private:
  mutable std::mutex mu_;
  int holder_ = -1;
};

}  // namespace coatheal
