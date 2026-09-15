"""Per-verb command timeout table (protocol.COMMAND_TIMEOUTS /
protocol.timeout_for) and its two consumers: the CLI (command_client.py)
and the GUI dispatcher (gui/dispatch.py).

Background: CHECK measured ~3.00s on a healthy Pi with NO hardware
attached (it drives a real DPS310/ADS1115/Sequent-RTD/MAX31865 hardware
conversation -- see onboard/src/sensor_manager.cpp's ActiveCheck), which
races the old 3.0s default timeout used by both the CLI's `--timeout`
default and the GUI's CommandDispatcher.send() default. These tests pin
the fix: CHECK (and "CHECK <component>" variants) resolve to a longer
timeout everywhere a command can be sent, while an explicit timeout still
wins and ordinary verbs are untouched.

Follows the conventions of test_hardware_setup.py (mock.patch.object over
a module function to avoid real I/O) and test_gui_uiux.py (offscreen QPA,
skip cleanly if PyQt6/Qt aren't usable, build a real MainWindow and click
a real button rather than asserting behavior indirectly).
"""
from __future__ import annotations

import argparse
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import command_client
from app.protocol import timeout_for


# ── protocol.timeout_for: pure-function resolution ────────────────────────
class TimeoutForTests(unittest.TestCase):
    def test_check_resolves_to_long_timeout(self) -> None:
        self.assertEqual(timeout_for("CHECK"), 15.0)

    # MUTATION: delete the "CHECK": 15.0 entry from protocol.COMMAND_TIMEOUTS
    # (or change timeout_for to ignore the table) and confirm
    # test_check_resolves_to_long_timeout fails, expecting 15.0 but getting
    # the 3.0 default.

    def test_check_with_component_argument_resolves_to_long_timeout(self) -> None:
        self.assertEqual(timeout_for("CHECK SEQUENT_RTD"), 15.0)
        self.assertEqual(timeout_for("CHECK MAX31865"), 15.0)

    # MUTATION: change timeout_for's lookup key from
    # `command.strip().split()[0].upper()` (the verb only) to
    # `command.strip().upper()` (the whole string) and confirm
    # test_check_with_component_argument_resolves_to_long_timeout fails --
    # "CHECK SEQUENT_RTD" is not a literal key in COMMAND_TIMEOUTS, so
    # lookup would fall through to the 3.0 default.

    def test_unknown_ordinary_verb_resolves_to_default(self) -> None:
        self.assertEqual(timeout_for("PING"), 3.0)
        self.assertEqual(timeout_for("SOME_FUTURE_COMMAND"), 3.0)

    # MUTATION: hardcode timeout_for to always return 15.0 and confirm
    # test_unknown_ordinary_verb_resolves_to_default fails (PING would
    # incorrectly get the long timeout too).


# ── CLI: command_client.py ─────────────────────────────────────────────────
def _parse_command_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers()
    command_client.add_subparser(subparsers)
    return parser.parse_args(argv)


class CliTimeoutTests(unittest.TestCase):
    def test_explicit_timeout_overrides_table(self) -> None:
        args = _parse_command_args(
            ["command", "--host", "127.0.0.1", "--cmd", "CHECK", "--timeout", "5"]
        )
        captured: dict = {}

        def fake_send_command(host, port, command, timeout):
            captured["timeout"] = timeout
            return "ACK,CHECK,overall=OK"

        with mock.patch.object(command_client, "send_command", fake_send_command):
            rc = command_client._handle(args)

        self.assertEqual(rc, 0)
        self.assertEqual(captured["timeout"], 5.0)

    # MUTATION: change `_handle` to always call
    # `send_command(host, args.port, args.cmd, timeout_for(args.cmd))`
    # (ignoring args.timeout) and confirm test_explicit_timeout_overrides_table
    # fails: captured["timeout"] becomes 15.0 instead of the requested 5.0.

    def test_check_without_explicit_timeout_uses_table_value(self) -> None:
        args = _parse_command_args(
            ["command", "--host", "127.0.0.1", "--cmd", "CHECK"]
        )
        self.assertIsNone(args.timeout, "--timeout must default to None, not 3.0, "
                                         "so _handle can tell 'not given' from "
                                         "'given the value 3.0'")
        captured: dict = {}

        def fake_send_command(host, port, command, timeout):
            captured["timeout"] = timeout
            return "ACK,CHECK,overall=OK"

        with mock.patch.object(command_client, "send_command", fake_send_command):
            command_client._handle(args)

        self.assertEqual(captured["timeout"], 15.0)

    # MUTATION: revert `--timeout` to `default=3.0` (instead of None) and
    # confirm test_check_without_explicit_timeout_uses_table_value fails --
    # captured["timeout"] becomes 3.0 because _handle can no longer
    # distinguish "user passed 3.0" from "user passed nothing".


# ── GUI: gui/dispatch.py CommandDispatcher + CommandPanel's CHECK button ──
class _FakePool:
    """Stand-in for QThreadPool.globalInstance() that records jobs instead
    of running them, so the real CommandDispatcher.send() resolution logic
    runs but no socket I/O happens."""

    def __init__(self) -> None:
        self.jobs: list = []

    def start(self, job, priority: int = 0) -> None:
        self.jobs.append(job)


class GuiCheckButtonTimeoutTests(unittest.TestCase):
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

    @staticmethod
    def _find_button(container, text: str):
        from PyQt6.QtWidgets import QPushButton
        for b in container.findChildren(QPushButton):
            if b.text() == text:
                return b
        return None

    def test_check_button_dispatch_uses_resolved_timeout(self) -> None:
        from app.gui.main_window import MainWindow

        win = MainWindow(
            bind="127.0.0.1", tel_port=44300, cmd_port=45300,
            cmd_host="127.0.0.1", log_path=Path("logs/check_timeout.csv"),
            firewall_check=False,
        )
        try:
            fake_pool = _FakePool()
            # Swap the dispatcher's real QThreadPool for a recorder. This
            # leaves CommandDispatcher.send() itself -- including its
            # timeout_for() resolution -- running for real; only the
            # network-touching _SendJob.run() never executes.
            win._dispatcher._pool = fake_pool

            btn = win._system.btn_check
            self.assertIsNotNone(btn, "CHECK button not found")
            win._system.check_target.setCurrentText("ALL")
            btn.click()

            self.assertEqual(len(fake_pool.jobs), 1, "CHECK click should queue exactly one job")
            self.assertEqual(
                fake_pool.jobs[0]._timeout, 15.0,
                "CHECK button's dispatch must resolve to the 15.0s table "
                "entry, not the 3.0s default",
            )
        finally:
            win.close()

    # MUTATION: revert CommandDispatcher.send's signature back to
    # `timeout: float = 3.0` (dropping the timeout_for() resolution) and
    # confirm test_check_button_dispatch_uses_resolved_timeout fails --
    # fake_pool.jobs[0]._timeout becomes 3.0 instead of 15.0.


if __name__ == "__main__":
    unittest.main()
