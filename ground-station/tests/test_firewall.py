"""Tests for app.gui.firewall.

On non-Windows hosts `check_and_prompt` must short-circuit to "ok" without
touching subprocess. On Windows we skip entirely — we never mutate the live
firewall from the test suite.
"""
from __future__ import annotations

import platform
import sys
import unittest
from pathlib import Path
from unittest import mock

_GS_ROOT = Path(__file__).resolve().parents[1]
if str(_GS_ROOT) not in sys.path:
    sys.path.insert(0, str(_GS_ROOT))


class FirewallTests(unittest.TestCase):
    @unittest.skipIf(platform.system() == "Windows",
                     "live firewall not exercised on Windows CI")
    def test_check_and_prompt_is_noop_on_non_windows(self) -> None:
        from app.gui import firewall
        with mock.patch("app.gui.firewall.subprocess.run") as run_mock:
            result = firewall.check_and_prompt(parent_widget=None)
        self.assertEqual(result, "ok")
        run_mock.assert_not_called()

    @unittest.skipIf(platform.system() == "Windows",
                     "live firewall not exercised on Windows CI")
    def test_is_windows_helper_returns_false_on_non_windows(self) -> None:
        from app.gui import firewall
        self.assertIs(firewall._is_windows(), False)


if __name__ == "__main__":
    unittest.main()
