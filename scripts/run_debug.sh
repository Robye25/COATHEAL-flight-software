#!/usr/bin/env bash
# Launch the onboard flight software in debug/bench mode. Meant to be run on
# the Pi during integration testing.
#
# Usage:
#   ./scripts/run_debug.sh             # foreground, Ctrl-C to stop
#   ./scripts/run_debug.sh --bg        # detach as a background process, PID
#                                       written to logs/onboard_debug.pid
#   ./scripts/run_debug.sh --stop      # stop a previously --bg run
#   ./scripts/run_debug.sh --status    # report running/stopped

set -eu
cd "$(dirname "$0")/.."

CONFIG="config/onboard.debug.ini"
BIN="build/onboard/coatheal_onboard"
LOG="logs/onboard_debug.stdout.log"
PIDFILE="logs/onboard_debug.pid"

mkdir -p logs

case "${1:-}" in
  --stop)
    if [[ -f "$PIDFILE" ]]; then
      pid=$(cat "$PIDFILE")
      if kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid"
        echo "sent SIGINT to $pid"
        for _ in 1 2 3 4 5; do
          kill -0 "$pid" 2>/dev/null || break
          sleep 1
        done
        kill -0 "$pid" 2>/dev/null && { kill -KILL "$pid"; echo "forced kill"; }
      fi
      rm -f "$PIDFILE"
    else
      echo "no pidfile at $PIDFILE — nothing to stop"
    fi
    exit 0
    ;;
  --status)
    if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "RUNNING pid=$(cat "$PIDFILE")"
      exit 0
    fi
    echo "stopped"
    exit 1
    ;;
esac

if [[ ! -x "$BIN" ]]; then
  echo "binary not found — run: cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

# Guard: refuse to launch a second instance on top of an existing one.
if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "already running (pid $(cat "$PIDFILE")). Use --stop first." >&2
  exit 1
fi

if [[ "${1:-}" == "--bg" ]]; then
  nohup "$BIN" --config "$CONFIG" >"$LOG" 2>&1 &
  echo $! > "$PIDFILE"
  echo "launched in background, pid=$!, log=$LOG"
else
  echo "launching foreground — Ctrl-C to stop"
  exec "$BIN" --config "$CONFIG"
fi
