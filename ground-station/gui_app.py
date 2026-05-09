#!/usr/bin/env python3
"""COATHEAL Ground Station — PyQt6 + PyQtGraph real-time GUI.

Usage:
    cd ground-station
    python gui_app.py [--tel-port 4000] [--cmd-port 5000] [--host 169.254.10.10]
    python gui_app.py --demo

All real code lives in `app/gui/`. This file stays as a stable entry point so
launch commands and shortcuts don't break when the UI is refactored.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from app.gui.main_window import run_gui


if __name__ == "__main__":
    raise SystemExit(run_gui())
