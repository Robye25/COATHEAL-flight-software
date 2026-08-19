#!/usr/bin/env bash
set -u
# ============================================================
#  COATHEAL Ground Station launcher - Linux (Mint/Ubuntu/Debian).
#
#    ./COATHEAL-GroundStation.sh            launch the GUI
#    ./COATHEAL-GroundStation.sh --check    verify the setup, no GUI
#
#  First run: creates a local Python environment, installs the
#  dependencies, and offers firewall openings if ufw is active.
#  Takes a few minutes. Every later run launches instantly.
# ============================================================

cd "$(dirname "$0")"
VENV=".venv"
REQ="requirements.txt"
REQ_MARKER="$VENV/.requirements.sha"
FW_MARKER="$VENV/.firewall.done"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m  %s\033[0m\n' "$*"; exit 1; }

# ---- 1. Python + venv support ------------------------------------------
command -v python3 >/dev/null 2>&1 || fail "python3 not found. Install it:  sudo apt install python3 python3-venv python3-pip"

# Debian-family systems ship the venv module but strip ensurepip out of it
# until python3-venv is installed; "python3 -m venv" then either fails or
# leaves a half-built .venv with no pip inside. So the gate is functional:
# the venv only counts as created once its own pip answers.
ensure_venv_tooling() {
  say "The python3-venv/pip packages are missing or incomplete"
  read -r -p "  Install them now with apt? [Y/n] " ans
  case "${ans:-Y}" in
    [Yy]*) sudo apt-get update -y && sudo apt-get install -y python3-venv python3-pip || fail "apt install failed" ;;
    *) fail "Cannot continue without python3-venv. Install it and re-run:  sudo apt install python3-venv python3-pip" ;;
  esac
}

# ---- 2. Create the local environment on first run ----------------------
VPY="$VENV/bin/python"
venv_ok() { [[ -x "$VPY" ]] && "$VPY" -m pip --version >/dev/null 2>&1; }

if ! venv_ok; then
  # A .venv without a working pip is a broken half-creation from an earlier
  # attempt (the classic "No module named pip"). Try to repair in place,
  # then rebuild from scratch, installing the apt packages if needed.
  if [[ -x "$VPY" ]]; then
    say "Repairing the Python environment (pip is missing inside it)"
    "$VPY" -m ensurepip --upgrade >/dev/null 2>&1 || true
  fi
  if ! venv_ok; then
    [[ -d "$VENV" ]] && rm -rf "$VENV"
    say "Creating the Python environment"
    if ! python3 -m venv "$VENV" || ! venv_ok; then
      rm -rf "$VENV"
      ensure_venv_tooling
      say "Creating the Python environment"
      python3 -m venv "$VENV" && venv_ok || fail "could not create a working Python environment"
    fi
  fi
fi

# ---- 3. Install dependencies only when requirements changed ------------
REQ_HASH="$(sha256sum "$REQ" | awk '{print $1}')"
OLD_HASH="$(cat "$REQ_MARKER" 2>/dev/null || true)"
if [[ "$REQ_HASH" != "$OLD_HASH" ]]; then
  say "Installing dependencies (first run or requirements changed)"
  "$VPY" -m pip install --upgrade pip --quiet
  "$VPY" -m pip install -r "$REQ" || fail "dependency installation failed - check your internet connection and re-run"
  echo "$REQ_HASH" > "$REQ_MARKER"
  # Qt6's xcb platform plugin needs libxcb-cursor0 on Debian-family
  # desktops; without it the GUI dies with a cryptic qt.qpa.plugin error.
  if command -v apt-get >/dev/null 2>&1 && ! dpkg -s libxcb-cursor0 >/dev/null 2>&1; then
    say "Installing the Qt runtime dependency (libxcb-cursor0)"
    sudo apt-get install -y libxcb-cursor0 || \
      echo "  (could not install libxcb-cursor0 - if the GUI fails to start, install it manually)"
  fi
fi

# ---- 4. Diagnostics mode (skips the firewall prompt) -------------------
if [[ "${1:-}" == "--check" ]]; then
  say "Environment check"
  "$VPY" -c "import PyQt6, pyqtgraph, numpy; print('  Python environment OK')" || fail "environment check failed"
  echo "  All good. Run without --check to start the GUI."
  exit 0
fi

# ---- 5. First-run firewall setup (needed for auto-discovery) -----------
if [[ ! -f "$FW_MARKER" ]]; then
  if command -v ufw >/dev/null 2>&1 && sudo ufw status 2>/dev/null | grep -q "Status: active"; then
    say "The ufw firewall is active"
    echo "  Auto-discovery needs telemetry (TCP 4000) and discovery (UDP 4100) open."
    read -r -p "  Open them now? [Y/n] " ans
    case "${ans:-Y}" in
      [Yy]*)
        sudo ufw allow 4000/tcp comment 'COATHEAL telemetry' && \
        sudo ufw allow 4100/udp comment 'COATHEAL discovery' && \
        echo done > "$FW_MARKER" || echo "  (firewall step failed - discovery may not work)"
        ;;
      *) echo skipped > "$FW_MARKER"
         echo "  Skipped. If the onboard is never discovered:  sudo ufw allow 4000/tcp; sudo ufw allow 4100/udp" ;;
    esac
  else
    echo done > "$FW_MARKER"   # no active firewall - nothing to open
  fi
fi

# ---- 6. Launch ----------------------------------------------------------
say "Starting COATHEAL Ground Station"
exec "$VPY" gui_app.py "$@"
