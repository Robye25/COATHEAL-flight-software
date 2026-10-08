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
    ACK_RESERVE_BYTES, CAP_BYTES, COMMAND_EXCHANGE_FIXED, EGRESS_MODEL_RATE_PERCENT, GROUND_EGRESS_BURST,
    GROUND_EGRESS_RATE, GROUND_SHARE, LINK_MTU, MAX_REQUEST_BYTES, ONBOARD_EGRESS_BURST,
    ONBOARD_EGRESS_RATE, ONBOARD_SHARE, PURE_ACK, SYN, TCP_MAX_PAYLOAD, UNACCOUNTED, WINDOW_S,
    DiscoveryRounds, LinkBudget, Priority, budget_wait_error, command_budget_wait_s,
    command_exchange_bytes, command_exchange_egress, ground_budget, priority_for, request_too_long,
    send_answer, shaped_ground_budget, tcp_segment, udp_datagram,
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
        self.assertEqual((LINK_MTU, TCP_MAX_PAYLOAD), (576, 524), "one segment at the capped E-Link MTU")
        self.assertEqual(tcp_segment(524), 66 + 524 + 24)
        self.assertEqual(PURE_ACK, 90)
        self.assertEqual(SYN, 98)
        self.assertEqual(udp_datagram(0), 60 + 24, "short datagrams pad to the 60 B minimum frame")
        self.assertEqual(udp_datagram(18), 60 + 24)
        self.assertEqual(udp_datagram(19), 42 + 19 + 24)
        self.assertEqual(udp_datagram(38), 104, "a GS_BEACON line")

    def test_payload_over_one_segment_pays_headers_per_segment(self) -> None:
        self.assertEqual(tcp_segment(525), (66 + 524 + 24) + (66 + 1 + 24))
        self.assertEqual(tcp_segment(2 * 524), 2 * (66 + 524 + 24))
        self.assertEqual(tcp_segment(1145), 1145 + 3 * 90, "a plain DATA line: three segments")

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
        self.assertTrue(ground_budget().egress_shaped, "it models the ground station's kernel shaper")

    def test_hard_cap_buckets_add_up_to_the_cap(self) -> None:
        # Each side's kernel shaper passes at most bucket + rate bytes in any
        # second; together that is the whole 24 kbps.
        self.assertEqual((ONBOARD_EGRESS_BURST, ONBOARD_EGRESS_RATE), (1000, 800))
        self.assertEqual((GROUND_EGRESS_BURST, GROUND_EGRESS_RATE), (700, 500))
        self.assertEqual(ONBOARD_EGRESS_BURST + ONBOARD_EGRESS_RATE + GROUND_EGRESS_BURST + GROUND_EGRESS_RATE,
                         CAP_BYTES)
        # No frame at the capped MTU is larger than a bucket (it would never pass).
        self.assertLessEqual(LINK_MTU + 14 + 24, GROUND_EGRESS_BURST)
        self.assertEqual(command_exchange_egress(5), 98 + 4 * 90 + 5)
        self.assertEqual(ACK_RESERVE_BYTES, 260)


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


