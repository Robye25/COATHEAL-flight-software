"""Windows firewall + network-profile auto-configure for the GS.

On non-Windows hosts this module is a no-op; `check_and_prompt` returns
``"ok"`` immediately. On Windows it checks whether the two inbound firewall
rules we need exist, and — if not — prompts the operator to run an elevated
PowerShell helper (``scripts/configure_firewall.ps1``). The user's decision
(accept / decline) is remembered for the session so we don't nag repeatedly.

Return values from :func:`check_and_prompt`:

* ``"ok"``       – rules in place (or non-Windows host, or user previously
                   accepted and helper reported success).
* ``"deferred"`` – user declined elevation for this session.
* ``"failed"``   – helper ran but exited non-zero, or elevation itself
                   failed (e.g. user hit "No" on the UAC prompt).
"""
from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional, Tuple

try:
    from PyQt6.QtWidgets import QMessageBox
except Exception:  # pragma: no cover - PyQt is a runtime dep
    QMessageBox = None  # type: ignore[assignment]


_TELEMETRY_RULE = "COATHEAL Telemetry"
_DISCOVERY_RULE = "COATHEAL Discovery"
_TELEMETRY_PORT = 4000
_DISCOVERY_PORT = 4100

# Windows constant for subprocess: suppress the flashing console window.
_CREATE_NO_WINDOW = 0x08000000

# Per-session memory: if the user declined once, don't prompt again.
_session_declined = False


def _is_windows() -> bool:
    return platform.system() == "Windows"


def _script_path() -> Path:
    # ground-station/app/gui/firewall.py -> ground-station/scripts/configure_firewall.ps1
    return Path(__file__).resolve().parents[2] / "scripts" / "configure_firewall.ps1"


def _run_hidden(cmd: list[str], timeout: float = 5.0) -> subprocess.CompletedProcess:
    """Run a subprocess with no console flash (Windows) and capture output."""
    kwargs: dict = {
        "capture_output": True,
        "text": True,
        "timeout": timeout,
    }
    if _is_windows():
        kwargs["creationflags"] = _CREATE_NO_WINDOW
    return subprocess.run(cmd, **kwargs)


def _powershell_available() -> bool:
    if not _is_windows():
        return False
    try:
        res = _run_hidden(["powershell", "-NoProfile", "-Command", "$PSVersionTable.PSVersion.Major"],
                          timeout=5.0)
        return res.returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return False


def _rule_exists_powershell(display_name: str) -> Optional[bool]:
    """True/False if rule exists, None if we couldn't determine."""
    cmd = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
        f"if (Get-NetFirewallRule -DisplayName '{display_name}' "
        f"-ErrorAction SilentlyContinue) {{ 'YES' }} else {{ 'NO' }}",
    ]
    try:
        res = _run_hidden(cmd, timeout=8.0)
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return None
    if res.returncode != 0:
        return None
    out = (res.stdout or "").strip().upper()
    if "YES" in out:
        return True
    if "NO" in out:
        return False
    return None


