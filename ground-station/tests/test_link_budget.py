"""The ground station's 24 kbps link budget (docs/link-budget.md): the
on-wire byte model, the sliding-window ledger with priorities, and the
discovery rounds that charge it. No Qt, no sockets; time is a fake clock."""
from __future__ import annotations

import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.link_budget import (  # noqa: E402
    CAP_BYTES, COMMAND_EXCHANGE_FIXED, GROUND_SHARE, MAX_REQUEST_BYTES, ONBOARD_SHARE, PURE_ACK, SYN,
    UNACCOUNTED, WINDOW_S, DiscoveryRounds, LinkBudget, Priority, budget_wait_error,
    command_budget_wait_s, command_exchange_bytes, ground_budget, priority_for, request_too_long,
    tcp_segment, udp_datagram,
)


class FakeClock:
    def __init__(self, now: float = 1000.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now


def wait_until(predicate, timeout_s: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return bool(predicate())


class ByteModelTests(unittest.TestCase):
    def test_spec_table(self) -> None:
        self.assertEqual(tcp_segment(0), 90, "a segment without payload is a pure ACK")
        self.assertEqual(tcp_segment(1), 66 + 1 + 24)
        self.assertEqual(tcp_segment(1448), 66 + 1448 + 24)
        self.assertEqual(PURE_ACK, 90)
        self.assertEqual(SYN, 98)
        self.assertEqual(udp_datagram(0), 60 + 24, "short datagrams pad to the 60 B minimum frame")
        self.assertEqual(udp_datagram(18), 60 + 24)
        self.assertEqual(udp_datagram(19), 42 + 19 + 24)
        self.assertEqual(udp_datagram(38), 104, "a GS_BEACON line")

    def test_payload_over_one_segment_pays_headers_per_segment(self) -> None:
        self.assertEqual(tcp_segment(1449), (66 + 1448 + 24) + (66 + 1 + 24))
        self.assertEqual(tcp_segment(2 * 1448), 2 * (66 + 1448 + 24))
        self.assertEqual(tcp_segment(1145), 1235, "a plain DATA line")

    def test_shares(self) -> None:
        self.assertEqual(CAP_BYTES, 3000)
        self.assertEqual(ONBOARD_SHARE + GROUND_SHARE + UNACCOUNTED, CAP_BYTES)
        self.assertEqual((ONBOARD_SHARE, GROUND_SHARE, UNACCOUNTED), (1600, 1150, 250))
        self.assertEqual(WINDOW_S, 1.0)
        self.assertEqual(COMMAND_EXCHANGE_FIXED, 920)
        self.assertEqual(MAX_REQUEST_BYTES, 230)
        self.assertEqual(command_exchange_bytes(5), 925)

    def test_request_length_and_wait_rules(self) -> None:
        self.assertIsNone(request_too_long(230))
        self.assertEqual(request_too_long(231), "request too long for the 24 kbps link budget (231 > 230 B)")
        self.assertEqual(command_budget_wait_s(3.0), 10.0)
        self.assertEqual(command_budget_wait_s(15.0), 15.0)
        self.assertEqual(budget_wait_error(10.0), "waited 10 s for link budget")

    def test_priorities(self) -> None:
        self.assertLess(Priority.CRITICAL, Priority.COMMAND)
        self.assertLess(Priority.COMMAND, Priority.POLL)
        self.assertLess(Priority.POLL, Priority.DISCOVERY)
        for verb in ("HEATERS_OFF", "STEPPER_STOP 1", "disarm", " SHUTDOWN_SAFE", "RADIO_SILENCE", "RADIO_RESUME"):
            self.assertEqual(priority_for(verb), Priority.CRITICAL, verb)
        self.assertEqual(priority_for("STEPPER_STOP 0", quiet=True), Priority.CRITICAL,
                         "a safety verb stays critical whatever path sends it")
        self.assertEqual(priority_for("MOTOR_DEBUG 0", quiet=True), Priority.POLL)
        for verb in ("ARM", "DISARM_DEBUG", "FALLBACK_DISARM", "STATUS", "", "STEPPER_STOP_X"):
            self.assertEqual(priority_for(verb), Priority.COMMAND, verb)

    def test_one_ledger_per_process(self) -> None:
        self.assertIs(ground_budget(), ground_budget())
        self.assertEqual(ground_budget().share_bytes, GROUND_SHARE)


class LedgerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.budget = LinkBudget(1000, window_s=1.0, clock=self.clock)

    def test_sliding_window(self) -> None:
        b = self.budget
        self.assertIsNotNone(b.try_charge(600, Priority.COMMAND))
        self.clock.now += 0.5
        self.assertIsNotNone(b.try_charge(400, Priority.COMMAND))
        self.assertEqual(b.in_window(), 1000)
        self.assertIsNone(b.try_charge(1, Priority.CRITICAL), "full is full, whatever the priority")
        self.clock.now += 0.5           # the first charge is exactly one window old
        self.assertEqual(b.in_window(), 400)
        self.assertIsNotNone(b.try_charge(600, Priority.DISCOVERY))
        self.assertIsNone(b.try_charge(1, Priority.DISCOVERY))
        self.clock.now += 0.5
        self.assertEqual(b.in_window(), 600)
        self.clock.now += 0.5
        self.assertEqual(b.in_window(), 0)

    # MUTATION: count a charge until `released_at` instead of `released_at +
    # window_s` in LinkBudget._counting and confirm test_sliding_window fails.

    def test_hold_counts_while_open_and_one_window_after_release(self) -> None:
        b = self.budget
        hold = b.hold(900, Priority.COMMAND)
        self.assertIsNotNone(hold)
        self.assertTrue(hold.is_open)
        self.clock.now += 30.0
        self.assertEqual(b.in_window(), 900, "an open hold never ages out")
        self.assertIsNone(b.try_charge(101, Priority.CRITICAL))
        b.release(hold)
        self.assertFalse(hold.is_open)
        self.clock.now += 0.99
        self.assertEqual(b.in_window(), 900)
        b.release(hold)                 # a second release changes nothing
        self.clock.now += 0.01
        self.assertEqual(b.in_window(), 0)
        b.release(None)

    def test_a_delayed_release_counts_until_the_delay_plus_one_window(self) -> None:
        b = self.budget
        hold = b.hold(900, Priority.COMMAND)
        b.release(hold, 0.5)
        self.assertFalse(hold.is_open)
        self.clock.now += 1.49
        self.assertEqual(b.in_window(), 900, "the bytes it covers may still be on their way")
        self.clock.now += 0.01
        self.assertEqual(b.in_window(), 0)

    def test_oversize_request_fails_at_once(self) -> None:
        started = time.monotonic()
        self.assertIsNone(self.budget.hold(1001, Priority.CRITICAL, timeout_s=30.0))
        self.assertIsNone(self.budget.try_charge(1001, Priority.CRITICAL))
        self.assertLess(time.monotonic() - started, 1.0, "a request larger than the share never waits")
        self.assertEqual(self.budget.in_window(), 0)

    def test_waiting_critical_hold_blocks_a_command_that_would_fit(self) -> None:
        b = self.budget
        first = b.hold(900, Priority.COMMAND)
        got: list = []
        waiter = threading.Thread(target=lambda: got.append(b.hold(500, Priority.CRITICAL, timeout_s=60.0)),
                                  daemon=True)
        waiter.start()
        self.assertTrue(wait_until(lambda: b.waiting() == (Priority.CRITICAL,)))
        self.assertIsNone(b.try_charge(50, Priority.COMMAND),
                          "50 B fit, but a CRITICAL hold is queued for them")
        self.assertIsNone(b.hold(50, Priority.POLL))
        # The critical waiter itself does not block an equal priority charge.
        self.assertIsNotNone(b.try_charge(50, Priority.CRITICAL))
        b.release(first)
        self.clock.now += 1.0
        b.notify()
        waiter.join(2.0)
        self.assertFalse(waiter.is_alive())
        self.assertIsNotNone(got[0])
        self.assertEqual(b.waiting(), ())

    # MUTATION: drop the `other.priority < priority` test from
    # LinkBudget._fits and confirm
    # test_waiting_critical_hold_blocks_a_command_that_would_fit fails.

    def test_waiter_wakes_when_charges_age_out(self) -> None:
        # Real clock, short window: nothing calls notify(); the waiter must
        # compute when the charge expires and wake by itself.
        b = LinkBudget(1000, window_s=0.2)
        b.try_charge(800, Priority.COMMAND)
        started = time.monotonic()
        ticket = b.hold(500, Priority.COMMAND, timeout_s=5.0)
        waited = time.monotonic() - started
        self.assertIsNotNone(ticket)
        self.assertGreaterEqual(waited, 0.15)
        self.assertLess(waited, 1.0)

    def test_waiter_wakes_on_release(self) -> None:
        b = LinkBudget(1000, window_s=0.1)
        first = b.hold(800, Priority.COMMAND)
        timer = threading.Timer(0.1, lambda: b.release(first))
        timer.daemon = True
        timer.start()
        started = time.monotonic()
        ticket = b.hold(500, Priority.COMMAND, timeout_s=5.0)
        self.assertIsNotNone(ticket)
        self.assertLess(time.monotonic() - started, 1.0)
        timer.join()

    def test_hold_times_out_and_leaves_the_queue(self) -> None:
        b = LinkBudget(1000, window_s=1.0)
        b.hold(800, Priority.COMMAND)
        started = time.monotonic()
        self.assertIsNone(b.hold(500, Priority.CRITICAL, timeout_s=0.1))
        self.assertGreaterEqual(time.monotonic() - started, 0.09)
        self.assertEqual(b.waiting(), ())
        self.assertIsNotNone(b.try_charge(100, Priority.DISCOVERY), "a timed-out waiter blocks nobody")

    def test_cancel_ends_the_wait(self) -> None:
        b = LinkBudget(1000, window_s=1.0)
        b.hold(800, Priority.COMMAND)
        cancel = threading.Event()
        timer = threading.Timer(0.05, cancel.set)
        timer.daemon = True
        timer.start()
        started = time.monotonic()
        self.assertIsNone(b.hold(500, Priority.COMMAND, timeout_s=30.0, cancel=cancel))
        self.assertLess(time.monotonic() - started, 1.0)

    def test_same_priority_holds_are_served_in_arrival_order(self) -> None:
        # Commands keep the operator's order: a later hold that would fit
        # does not overtake an earlier one of the same priority.
        b = self.budget
        first = b.hold(900, Priority.COMMAND)
        got: dict = {}

        def wait(name: str, size: int) -> None:
            got[name] = b.hold(size, Priority.COMMAND, timeout_s=60.0)

        early = threading.Thread(target=wait, args=("early", 900), daemon=True)
        early.start()
        self.assertTrue(wait_until(lambda: len(b.waiting()) == 1))
        late = threading.Thread(target=wait, args=("late", 50), daemon=True)
        late.start()
        self.assertTrue(wait_until(lambda: len(b.waiting()) == 2),
                        "50 B fit next to the open hold, but an earlier hold is queued for the room")
        self.assertIsNone(b.hold(50, Priority.COMMAND), "a new hold queues behind both")
        self.assertIsNotNone(b.try_charge(50, Priority.COMMAND),
                             "a plain charge yields only to waiting holds of higher priority")
        b.release(first)
        self.clock.now += 1.0
        b.notify()
        early.join(2.0); late.join(2.0)
        self.assertFalse(early.is_alive() or late.is_alive())
        self.assertIsNotNone(got["early"])
        self.assertIsNotNone(got["late"])
        self.assertEqual(b.in_window(), 950)

    # MUTATION: drop the same-priority `other.order < waiter.order` test from
    # LinkBudget._fits and confirm test_same_priority_holds_are_served_in_arrival_order
    # fails on "an earlier hold is queued for the room".


class DiscoveryRoundsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.budget = LinkBudget(1150, clock=self.clock)
        self.rounds = DiscoveryRounds(self.budget)
        self.sent: list = []

    def send(self, data: bytes, address) -> None:
        self.sent.append((self.clock.now, data, address))

    def test_cadence_follows_the_link(self) -> None:
        r = self.rounds
        self.assertTrue(r.due(self.clock.now, healthy=False), "the first round goes at once")
        r.start(self.clock.now, False, [])
        self.assertFalse(r.due(self.clock.now + 1.9, healthy=False))
        self.assertTrue(r.due(self.clock.now + 2.0, healthy=False))
        self.assertFalse(r.due(self.clock.now + 14.9, healthy=True))
        self.assertTrue(r.due(self.clock.now + 15.0, healthy=True))
        self.assertAlmostEqual(r.next_check_s(self.clock.now + 0.5, healthy=False), 1.5)
        self.assertAlmostEqual(r.next_check_s(self.clock.now + 0.5, healthy=True), 14.5)

    def test_datagrams_are_charged_and_skipped_while_the_ledger_is_full(self) -> None:
        r = self.rounds
        line = b"GS_BEACON,1789500000000,4000,5000,100\n"       # 104 B on the wire
        hold = self.budget.hold(1000, Priority.COMMAND)
        r.start(self.clock.now, False, [(line, "a"), (line, "b"), (line, "c")])
        r.send_pending(False, self.send)
        self.assertEqual(len(self.sent), 1, "150 B left: one 104 B datagram fits")
        self.assertEqual(r.pending, 2)
        self.assertEqual(r.skipped, 2)
        self.assertEqual(self.budget.in_window(), 1104)
        self.assertGreater(r.next_check_s(self.clock.now, False), 0)
        self.assertLessEqual(r.next_check_s(self.clock.now, False), DiscoveryRounds.RETRY_S)
        # Still full at the next check: nothing more goes out.
        self.clock.now += 0.25
        r.send_pending(False, self.send)
        self.assertEqual(len(self.sent), 1)
        self.budget.release(hold)
        self.clock.now += 1.0
        r.send_pending(False, self.send)
        self.assertEqual([address for _t, _d, address in self.sent], ["a", "b", "c"])
        self.assertEqual(r.pending, 0)

    # MUTATION: in DiscoveryRounds.send_pending, send without the try_charge
    # and confirm test_datagrams_are_charged_and_skipped_while_the_ledger_is_full fails.

    def test_a_new_round_or_a_link_change_drops_what_is_left(self) -> None:
        r = self.rounds
        self.budget.hold(1150, Priority.COMMAND)
        r.start(self.clock.now, False, [(b"GS_HELLO,1,4000,5000\n", "a")])
        r.send_pending(False, self.send)
        self.assertEqual(r.pending, 1)
        r.send_pending(True, self.send)
        self.assertEqual(r.pending, 0, "no legacy hello once telemetry arrives")
        r.start(self.clock.now, False, [(b"GS_HELLO,2,4000,5000\n", "a")])
        r.start(self.clock.now + 2.0, False, [(b"GS_HELLO,3,4000,5000\n", "b")])
        self.assertEqual(r.pending, 1)
        r.clear()
        self.assertEqual(r.pending, 0)
        self.assertEqual(self.sent, [])


if __name__ == "__main__":
    unittest.main()
