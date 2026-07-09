#!/usr/bin/env bash
set -euo pipefail

CONFIG_PATH="${1:-/bexus/code/coatheal/config/onboard.example.ini}"

if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "[preflight] missing config: $CONFIG_PATH" >&2
  exit 1
fi

PROJECT_ROOT="$(cd "$(dirname "$CONFIG_PATH")/.." && pwd)"

read_cfg() {
  local key="$1"
  awk -F'=' -v k="$key" '$1==k {print substr($0, index($0,"=")+1)}' "$CONFIG_PATH" | tail -n 1
}

resolve_path() {
  local p="$1"
  if [[ "$p" = /* ]]; then
    printf '%s\n' "$p"
  else
    printf '%s\n' "$PROJECT_ROOT/$p"
  fi
}

PRIMARY_LOG_RAW="$(read_cfg storage.primary_log_path)"
SECONDARY_LOG_RAW="$(read_cfg storage.secondary_log_path)"
QUEUE_DIR_RAW="$(read_cfg storage.queue_dir)"

if [[ -z "$PRIMARY_LOG_RAW" || -z "$SECONDARY_LOG_RAW" || -z "$QUEUE_DIR_RAW" ]]; then
  echo "[preflight] missing required storage paths in config" >&2
  exit 1
fi

PRIMARY_LOG="$(resolve_path "$PRIMARY_LOG_RAW")"
SECONDARY_LOG="$(resolve_path "$SECONDARY_LOG_RAW")"
QUEUE_DIR="$(resolve_path "$QUEUE_DIR_RAW")"

if ! mkdir -p "$(dirname "$PRIMARY_LOG")" "$(dirname "$SECONDARY_LOG")" "$QUEUE_DIR" ||
   ! touch "$PRIMARY_LOG" "$SECONDARY_LOG"; then
  echo "[preflight] warn: storage unavailable; onboard will run degraded" >&2
elif [[ ! -w "$PRIMARY_LOG" || ! -w "$SECONDARY_LOG" || ! -w "$QUEUE_DIR" ]]; then
  echo "[preflight] warn: storage paths are not writable; onboard will run degraded" >&2
fi

# --- Network environment checks (fail fast) -------------------------------

# UDP 4100 is the discovery port; if something else holds it we can't bind.
# ss is preferred (iproute2); fall back to lsof/netstat if missing.
udp_port_in_use() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -lun 2>/dev/null | awk '{print $5}' | grep -Eq "[:.]${port}$" && return 0 || return 1
  elif command -v lsof >/dev/null 2>&1; then
    lsof -iUDP:"$port" -sUDP:LISTEN >/dev/null 2>&1 && return 0 || return 1
  elif command -v netstat >/dev/null 2>&1; then
    netstat -lun 2>/dev/null | awk '{print $4}' | grep -Eq "[:.]${port}$" && return 0 || return 1
  else
    # Can't check - don't block the flight on tool absence.
    echo "[preflight] warn: no ss/lsof/netstat; skipping UDP port check" >&2
    return 1
  fi
}

if udp_port_in_use 4100; then
  echo "[preflight] warn: UDP 4100 is in use; discovery may be degraded" >&2
fi

# At least one non-loopback interface must be up. /sys/class/net/*/operstate
# reports "up" / "down" / "unknown".
have_link=0
if [[ -d /sys/class/net ]]; then
  for iface in /sys/class/net/*; do
    name="$(basename "$iface")"
    [[ "$name" == "lo" ]] && continue
    state="$(cat "$iface/operstate" 2>/dev/null || echo down)"
    if [[ "$state" == "up" ]]; then
      have_link=1
      break
    fi
  done
fi

if [[ "$have_link" -ne 1 ]]; then
  echo "[preflight] warn: no network link; onboard will wait for one" >&2
fi

echo "[preflight] ok config=$CONFIG_PATH"