def _rule_exists_netsh(display_name: str) -> Optional[bool]:
    """Fallback check using netsh for older Windows without NetSecurity module."""
    try:
        res = _run_hidden(
            ["netsh", "advfirewall", "firewall", "show", "rule", f"name={display_name}"],
            timeout=8.0,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return None
    # netsh returns 1 when no rule matches.
    if res.returncode != 0:
        return False
    return "Rule Name" in (res.stdout or "")


def _missing_rules() -> Tuple[str, ...]:
    """Return a tuple of display-names that are *missing* (i.e. need install)."""
    missing: list[str] = []
    use_ps = _powershell_available()
    for name in (_TELEMETRY_RULE, _DISCOVERY_RULE):
        exists: Optional[bool] = None
        if use_ps:
            exists = _rule_exists_powershell(name)
        if exists is None:
            exists = _rule_exists_netsh(name)
        # If we truly cannot tell, err on the side of "missing" so the user
        # at least gets the chance to install.
        if not exists:
            missing.append(name)
    return tuple(missing)


def _elevate_and_run(parent_widget) -> str:
    """Spawn the PowerShell helper elevated via ShellExecuteW. Returns
    ``"ok"`` or ``"failed"``."""
    script = _script_path()
    if _powershell_available() and script.exists():
        exe = "powershell.exe"
        params = f'-NoProfile -ExecutionPolicy Bypass -File "{script}"'
    else:
        # Fallback: netsh rules only (can't flip network profile without PS).
        exe = "cmd.exe"
        params = (
            '/c netsh advfirewall firewall add rule '
            f'name="{_TELEMETRY_RULE}" dir=in action=allow protocol=TCP '
            f'localport={_TELEMETRY_PORT} && '
            'netsh advfirewall firewall add rule '
            f'name="{_DISCOVERY_RULE}" dir=in action=allow protocol=UDP '
            f'localport={_DISCOVERY_PORT}'
        )

    try:
        import ctypes
        from ctypes import wintypes

        SEE_MASK_NOCLOSEPROCESS = 0x00000040
        SEE_MASK_NO_CONSOLE = 0x00008000

        class SHELLEXECUTEINFO(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD),
                ("fMask", wintypes.ULONG),
                ("hwnd", wintypes.HWND),
                ("lpVerb", wintypes.LPCWSTR),
                ("lpFile", wintypes.LPCWSTR),
                ("lpParameters", wintypes.LPCWSTR),
                ("lpDirectory", wintypes.LPCWSTR),
                ("nShow", ctypes.c_int),
                ("hInstApp", wintypes.HINSTANCE),
                ("lpIDList", ctypes.c_void_p),
                ("lpClass", wintypes.LPCWSTR),
                ("hkeyClass", wintypes.HKEY),
                ("dwHotKey", wintypes.DWORD),
                ("hIconOrMonitor", wintypes.HANDLE),
                ("hProcess", wintypes.HANDLE),
            ]

        sei = SHELLEXECUTEINFO()
        sei.cbSize = ctypes.sizeof(sei)
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE
        sei.lpVerb = "runas"
        sei.lpFile = exe
        sei.lpParameters = params
        sei.lpDirectory = str(script.parent.parent) if script.exists() else None
        sei.nShow = 0  # SW_HIDE

        ok = ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(sei))
        if not ok or not sei.hProcess:
            return "failed"  # UAC cancelled or launch failed

        # Wait up to 10 s for completion.
        WAIT_TIMEOUT = 0x00000102
        rc = ctypes.windll.kernel32.WaitForSingleObject(sei.hProcess, 10_000)
        if rc == WAIT_TIMEOUT:
            ctypes.windll.kernel32.CloseHandle(sei.hProcess)
            return "failed"

        exit_code = wintypes.DWORD()
        ctypes.windll.kernel32.GetExitCodeProcess(sei.hProcess, ctypes.byref(exit_code))
        ctypes.windll.kernel32.CloseHandle(sei.hProcess)
        return "ok" if exit_code.value == 0 else "failed"
    except Exception:
        return "failed"


def check_and_prompt(parent_widget) -> str:
    """Check firewall state; prompt once per session if configuration is
    needed. See module docstring for return-value semantics."""
    global _session_declined

    if not _is_windows():
        return "ok"

    try:
        missing = _missing_rules()
    except Exception:
        # Never crash the GUI because of a probing error.
        return "failed"

    if not missing:
        return "ok"

    if _session_declined:
        return "deferred"

    if QMessageBox is None:
        # Can't prompt — treat as deferred.
        return "deferred"

    box = QMessageBox(parent_widget)
    box.setIcon(QMessageBox.Icon.Question)
    box.setWindowTitle("COATHEAL — Firewall setup")
    missing_desc = ", ".join(missing)
    box.setText(
        "Allow GS auto-configure firewall? (requires admin)\n\n"
        f"Missing / needs configuration: {missing_desc}\n\n"
        "Without these rules the Pi may not be able to reach the ground "
        "station over the link-local Ethernet connection. See "
        "docs/firewall.md for details."
    )
    box.setStandardButtons(QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
    box.setDefaultButton(QMessageBox.StandardButton.Yes)
    choice = box.exec()

    if choice != QMessageBox.StandardButton.Yes:
        _session_declined = True
        return "deferred"

    return _elevate_and_run(parent_widget)
