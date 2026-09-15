"""Command exchanges paced by the ground station's link budget
(docs/link-budget.md): the GUI dispatcher's send jobs and the CLI hold
`920 + len(request line)` from before connecting until just after the socket
is closed (longer, with a reset, when the exchange fails), refuse a request
line that can never fit, and fail when the budget stays full. The fake
onboard is a loopback command server."""
from __future__ import annotations

import os
import socket
import sys
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import command_client  # noqa: E402
from app.link_budget import (  # noqa: E402
    ABORT_TAIL_S, CLOSE_TAIL_S, GROUND_SHARE, LinkBudget, Priority, ground_budget, udp_datagram,
)


class FakeCommandServer:
    """Answers every request line with `ACK,<verb>,ok` and records when each
    request arrived and when each client closed."""

    def __init__(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(8)
        self._sock.settimeout(0.1)
        self.port = self._sock.getsockname()[1]
        self.accepts = 0
        self.requests: list = []    # (monotonic, line)
        self.closes: list = []      # monotonic
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                conn, _addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self.accepts += 1
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn: socket.socket) -> None:
        with conn:
            conn.settimeout(5.0)
            data = b""
            try:
                while b"\n" not in data:
                    chunk = conn.recv(1024)
                    if not chunk:
                        return
                    data += chunk
                line = data.split(b"\n", 1)[0].decode("utf-8")
                with self._lock:
                    self.requests.append((time.monotonic(), line))
                conn.sendall(f"ACK,{line.split()[0]},ok\n".encode("utf-8"))
                while conn.recv(1024):
                    pass
            except OSError:
                return
            with self._lock:
                self.closes.append(time.monotonic())

    def close(self) -> None:
        self._stop.set()
        self._thread.join(2.0)
        self._sock.close()


class FakeClock:
    def __init__(self, now: float = 50.0) -> None:
        self.now = now

    def __call__(self) -> float:
        return self.now


