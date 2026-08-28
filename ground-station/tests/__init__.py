"""Test package. Redirects QSettings to a throwaway directory so headless
GUI tests never overwrite the operator's saved window layout or UI size."""
import os
import tempfile

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
try:
    from PyQt6.QtCore import QSettings

    _SETTINGS_DIR = tempfile.mkdtemp(prefix="coatheal-test-settings-")
    QSettings.setDefaultFormat(QSettings.Format.IniFormat)
    QSettings.setPath(QSettings.Format.IniFormat, QSettings.Scope.UserScope, _SETTINGS_DIR)
    QSettings.setPath(QSettings.Format.IniFormat, QSettings.Scope.SystemScope, _SETTINGS_DIR)
except Exception:  # PyQt6 absent: the GUI tests skip themselves
    pass
