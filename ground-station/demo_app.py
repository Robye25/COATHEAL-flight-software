#!/usr/bin/env python3
"""Launch the COATHEAL ground station in self-contained stand-demo mode."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from app.gui.main_window import run_gui


if __name__ == "__main__":
    raise SystemExit(run_gui(["--demo", *sys.argv[1:]]))
