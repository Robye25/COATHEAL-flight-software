#!/usr/bin/env bash
set -euo pipefail

# COATHEAL Raspberry Pi bootstrap (Debian/Raspberry Pi OS)

echo "==> Updating system packages"
sudo apt update -y
sudo apt full-upgrade -y

echo "==> Installing build and runtime dependencies"
sudo apt install -y \
  build-essential cmake git pkg-config \
  libgpiod-dev libi2c-dev i2c-tools \
  python3 python3-pip python3-venv

echo "==> Creating project directories"
sudo mkdir -p /bexus/code /bexus/data /bexus/logs /bexus/config
sudo chown -R "$USER":"$USER" /bexus

echo "==> Enabling SPI and I2C"
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0

echo "==> Removing obsolete kernel chip-select remapping"
BOOT_CONFIG="/boot/firmware/config.txt"
if [[ ! -f "$BOOT_CONFIG" ]]; then
  BOOT_CONFIG="/boot/config.txt"
fi
SPI_OVERLAY="dtoverlay=spi0-2cs,cs0_pin=22,cs1_pin=23"
if grep -Fxq "$SPI_OVERLAY" "$BOOT_CONFIG"; then
  sudo sed -i "\|^${SPI_OVERLAY}$|d" "$BOOT_CONFIG"
fi

echo "==> Recommended service trims"
sudo systemctl disable --now bluetooth || true
sudo systemctl disable --now triggerhappy || true
sudo systemctl disable --now avahi-daemon || true

if [[ -f scripts/preflight_healthcheck.sh ]]; then
  chmod +x scripts/preflight_healthcheck.sh || true
fi
if [[ -f scripts/install_onboard_service.sh ]]; then
  chmod +x scripts/install_onboard_service.sh || true
fi

cat <<'MSG'

COATHEAL bootstrap complete.

Next steps:
1. Clone repository into /bexus/code/coatheal
   Use a GitHub SSH deploy key or a Personal Access Token. GitHub account
   passwords do not work for private-repo Git clone/pull operations.
2. Reboot to activate I2C and SPI. The onboard software drives each configured
   TMC2240 chip-select GPIO line directly:
   sudo reboot
3. Build onboard app:
   cmake -S . -B build
   cmake --build build -j
4. Run onboard manually once:
   ./build/onboard/coatheal_onboard --config config/onboard.example.ini
5. Install boot service:
   ./scripts/install_onboard_service.sh /bexus/code/coatheal /bexus/code/coatheal/config/onboard.example.ini

MSG
