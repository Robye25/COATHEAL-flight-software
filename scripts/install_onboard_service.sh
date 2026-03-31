#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="${1:-/bexus/code/coatheal}"
CONFIG_PATH="${2:-$PROJECT_DIR/config/onboard.example.ini}"
SERVICE_NAME="coatheal-onboard.service"
SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME"
BINARY_PATH="$PROJECT_DIR/build/onboard/coatheal_onboard"
HEALTHCHECK_PATH="$PROJECT_DIR/scripts/preflight_healthcheck.sh"

if [[ ! -x "$HEALTHCHECK_PATH" ]]; then
  chmod +x "$HEALTHCHECK_PATH"
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

SERVICE_USER="${SUDO_USER:-$USER}"

cat <<SERVICE | sudo tee "$SERVICE_PATH" >/dev/null
[Unit]
Description=COATHEAL Onboard Flight Software
After=network-online.target time-sync.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$PROJECT_DIR
ExecStartPre=$HEALTHCHECK_PATH $CONFIG_PATH
ExecStart=$BINARY_PATH --config $CONFIG_PATH
Restart=always
RestartSec=2
StartLimitIntervalSec=120
StartLimitBurst=10
KillSignal=SIGINT
TimeoutStopSec=20
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
SERVICE

sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl restart "$SERVICE_NAME"
sudo systemctl --no-pager --full status "$SERVICE_NAME" || true

echo "[install-service] installed and started $SERVICE_NAME"