def wait_until(predicate, timeout_s: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return bool(predicate())


class _FakePool:
    def __init__(self) -> None:
        self.jobs: list = []
        self.priorities: list = []

    def start(self, job, priority: int = 0) -> None:
        self.jobs.append(job)
        self.priorities.append(priority)


class SendJobPacingTests(unittest.TestCase):
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

    def setUp(self) -> None:
        self.server = FakeCommandServer()
        self.results: list = []

    def tearDown(self) -> None:
        self.server.close()

    def job(self, command: str, budget: LinkBudget, timeout: float = 3.0, priority=Priority.COMMAND):
        from app.gui.dispatch import _SendJob
        return _SendJob("127.0.0.1", self.server.port, command, timeout, None,
                        lambda cmd, resp, ms, tag: self.results.append((cmd, resp, ms)),
                        priority=priority, budget=budget)

    def test_two_commands_are_serialized_by_the_ledger(self) -> None:
        window = 0.3
        budget = LinkBudget(GROUND_SHARE, window_s=window)
        threads = [threading.Thread(target=self.job(cmd, budget).run, daemon=True)
                   for cmd in ("STATUS", "GET_THERMAL")]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(5.0)
        self.assertEqual(len(self.results), 2)
        self.assertTrue(all(resp.ok for _cmd, resp, _ms in self.results), self.results)
        self.assertEqual(len(self.server.requests), 2)
        (first_request, _), (second_request, _) = sorted(self.server.requests)
        first_close = min(self.server.closes)
        self.assertLess(first_request, first_close)
        self.assertGreaterEqual(second_request - first_close, window - 0.05,
                                "the second exchange may start only one window after the first closed")
        self.assertGreaterEqual(max(ms for _cmd, _resp, ms in self.results), (window - 0.05) * 1000.0,
                                "the reported latency includes the wait for the budget")
        self.assertEqual(budget.waiting(), ())

    # MUTATION: skip the `self._budget.hold(...)` in _SendJob._exchange
    # (use a dummy ticket) and confirm test_two_commands_are_serialized_by_the_ledger
    # fails: both requests arrive together.

    def test_hold_covers_the_request_line_and_is_released(self) -> None:
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        self.job("STEPPER_STOP 1", budget).run()
        _cmd, resp, _ms = self.results[0]
        self.assertTrue(resp.ok, resp)
        self.assertEqual(budget.in_window(), 920 + len(b"STEPPER_STOP 1\n"),
                         "released just after the close, still counting for one window")
        clock.now += 1.0
        self.assertEqual(budget.in_window(), 920 + len(b"STEPPER_STOP 1\n"),
                         "the onboard's closing FIN or ACK may still be on its way at the close")
        clock.now += CLOSE_TAIL_S + 0.01
        self.assertEqual(budget.in_window(), 0, "a hold left open would never age out")

    # MUTATION: pass delay 0 to budget.release in paced_connection and confirm
    # the "closing FIN or ACK" assertion above fails.

    def test_a_failed_exchange_is_reset_and_held_for_the_abort_tail(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        ending: dict = {}

        def silent_onboard() -> None:
            conn, _addr = listener.accept()
            with conn:
                conn.settimeout(5.0)
                try:
                    while conn.recv(1024):
                        pass
                    ending["how"] = "fin"
                except ConnectionResetError:
                    ending["how"] = "reset"
                except OSError as exc:
                    ending["how"] = repr(exc)

        thread = threading.Thread(target=silent_onboard, daemon=True)
        thread.start()
        from app.gui.dispatch import _SendJob
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        _SendJob("127.0.0.1", listener.getsockname()[1], "STATUS", 0.3, None,
                 lambda cmd, resp, ms, tag: self.results.append((cmd, resp, ms)), budget=budget).run()
        thread.join(3.0)
        listener.close()
        self.assertFalse(self.results[0][1].ok)
        self.assertEqual(ending.get("how"), "reset", "a reply the onboard sends later must find no connection")
        clock.now += 1.0 + ABORT_TAIL_S - 0.01
        self.assertEqual(budget.in_window(), 920 + len(b"STATUS\n"),
                         "segments crossing the reset still draw resets from our kernel")
        clock.now += 0.02
        self.assertEqual(budget.in_window(), 0)

    def test_request_longer_than_230_bytes_fails_locally(self) -> None:
        budget = LinkBudget(GROUND_SHARE)
        command = "BENDSEQ_LOAD 0 long " + " ".join(["800:5:50"] * 30)
        length = len(command) + 1
        self.assertGreater(length, 230)
        self.job(command, budget).run()
        _cmd, resp, _ms = self.results[0]
        self.assertFalse(resp.ok)
        self.assertEqual(resp.error, f"request too long for the 24 kbps link budget ({length} > 230 B)")
        time.sleep(0.1)
        self.assertEqual(self.server.accepts, 0, "a refused command never touches the network")
        self.assertEqual(budget.in_window(), 0)
        # 230 B including the newline still goes.
        self.results.clear()
        self.job("STATUS " + "x" * 222, budget).run()
        self.assertEqual(self.results[0][1].error, "")
        self.assertEqual(self.server.accepts, 1)

    def test_a_budget_that_stays_full_fails_the_command(self) -> None:
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        budget.hold(GROUND_SHARE, Priority.CRITICAL)
        thread = threading.Thread(target=self.job("STATUS", budget, timeout=3.0).run, daemon=True)
        thread.start()
        self.assertTrue(wait_until(lambda: budget.waiting() == (Priority.COMMAND,)))
        clock.now += 9.9
        budget.notify()
        time.sleep(0.05)
        self.assertEqual(self.results, [], "still waiting before max(10 s, timeout)")
        clock.now += 0.2
        budget.notify()
        thread.join(3.0)
        _cmd, resp, _ms = self.results[0]
        self.assertFalse(resp.ok)
        self.assertEqual(resp.error, "waited 10 s for link budget")
        self.assertEqual(self.server.accepts, 0)


class DispatcherPriorityTests(unittest.TestCase):
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

    def _dispatcher(self, **kw):
        from app.gui.dispatch import CommandDispatcher
        d = CommandDispatcher("127.0.0.1", 5000, **kw)
        d._pool = _FakePool()
        return d

    def test_priorities_follow_the_verb_and_the_quiet_path(self) -> None:
        d = self._dispatcher()
        self.assertIs(d._budget, ground_budget(), "the dispatcher charges the process-wide ledger")
        d.send("MOTOR_DEBUG 0", tag="debug", quiet=True)
        d.send("HEATERS_OFF")
        d.send("STATUS")
        d.send("STEPPER_STOP 0", quiet=True)
        jobs = d._pool.jobs
        self.assertEqual([job._priority for job in jobs],
                         [Priority.POLL, Priority.CRITICAL, Priority.COMMAND, Priority.CRITICAL])
        self.assertTrue(all(job._budget is d._budget for job in jobs))
        self.assertEqual(d._pool.priorities, [1, 3, 2, 3],
                         "jobs waiting for a pool thread leave safety first")

    # MUTATION: pass `quiet=False` to priority_for in CommandDispatcher.send
    # and confirm test_priorities_follow_the_verb_and_the_quiet_path fails.

    def test_a_quiet_poll_is_not_queued_behind_its_own_previous_poll(self) -> None:
        from app.protocol import CommandResponse
        own = LinkBudget(GROUND_SHARE)
        d = self._dispatcher(budget=own)
        self.assertIs(d._budget, own)
        tag = object()
        self.assertTrue(d.send("MOTOR_DEBUG 0", tag=tag, quiet=True))
        self.assertFalse(d.send("MOTOR_DEBUG 0", tag=tag, quiet=True), "the first poll has not come back")
        self.assertTrue(d.send("MOTOR_DEBUG 1", tag=tag, quiet=True), "another motor is another poll")
        self.assertTrue(d.send("STATUS", tag=tag), "operator commands are never dropped")
        self.assertTrue(d.send("STATUS", tag=tag))
        self.assertEqual(len(d._pool.jobs), 4)
        d.quiet_response.emit("MOTOR_DEBUG 0", CommandResponse(ok=True, command="MOTOR_DEBUG"), 3.0, tag)
        self.assertTrue(d.send("MOTOR_DEBUG 0", tag=tag, quiet=True), "the reply frees the next poll")
        self.assertEqual(len(d._pool.jobs), 5)

    def test_closing_ends_commands_waiting_for_the_budget(self) -> None:
        from PyQt6.QtCore import Qt
        from app.gui.dispatch import CLOSING_ERROR

        class ThreadPool:
            def start(self, job, priority: int = 0) -> None:
                threading.Thread(target=job.run, daemon=True).start()

        budget = LinkBudget(GROUND_SHARE)
        budget.hold(GROUND_SHARE, Priority.CRITICAL)
        d = self._dispatcher(budget=budget)
        d._pool = ThreadPool()
        replies: list = []
        d.response_received.connect(lambda cmd, resp, ms, tag: replies.append(resp),
                                    Qt.ConnectionType.DirectConnection)
        d.send("STATUS")
        self.assertTrue(wait_until(lambda: budget.waiting() == (Priority.COMMAND,)))
        started = time.monotonic()
        d.close()
        self.assertTrue(wait_until(lambda: len(replies) == 1, timeout_s=2.0))
        self.assertLess(time.monotonic() - started, 1.0, "not the 10 s budget wait")
        self.assertEqual(replies[0].error, CLOSING_ERROR)
        self.assertEqual(budget.waiting(), ())


class CliPacingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FakeCommandServer()

    def tearDown(self) -> None:
        self.server.close()

    def test_send_command_holds_the_exchange(self) -> None:
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        reply = command_client.send_command("127.0.0.1", self.server.port, "PING", 3.0, budget=budget)
        self.assertEqual(reply, "ACK,PING,ok")
        self.assertEqual(budget.in_window(), 920 + 5)
        clock.now += 1.0 + CLOSE_TAIL_S + 0.01
        self.assertEqual(budget.in_window(), 0)

    def test_cli_refuses_a_request_that_cannot_fit(self) -> None:
        long_command = "SET_PID ALL " + "1" * 230
        with self.assertRaises(command_client.LinkBudgetRefusal) as caught:
            command_client.send_command("127.0.0.1", self.server.port, long_command, 3.0,
                                        budget=LinkBudget(GROUND_SHARE))
        self.assertIn("request too long for the 24 kbps link budget", str(caught.exception))
        parser = __import__("argparse").ArgumentParser()
        command_client.add_subparser(parser.add_subparsers())
        args = parser.parse_args(["command", "--host", "127.0.0.1", "--port", str(self.server.port),
                                  "--cmd", long_command])
        with mock.patch("builtins.print") as printed:
            self.assertEqual(command_client._handle(args), 1)
        self.assertIn("not sent", printed.call_args[0][0])
        self.assertEqual(self.server.accepts, 0)

    def test_discovery_hello_is_charged_per_datagram(self) -> None:
        sent: list = []

        class FakeUdp:
            def __init__(self, *_args) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_exc) -> bool:
                return False

            def setsockopt(self, *_args) -> None:
                pass

            def settimeout(self, _timeout) -> None:
                pass

            def sendto(self, data, address) -> None:
                sent.append((data, address))

            def recvfrom(self, _size):
                raise socket.timeout()

        fake_socket = types.SimpleNamespace(
            socket=FakeUdp, AF_INET=socket.AF_INET, SOCK_DGRAM=socket.SOCK_DGRAM,
            SOL_SOCKET=socket.SOL_SOCKET, SO_BROADCAST=socket.SO_BROADCAST, timeout=socket.timeout)
        clock = FakeClock()
        budget = LinkBudget(GROUND_SHARE, clock=clock)
        with mock.patch.object(command_client, "socket", fake_socket):
            self.assertIsNone(command_client.discover_onboard_host(4100, 5000, 0.05, budget=budget))
            self.assertEqual(len(sent), 2)
            self.assertEqual(budget.in_window(), sum(udp_datagram(len(data)) for data, _addr in sent))
            # Room for one more datagram only.
            clock.now += 1.0
            budget.hold(GROUND_SHARE - udp_datagram(len(sent[0][0])), Priority.COMMAND)
            command_client.discover_onboard_host(4100, 5000, 0.05, budget=budget)
            self.assertEqual(len(sent), 3, "the datagram the budget cannot take is skipped")


if __name__ == "__main__":
    unittest.main()
