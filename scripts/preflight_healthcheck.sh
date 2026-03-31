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

mkdir -p "$(dirname "$PRIMARY_LOG")" "$(dirname "$SECONDARY_LOG")" "$QUEUE_DIR"

touch "$PRIMARY_LOG" "$SECONDARY_LOG"

if [[ ! -w "$PRIMARY_LOG" || ! -w "$SECONDARY_LOG" || ! -w "$QUEUE_DIR" ]]; then
  echo "[preflight] storage paths are not writable" >&2
  exit 1
fi

echo "[preflight] ok config=$CONFIG_PATH"
