// Contract tests for RenderPwmPeriod -- the slice renderer inside
// LibgpiodPwmController::PwmLoop.
//
// The property under test is WHEN the duty is read. At
// heater.pwm_frequency_hz = 1.0 one PWM period is a full second, so the old
// once-per-period duty snapshot delayed a SetDuty(0) from
// SystemController::InhibitHeatersForMotion() (and from a MotionLock
// acquisition) by up to 1000 ms: a heater could stay energised for most of a
// second after a PULL was commanded. Reading per slice bounds that to one
// slice, ~10 ms at 1 Hz.
//
// Scope limit, stated honestly: LibgpiodPwmController's worker thread is
// only started under COATHEAL_HAS_LIBGPIOD, so the loop itself cannot run on
// this Windows dev host. These tests cover the extracted decision the loop
// delegates to; the surrounding thread/GPIO plumbing is covered by Gate 6's
// bench step in docs/tmc5160-commissioning.md, which measures the real GPIO
// drop against one SLICE rather than one period.

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "coatheal/hal/pwm_controller.hpp"

using namespace coatheal;

namespace {

struct Recorder {
  std::vector<std::pair<std::size_t, bool>> writes;
  // Slice index at which each write happened, captured by the harness so a
  // "wrote eventually" pass can't masquerade as "wrote within one slice".
  std::vector<int> write_slices;
  int duty_reads = 0;
  int slices_waited = 0;
};

// ---------------------------------------------------------------------
// Hand-computed, pinned before the assertions.
//
// 10 slices, 1 channel. Duty is 1.0 for the first three reads, 0.0 from the
// fourth read on -- i.e. the "inhibit" lands between slice 2 and slice 3.
//   on(slice) = slice < round(duty * 10)
//   duty 1.0 -> round(10.0) = 10 -> on for every slice 0..9
//   duty 0.0 -> round(0.0)  =  0 -> off for every slice
// So with a PER-SLICE read:
//   slice 0: duty 1.0 -> on=true,  last=false -> WRITE (0,true)
//   slice 1: duty 1.0 -> on=true,  no change
//   slice 2: duty 1.0 -> on=true,  no change
//   slice 3: duty 0.0 -> on=false, last=true  -> WRITE (0,false)
//   slices 4..9: off, no change
// => exactly 2 writes, the second at slice index 3, and 10 duty reads.
// With a once-per-period read the duty would be latched at 1.0 for the whole
// period: 1 write, none turning the line off.
// ---------------------------------------------------------------------

void TestDutyIsReadEverySliceSoInhibitLandsWithinOneSlice() {
  Recorder rec;
  std::vector<bool> last_state(1, false);
  int slice_cursor = 0;

  RenderPwmPeriod(
      /*slices=*/10, /*channels=*/1,
      [&rec](std::size_t) {
        ++rec.duty_reads;
        return rec.duty_reads <= 3 ? 1.0 : 0.0;
      },
      [&rec, &slice_cursor](std::size_t channel, bool on) {
        rec.writes.emplace_back(channel, on);
        rec.write_slices.push_back(slice_cursor);
      },
      []() { return true; },
      [&rec, &slice_cursor]() {
        ++rec.slices_waited;
        ++slice_cursor;
      },
      &last_state);

  // Behaviour first, bookkeeping after: a once-per-period read must fail on
  // the observable outcome (the line never goes low), not merely on a
  // call-count proxy.
  assert(rec.writes.size() == 2);
  assert(rec.writes[0].first == 0 && rec.writes[0].second == true);
  assert(rec.write_slices[0] == 0);
  // The whole point: the line goes low at slice 3, one slice after the duty
  // changed -- not at the end of the period.
  assert(rec.writes[1].first == 0 && rec.writes[1].second == false);
  assert(rec.write_slices[1] == 3);
  assert(last_state[0] == false);
  assert(rec.duty_reads == 10);
  assert(rec.slices_waited == 10);
}

// ---------------------------------------------------------------------
// Steady duty 0.5 over 10 slices:
//   round(0.5 * 10) = 5 -> on for slices 0..4, off for slices 5..9
// => exactly 2 writes (on at slice 0, off at slice 5). Guards the other
// direction of the same change: making the read per-slice must NOT increase
// the GPIO write rate, because writes are still change-gated.
// ---------------------------------------------------------------------

void TestSteadyDutyStillWritesOnlyOnChange() {
  Recorder rec;
  std::vector<bool> last_state(1, false);
  int slice_cursor = 0;

  RenderPwmPeriod(
      /*slices=*/10, /*channels=*/1, [&rec](std::size_t) {
        ++rec.duty_reads;
        return 0.5;
      },
      [&rec, &slice_cursor](std::size_t channel, bool on) {
        rec.writes.emplace_back(channel, on);
        rec.write_slices.push_back(slice_cursor);
      },
      []() { return true; },
      [&slice_cursor]() { ++slice_cursor; }, &last_state);

  assert(rec.duty_reads == 10);
  assert(rec.writes.size() == 2);
  assert(rec.writes[0].second == true && rec.write_slices[0] == 0);
  assert(rec.writes[1].second == false && rec.write_slices[1] == 5);
}

// Duty 0.0 for the whole period: round(0.0 * 10) = 0, so `slice < 0` is
// never true and the line, already low, is never written at all.
void TestZeroDutyNeverWrites() {
  Recorder rec;
  std::vector<bool> last_state(1, false);

  RenderPwmPeriod(
      /*slices=*/10, /*channels=*/1, [&rec](std::size_t) {
        ++rec.duty_reads;
        return 0.0;
      },
      [&rec](std::size_t channel, bool on) {
        rec.writes.emplace_back(channel, on);
      },
      []() { return true; }, []() {}, &last_state);

  assert(rec.duty_reads == 10);
  assert(rec.writes.empty());
  assert(last_state[0] == false);
}

// Shutdown must abort mid-period, exactly as the pre-existing
// `slice < kSlices && running_.load()` guard did: running() goes false after
// four slices, so slices 0..3 render and nothing after them does.
void TestRunningFalseAbortsPeriodEarly() {
  Recorder rec;
  std::vector<bool> last_state(1, false);
  int running_checks = 0;

  RenderPwmPeriod(
      /*slices=*/10, /*channels=*/1, [&rec](std::size_t) {
        ++rec.duty_reads;
        return 1.0;
      },
      [&rec](std::size_t channel, bool on) {
        rec.writes.emplace_back(channel, on);
      },
      [&running_checks]() { return ++running_checks <= 4; },
      [&rec]() { ++rec.slices_waited; }, &last_state);

  assert(rec.duty_reads == 4);
  assert(rec.slices_waited == 4);
  assert(rec.writes.size() == 1);  // on at slice 0, then nothing more
}

// Two channels, independent duties, per-slice reads: channel 0 is inhibited
// mid-period while channel 1 holds a steady 1.0 and must be untouched.
//   channel 0: 1.0 for slices 0..1, then 0.0 -> WRITE on at 0, off at 2
//   channel 1: 1.0 throughout          -> WRITE on at 0 only
void TestPerChannelDutiesAreIndependent() {
  Recorder rec;
  std::vector<bool> last_state(2, false);
  int slice_cursor = 0;

  RenderPwmPeriod(
      /*slices=*/10, /*channels=*/2,
      [&slice_cursor](std::size_t channel) {
        if (channel == 0) return slice_cursor < 2 ? 1.0 : 0.0;
        return 1.0;
      },
      [&rec, &slice_cursor](std::size_t channel, bool on) {
        rec.writes.emplace_back(channel, on);
        rec.write_slices.push_back(slice_cursor);
      },
      []() { return true; }, [&slice_cursor]() { ++slice_cursor; },
      &last_state);

  assert(rec.writes.size() == 3);
  assert(rec.writes[0] == std::make_pair(std::size_t{0}, true));
  assert(rec.write_slices[0] == 0);
  assert(rec.writes[1] == std::make_pair(std::size_t{1}, true));
  assert(rec.write_slices[1] == 0);
  assert(rec.writes[2] == std::make_pair(std::size_t{0}, false));
  assert(rec.write_slices[2] == 2);
  assert(last_state[0] == false);
  assert(last_state[1] == true);
}

}  // namespace

int main() {
  TestDutyIsReadEverySliceSoInhibitLandsWithinOneSlice();
  TestSteadyDutyStillWritesOnlyOnChange();
  TestZeroDutyNeverWrites();
  TestRunningFalseAbortsPeriodEarly();
  TestPerChannelDutiesAreIndependent();
  return 0;
}
