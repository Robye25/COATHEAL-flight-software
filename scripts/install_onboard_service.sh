#!/usr/bin/env bash
set -euo pipefail

# Install COATHEAL onboard systemd units (flight + debug + link-watch + the
# E-Link hard cap) from the deploy/ tree. Idempotent: safe to re-run.

PROJECT_DIR="${1:-/bexus/code/coatheal}"
if [[ $# -ge 2 ]]; then
  CONFIG_PATH="$2"
elif [[ -f "$PROJECT_DIR/config/onboard.local.ini" ]]; then
  CONFIG_PATH="$PROJECT_DIR/config/onboard.local.ini"
else
  CONFIG_PATH="$PROJECT_DIR/config/onboard.example.ini"
fi
DEPLOY_DIR="$PROJECT_DIR/deploy"
SYSTEMD_DIR="/etc/systemd/system"
BINARY_PATH="$PROJECT_DIR/build/onboard/coatheal_onboard"
HEALTHCHECK_PATH="$PROJECT_DIR/scripts/preflight_healthcheck.sh"

UNITS=(
  coatheal-onboard.service
  coatheal-onboard-debug.service
  coatheal-link-watch.service
  coatheal-link-watch.path
  coatheal-link-cap.service
)

if [[ ! -x "$HEALTHCHECK_PATH" ]]; then
  chmod +x "$HEALTHCHECK_PATH" || true
fi

if [[ ! -x "$BINARY_PATH" ]]; then
  echo "[install-service] onboard binary missing: $BINARY_PATH" >&2
  echo "[install-service] build first: cmake -S . -B build && cmake --build build --config Release" >&2
  exit 1
fi

if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "[install-service] config missing: $CONFIG_PATH" >&2
  exit 1
fi

if [[ ! -d "$DEPLOY_DIR" ]]; then
  echo "[install-service] deploy dir missing: $DEPLOY_DIR" >&2
  exit 1
fi

SUDO=""
if [[ $EUID -ne 0 ]]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "[install-service] must run as root or have sudo" >&2
    exit 1
  fi
  SUDO="sudo"
fi

$SUDO chmod 0644 "$CONFIG_PATH" || true
if [[ -n "$SUDO" ]]; then
  config_readable_cmd=("$SUDO" -u coatheal test -r "$CONFIG_PATH")
elif command -v runuser >/dev/null 2>&1; then
  config_readable_cmd=(runuser -u coatheal -- test -r "$CONFIG_PATH")
else
  config_readable_cmd=(test -r "$CONFIG_PATH")
fi
if ! "${config_readable_cmd[@]}" 2>/dev/null; then
  echo "[install-service] config is not readable by service user coatheal: $CONFIG_PATH" >&2
  echo "[install-service] fix with: sudo chmod 0644 '$CONFIG_PATH'" >&2
  exit 1
fi

for unit in "${UNITS[@]}"; do
  src="$DEPLOY_DIR/$unit"
  if [[ ! -f "$src" ]]; then
    echo "[install-service] missing unit file: $src" >&2
    exit 1
  fi
  echo "[install-service] installing $unit"
  $SUDO cp -f "$src" "$SYSTEMD_DIR/$unit"
  $SUDO chmod 0644 "$SYSTEMD_DIR/$unit"
done

# Write the config path into the environment file read by the service unit.
# This allows the same .service file to work with any config without edits.
$SUDO mkdir -p /etc/coatheal
echo "COATHEAL_CONFIG=$CONFIG_PATH" | $SUDO tee /etc/coatheal/env >/dev/null
$SUDO chmod 0644 /etc/coatheal/env
echo "[install-service] config path written to /etc/coatheal/env"

$SUDO systemctl daemon-reload

# The E-Link hard cap (docs/link-budget.md) takes the whole port, ssh and
# git included, so it is switched on by hand, or by a --flight deploy:
#   coatheal-link-cap on | off | status
$SUDO chmod +x "$PROJECT_DIR/scripts/link_cap.sh" || true
$SUDO ln -sf "$PROJECT_DIR/scripts/link_cap.sh" /usr/local/bin/coatheal-link-cap
LINK_CAP="off"
if [[ -r /etc/coatheal/link-cap.env ]] && grep -qx 'COATHEAL_LINK_CAP=on' /etc/coatheal/link-cap.env; then
  LINK_CAP="on"
fi
if [[ "$LINK_CAP" == "on" ]]; then
  echo "[install-service] E-Link hard cap: on (coatheal-link-cap.service)"
  $SUDO systemctl enable coatheal-link-cap.service
  $SUDO systemctl restart coatheal-link-cap.service || \
    echo "[install-service] WARNING: the hard cap could not be installed; see: systemctl status coatheal-link-cap.service" >&2
else
  echo "[install-service] E-Link hard cap: off (bench). Before flight: coatheal-link-cap on"
  $SUDO systemctl disable --now coatheal-link-cap.service >/dev/null 2>&1 || true
fi

# Flight + link-watch on by default; debug installed but left disabled.
$SUDO systemctl enable --now coatheal-onboard.service coatheal-link-watch.path

echo "[install-service] --- status ---"
for unit in "${UNITS[@]}"; do
  echo
  echo "### $unit"
  $SUDO systemctl --no-pager --full status "$unit" || true
done

echo "[install-service] done. Debug profile is installed but not enabled."
echo "[install-service] To switch profiles:"
echo "  $SUDO systemctl disable --now coatheal-onboard.service"
echo "  $SUDO systemctl enable  --now coatheal-onboard-debug.service"
