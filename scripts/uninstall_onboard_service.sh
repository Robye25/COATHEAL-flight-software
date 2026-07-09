#!/usr/bin/env bash
set -euo pipefail

# Inverse of install_onboard_service.sh: stop, disable, and remove all
# COATHEAL systemd units. Idempotent.

SYSTEMD_DIR="/etc/systemd/system"
UNITS=(
  coatheal-onboard.service
  coatheal-onboard-debug.service
  coatheal-link-watch.service
  coatheal-link-watch.path
)

SUDO=""
if [[ $EUID -ne 0 ]]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "[uninstall-service] must run as root or have sudo" >&2
    exit 1
  fi
  SUDO="sudo"
fi

for unit in "${UNITS[@]}"; do
  if $SUDO systemctl list-unit-files "$unit" 2>/dev/null | grep -q "^$unit"; then
    echo "[uninstall-service] stopping+disabling $unit"
    $SUDO systemctl disable --now "$unit" 2>/dev/null || true
  fi
  if [[ -f "$SYSTEMD_DIR/$unit" ]]; then
    echo "[uninstall-service] removing $SYSTEMD_DIR/$unit"
    $SUDO rm -f "$SYSTEMD_DIR/$unit"
  fi
done

$SUDO systemctl daemon-reload
$SUDO systemctl reset-failed || true

# Clean up runtime cool-down file left by link-watch.
$SUDO rm -f /run/coatheal-link-watch.cooldown || true

echo "[uninstall-service] done."
