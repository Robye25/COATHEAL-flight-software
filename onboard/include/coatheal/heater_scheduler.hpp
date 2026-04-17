#pragma once

#include <cstddef>
#include <vector>

#include "coatheal/config.hpp"

namespace coatheal {

class MotionLock;  // forward-declared; header included in the .cpp

// HeaterScheduler enforces the BEXUS power constraints from User Manual §5.2:
//   * max_active_heaters    — at most N heaters energised simultaneously
//   * max_thermal_w         — peak combined thermal draw (proportional clamp)
//   * energy_budget_wh      — cumulative heater energy over the mission
//
// The energy budget is optional: when energy_budget_wh <= 0 it is disabled and
// the scheduler behaves like the legacy peak-power-only version. When set, the
// scheduler integrates duty * heater_nominal_w * dt every Schedule() call and
// returns all-zero duty cycles once the budget is exhausted, latching heaters
// off for the remainder of the mission.
//
// REV B safety interlock: if a non-null MotionLock is supplied and it is
// currently held by any motor (is_active() == true), Schedule() MUST return
// all-zero duties AND set last_inhibited() == true. This is a hard
// heater↔motor mutex — there is no "clamp to 20 %" fallback; anything
// non-zero while a pull is in progress is a safety violation.
class HeaterScheduler {
 public:
  HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index);
  HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index,
                  MotionLock* lock);

  // Schedule with no energy accounting (legacy entry point).
  std::vector<double> Schedule(const std::vector<double>& requested,
                               bool deprioritize_electronics);

  // Schedule with energy accounting. dt_seconds is the integration window for
  // this tick. When dt_seconds <= 0 this is identical to Schedule() above.
  std::vector<double> Schedule(const std::vector<double>& requested,
                               bool deprioritize_electronics,
                               double dt_seconds);

  // Cumulative heater energy delivered so far (Wh). Resets only with Reset().
  double energy_consumed_wh() const { return energy_consumed_wh_; }

  // True if energy_budget_wh > 0 and the budget has been reached. Once true,
  // Schedule() returns all-zero duty cycles for the rest of the mission.
  bool is_budget_exhausted() const { return budget_exhausted_; }

  // True iff the most recent Schedule() call was inhibited by the motion
  // lock (i.e., a motor was pulling and all duties were forced to zero).
  // Agent C wires this into the HEATER_INHIBITED STATUS bit.
  bool heater_inhibited() const { return last_inhibited_; }

  // Alias requested by the system_controller wiring plan. Identical to
  // heater_inhibited() — both names refer to the same latched-per-tick flag.
  bool last_inhibited() const { return last_inhibited_; }

  // Allow the orchestrator (system_controller) to attach / detach the
  // MotionLock after construction. Passing nullptr disables the interlock
  // (tests and ground-bench builds).
  void SetMotionLock(MotionLock* lock) { lock_ = lock; }

  // Reset cumulative energy and unlatch the budget. Intended for tests.
  void Reset();

 private:
  PowerConfig power_;
  std::size_t electronics_heater_index_ = 0;
  double energy_consumed_wh_ = 0.0;
  bool budget_exhausted_ = false;

  // Safety interlock: while any motor holds this lock, all heater duties
  // are forced to zero. Nullable — nullptr means "no interlock configured",
  // which is a valid bench-test mode.
  MotionLock* lock_ = nullptr;

  // Edge-detection state for one-shot logging on lock active/cleared
  // transitions. Also feeds the heater_inhibited() / last_inhibited()
  // getters consumed by StatusFlags.
  bool last_inhibited_ = false;

  // Pre-reserved scratch vectors so Schedule() no longer heap-allocates on
  // the steady-state tick path. Grown lazily to `requested.size()` on the
  // first call and then reused. Agent B perf fix.
  std::vector<double> scratch_clamped_;
  std::vector<std::pair<std::size_t, double>> scratch_ranked_;
  std::vector<double> scratch_scheduled_;
};

}  // namespace coatheal
