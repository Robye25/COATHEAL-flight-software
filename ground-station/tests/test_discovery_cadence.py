"""Discovery follows the link (docs/link-budget.md, "Discovery cadence"):
GS_BEACON with the legacy GS_HELLO every 2 s and the PING probe while no
telemetry arrives; GS_BEACON alone every 15 s and no probe while it does.
Every datagram and probe is charged to the link budget and skipped, never
waited for, when it does not fit. Beacons go to a fake socket; the probe
only ever connects to loopback."""
from __future__ import annotations

import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from app.link_budget import ABORT_TAIL_S, CLOSE_TAIL_S, GROUND_SHARE, LinkBudget, Priority, udp_datagram  # noqa: E402

TARGETS = ["10.99.0.255", "169.254.10.10"]


class FakeClock:
    def __init__(self, now: float = 100.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now


class FakeSocket:
    def __init__(self, clock: FakeClock) -> None:
        self.clock = clock
        self.sent: list = []   # (time, kind, address)

    def sendto(self, data: bytes, address) -> None:
        self.sent.append((self.clock.now, data.split(b",", 1)[0].decode(), address[0]))

    def kinds(self, since: float = float("-inf")) -> list:
        return [kind for t, kind, _addr in self.sent if t >= since]


class QtTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            from PyQt6.QtWidgets import QApplication
        except Exception as exc:
            raise unittest.SkipTest(f"PyQt6 unavailable: {exc}")
        try:
            cls._app = QApplication.instance() or QApplication([])
        except Exception as exc:
            raise unittest.SkipTest(f"no Qt platform: {exc}")


class BeaconCadenceTests(QtTestCase):
    def setUp(self) -> None:
        from app.gui.discovery import GsBeacon, SentNonceRegistry
        self.clock = FakeClock()
        self.budget = LinkBudget(GROUND_SHARE, clock=self.clock)
        self.nonces = SentNonceRegistry()
        self.beacon = GsBeacon(tel_port=4000, cmd_port=5000, sent_nonces=self.nonces, budget=self.budget)
        self.sock = FakeSocket(self.clock)
        patcher = mock.patch("app.gui.discovery._discovery_targets", return_value=list(TARGETS))
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_for(self, seconds: float, step: float = 0.5) -> None:
        end = self.clock.now + seconds
        while self.clock.now <= end + 1e-9:
            self.beacon.poll(self.sock, self.clock.now)
            self.clock.now += step

    def test_no_telemetry_beacons_and_hellos_every_2_s(self) -> None:
        start = self.clock.now
        self.run_for(5.9)
        rounds = sorted({t for t, _kind, _addr in self.sock.sent})
        self.assertEqual([t - start for t in rounds], [0.0, 2.0, 4.0])
        self.assertEqual(self.sock.kinds(), ["GS_BEACON", "GS_BEACON", "GS_HELLO", "GS_HELLO"] * 3)
        self.assertEqual([addr for _t, _k, addr in self.sock.sent[:2]], TARGETS, "once per target address")
        self.assertEqual((self.beacon.beacons_sent, self.beacon.hellos_sent), (6, 6))

    def test_telemetry_arriving_beacons_every_15_s_without_hellos(self) -> None:
        self.beacon.set_link_healthy(True)
        start = self.clock.now
        self.run_for(31.0)
        rounds = sorted({t for t, _kind, _addr in self.sock.sent})
        self.assertEqual([t - start for t in rounds], [0.0, 15.0, 30.0])
        self.assertEqual(set(self.sock.kinds()), {"GS_BEACON"}, "no legacy GS_HELLO while the link is up")
        self.assertEqual(self.beacon.hellos_sent, 0)

    # MUTATION: make GsBeacon.poll ignore `healthy` (always the 2 s round with
    # GS_HELLO) and confirm test_telemetry_arriving_beacons_every_15_s_without_hellos fails.

    def test_losing_the_link_brings_the_2_s_cadence_back(self) -> None:
        self.beacon.set_link_healthy(True)
        self.run_for(1.0)
        self.assertEqual(len(self.sock.sent), 2)
        self.beacon.set_link_healthy(False)      # 1.5 s after the last round: not due yet
        lost = self.clock.now
        self.run_for(3.0)
        self.assertEqual(sorted({t for t, _k, _a in self.sock.sent if t >= lost})[0], lost + 0.5,
                         "the next round is 2 s after the last one, not 15 s")
        self.assertIn("GS_HELLO", self.sock.kinds(since=lost))

    def test_every_datagram_is_charged_and_skipped_when_the_budget_is_full(self) -> None:
        self.beacon.poll(self.sock, self.clock.now)
        self.assertEqual(self.budget.in_window(), sum(udp_datagram(len(line)) for line in self.lines_sent()))
        self.clock.now += 2.0
        hold = self.budget.hold(GROUND_SHARE, Priority.COMMAND)
        self.beacon.poll(self.sock, self.clock.now)
        self.assertEqual(len(self.sock.sent), 4, "nothing goes out while a command holds the share")
        self.assertEqual(self.beacon.datagrams_skipped, 4)
        self.budget.release(hold)
        self.clock.now += 1.0                     # the command's window has passed
        self.beacon.poll(self.sock, self.clock.now)
        self.assertEqual(len(self.sock.sent), 8, "the round's datagrams go out once the budget frees")

    def lines_sent(self) -> list:
        # The beacon's datagrams of the first round, rebuilt from the nonce it registered.
        nonce = self.nonces._order[-1]
        beacon = f"GS_BEACON,{nonce},4000,5000,100\n".encode()
        hello = f"GS_HELLO,{nonce},4000,5000\n".encode()
        return [beacon] * len(TARGETS) + [hello] * len(TARGETS)

    def test_a_waiting_command_blocks_beacons_that_would_fit(self) -> None:
        hold = self.budget.hold(900, Priority.COMMAND)
        waiter = threading.Thread(target=lambda: self.budget.hold(900, Priority.CRITICAL, timeout_s=60.0),
                                  daemon=True)
        waiter.start()
        deadline = time.monotonic() + 2.0
        while self.budget.waiting() == () and time.monotonic() < deadline:
            time.sleep(0.005)
        self.beacon.poll(self.sock, self.clock.now)
        self.assertEqual(self.sock.sent, [], "a queued safety command owns the free bytes")
        self.budget.release(hold)
        self.clock.now += 1.0
        self.budget.notify()
        waiter.join(2.0)

    def test_radio_silence_still_sends_nothing(self) -> None:
        self.beacon.set_quiet(True)
        self.run_for(20.0)
        self.assertEqual(self.sock.sent, [])
        self.beacon.set_quiet(False)
        self.beacon.poll(self.sock, self.clock.now)
        self.assertEqual(len(self.sock.sent), 4, "discovery is back at once after RADIO_RESUME")


class ProbeCadenceTests(QtTestCase):
    def test_no_probe_while_telemetry_arrives(self) -> None:
        from app.gui.discovery import CommandProbe
        probe = CommandProbe(["127.0.0.1"], cmd_port=1, interval_s=0.05, timeout_s=0.05,
                             include_static=False, budget=LinkBudget(GROUND_SHARE))
        probe.set_link_healthy(True)
        probe.start()
        time.sleep(0.3)
        self.assertEqual(probe.probes_sent, 0, "the onboard is found: no PING")
        probe.set_link_healthy(False)
        deadline = time.monotonic() + 3.0
        while probe.probes_sent == 0 and time.monotonic() < deadline:
            time.sleep(0.02)
        probe.stop()
        probe.wait(2000)
        self.assertGreater(probe.probes_sent, 0, "probing resumes when telemetry stops")

    # MUTATION: remove `or self._healthy.is_set()` from CommandProbe.run and
    # confirm test_no_probe_while_telemetry_arrives fails on "no PING".

    def test_probe_is_charged_and_skipped_not_waited_for(self) -> None:
        from PyQt6.QtCore import Qt
        from app.gui.discovery import CommandProbe
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.bind(("127.0.0.1", 0))
        server.listen(4)
        port = server.getsockname()[1]

        def answer() -> None:
            conn, _addr = server.accept()
            with conn:
                conn.recv(64)
                conn.sendall(b"ACK,PING,pong\n")

        responder = threading.Thread(target=answer, daemon=True)
        responder.start()
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        # 127.0.0.2 is loopback too but nothing listens there: refused at once.
        probe = CommandProbe(["127.0.0.2", "127.0.0.1"], cmd_port=port, timeout_s=1.0,
                             include_static=False, budget=budget)
        reachable: list = []
        probe.onboard_reachable.connect(lambda host, p: reachable.append(host), Qt.ConnectionType.DirectConnection)
        try:
            started = time.monotonic()
            self.assertFalse(probe.probe_round(), "the second probe does not fit beside the first")
            self.assertLess(time.monotonic() - started, 1.0, "never waits for the budget")
            self.assertEqual((probe.probes_sent, probe.budget_skips), (1, 1))
            self.assertEqual(budget.in_window(), 920 + len(b"PING\n"))
            self.assertFalse(probe.probe_round(), "still inside the first probe's window")
            # A refused connection may still see a late SYN-ACK: abort tail.
            clock.now += 1.0 + ABORT_TAIL_S
            self.assertTrue(probe.probe_round())
            self.assertEqual(reachable, ["127.0.0.1"], "the round resumed with the host it skipped")
            self.assertEqual(probe.probes_sent, 2)
            clock.now += 1.0 + CLOSE_TAIL_S + 0.01
            self.assertIsNotNone(budget.hold(GROUND_SHARE, Priority.POLL))
            self.assertFalse(probe.probe_round())
            self.assertEqual(probe.probes_sent, 2, "a full budget opens no connection")
        finally:
            responder.join(2.0)
            server.close()


class WindowCadenceTests(QtTestCase):
    def test_window_switches_discovery_with_the_link(self) -> None:
        from gui_helpers import frame, make_window
        from app.link_budget import LINK_HEALTHY_S
        from app.protocol import CommandResponse, parse_telemetry_csv
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch("app.gui.discovery._discovery_targets", return_value=["127.0.0.1"]):
            win = make_window(Path(tmp))
            try:
                workers = (win._beacon, win._probe)
                self.assertEqual([w.is_link_healthy() for w in workers], [False, False])
                win._dispatcher.send = lambda *a, **kw: True
                win._on_packet(parse_telemetry_csv(frame()))
                self.assertEqual([w.is_link_healthy() for w in workers], [True, True])
                win._tick()
                self.assertEqual([w.is_link_healthy() for w in workers], [True, True])
                real = time.monotonic
                with mock.patch("app.gui.main_window.time.monotonic",
                                side_effect=lambda: real() + LINK_HEALTHY_S + 0.5):
                    win._tick()
                self.assertEqual([w.is_link_healthy() for w in workers], [False, False],
                                 "5 s without a frame: beacon and probe every 2 s again")
                # Radio silence works as before, whatever the link.
                win._dispatcher.response_received.emit(
                    "RADIO_SILENCE", CommandResponse(ok=True, command="RADIO_SILENCE", body="radio silent",
                                                     raw="ACK,RADIO_SILENCE,radio silent"), 5.0, None)
                self.assertEqual([w.is_quiet() for w in workers], [True, True])
                win._on_packet(parse_telemetry_csv(frame(seq=2)))
                self.assertEqual([w.is_quiet() for w in workers], [True, True])
                self.assertEqual([w.is_link_healthy() for w in workers], [True, True])
            finally:
                win.close()

    # MUTATION: delete the `_update_discovery_cadence()` call from
    # MainWindow._on_packet and confirm test_window_switches_discovery_with_the_link fails.


class ServerDiscoveryTests(unittest.TestCase):
    def test_server_rounds_follow_the_same_rules(self) -> None:
        from app.link_budget import DiscoveryRounds
        from app.telemetry_server import TelemetryServer
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            server = TelemetryServer(
                bind="127.0.0.1", port=4000, log_root=tmp / "logs", plot=False, alert_temp_c=80.0,
                timeout_s=5.0, discovery_enabled=False, discovery_port=4100, command_port=5000,
                cursor_path=tmp / "cursor.json", discovered_path=tmp / "discovered.json")
            try:
                self.assertFalse(server._link_healthy())
                clock = FakeClock()
                rounds = DiscoveryRounds(LinkBudget(GROUND_SHARE, clock=clock))
                sent: list = []
                server._discovery_round(rounds, clock.now, healthy=False)
                rounds.send_pending(False, lambda data, target: sent.append((data.split(b",")[0], target)))
                self.assertEqual(sent, [(b"GS_HELLO", "255.255.255.255"), (b"GS_HELLO", "169.254.10.10"),
                                        (b"GS_BEACON", "255.255.255.255"), (b"GS_BEACON", "169.254.10.10")])
                self.assertFalse(rounds.due(clock.now + 1.9, healthy=False))
                server._last_packet_time = time.time()
                self.assertTrue(server._link_healthy())
                sent.clear()
                self.assertFalse(rounds.due(clock.now + 14.0, healthy=True))
                clock.now += 15.0
                self.assertTrue(rounds.due(clock.now, healthy=True))
                server._discovery_round(rounds, clock.now, healthy=True)
                rounds.send_pending(True, lambda data, target: sent.append((data.split(b",")[0], target)))
                self.assertEqual([kind for kind, _t in sent], [b"GS_BEACON", b"GS_BEACON"])
                server._last_packet_time = time.time() - 6.0
                self.assertFalse(server._link_healthy())
            finally:
                server.logs.close()


if __name__ == "__main__":
    unittest.main()