class EgressShaperTests(unittest.TestCase):
    """The ledger's model of the kernel shaper on this side's port
    (docs/link-budget.md, "Hard cap")."""

    def setUp(self) -> None:
        self.clock = FakeClock()
        self.budget = LinkBudget(1150, window_s=1.0, clock=self.clock)
        self.budget.set_egress_shaper(700, 500, reserve_bytes=260)

    def test_a_charge_goes_only_while_the_bucket_holds_its_bytes(self) -> None:
        b = self.budget
        self.assertEqual(b.egress_tokens(), 700)
        # 300 of ours: 300 + the 260 reserve are there.
        self.assertIsNotNone(b.try_charge(400, Priority.COMMAND, tx_bytes=300))
        self.assertEqual(b.egress_tokens(), 400)
        # 200 more would leave less than the reserve.
        self.assertIsNone(b.try_charge(200, Priority.COMMAND, tx_bytes=200))
        self.assertEqual(b.in_window(), 400, "a refused charge charges nothing")
        # A safety command does not keep the reserve.
        self.assertIsNotNone(b.try_charge(200, Priority.CRITICAL, tx_bytes=200))
        self.assertEqual(b.egress_tokens(), 200)
        # Bytes the other side sends take no tokens.
        self.assertIsNotNone(b.try_charge(100, Priority.COMMAND))
        self.assertEqual(b.egress_tokens(), 200)
        self.clock.now += 0.5            # 250 B refilled
        self.assertEqual(b.egress_tokens(), 450)
        self.clock.now += 10.0
        self.assertEqual(b.egress_tokens(), 700, "the bucket does not overfill")

    # MUTATION: return 0.0 from LinkBudget._egress_need and confirm
    # test_a_charge_goes_only_while_the_bucket_holds_its_bytes fails.

    def test_what_cannot_wait_is_debited_and_leaves_a_debt(self) -> None:
        b = self.budget
        b.debit_egress(600)
        b.debit_egress(300)              # ACK lines go out whatever the bucket holds
        self.assertEqual(b.egress_tokens(), -200)
        self.assertIsNone(b.try_charge(100, Priority.CRITICAL, tx_bytes=100))
        self.clock.now += 0.6            # 300 B refilled: 100 tokens
        self.assertIsNotNone(b.try_charge(100, Priority.CRITICAL, tx_bytes=100))
        for _ in range(20):
            b.debit_egress(300)
        self.assertEqual(b.egress_tokens(), -700, "the debt is never more than one bucket")
        b.refund_egress(5000)            # never more than the bucket
        self.assertEqual(b.egress_tokens(), 700)

    def test_a_charge_needing_more_than_the_bucket_goes_from_a_full_bucket(self) -> None:
        b = self.budget
        # 473 B of a short command + the 260 reserve > 700: a full bucket does.
        tx = command_exchange_egress(15)
        self.assertIsNotNone(b.try_charge(935, Priority.COMMAND, tx_bytes=tx))
        self.assertEqual(b.egress_tokens(), 700 - tx)
        self.clock.now += 0.5
        self.assertIsNone(b.try_charge(100, Priority.COMMAND, tx_bytes=tx), "not full again yet")

    def test_hold_waits_for_the_tokens(self) -> None:
        # Real clock: nothing calls notify(); the waiter computes the refill.
        b = LinkBudget(1150, window_s=0.05)
        b.set_egress_shaper(700, 2000)
        b.debit_egress(700)
        started = time.monotonic()
        ticket = b.hold(300, Priority.COMMAND, timeout_s=5.0, tx_bytes=300)
        waited = time.monotonic() - started
        self.assertIsNotNone(ticket)
        self.assertGreaterEqual(waited, 0.12)   # 300 B at 2 000 B/s
        self.assertLess(waited, 1.0)

    def test_an_unshaped_ledger_ignores_tx_bytes(self) -> None:
        b = LinkBudget(1000, window_s=1.0, clock=self.clock)
        self.assertFalse(b.egress_shaped)
        self.assertIsNotNone(b.try_charge(900, Priority.COMMAND, tx_bytes=900))
        b.debit_egress(5000)
        self.assertEqual(b.egress_tokens(), 0)

    def test_telemetry_answers_are_debited(self) -> None:
        class Conn:
            sent = b""

            def sendall(self, data: bytes) -> None:
                self.sent += data

        b, conn = self.budget, Conn()
        send_answer(b, conn, "ACK,coatheal-1789498045-582267,123456\n")
        self.assertEqual(conn.sent, b"ACK,coatheal-1789498045-582267,123456\n")
        self.assertEqual(b.egress_tokens(), 700 - tcp_segment(38))
        self.assertEqual(b.in_window(), 0, "the onboard holds an ACK line's bytes on its share")

    def test_the_ground_station_ledger_models_its_own_shaper(self) -> None:
        b = shaped_ground_budget()
        self.assertEqual(b.share_bytes, GROUND_SHARE)
        self.assertEqual(b.egress_tokens(), GROUND_EGRESS_BURST)
        # A full bucket takes one command at once and still answers a frame.
        self.assertIsNotNone(b.try_charge(command_exchange_bytes(15), Priority.COMMAND,
                                          tx_bytes=command_exchange_egress(15)))
        self.assertGreaterEqual(b.egress_tokens(), tcp_segment(40))
        self.assertLess(EGRESS_MODEL_RATE_PERCENT, 100)


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
