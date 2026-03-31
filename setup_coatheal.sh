#!/bin/bash
# ============================================================
#  COATHEAL Raspberry Pi 4 Setup Script
#  Prepares Raspberry Pi OS Lite (64-bit) for flight software
# ============================================================

echo "==> Updating system packages..."
sudo apt update -y && sudo apt full-upgrade -y

echo "==> Enabling I2C and SPI via raspi-config..."
sudo raspi-config nonint do_i2c 0   # 0 = enable
sudo raspi-config nonint do_spi 0

echo "==> Installing development tools..."
sudo apt install -y build-essential cmake git pkg-config

echo "==> Installing libraries and utilities..."
sudo apt install -y libi2c-dev libgpiod-dev pigpio i2c-tools vim htop screen curl

echo "==> Enabling and starting pigpiod service..."
sudo systemctl enable pigpiod
sudo systemctl start pigpiod

echo "==> Creating COATHEAL directory structure..."
sudo mkdir -p /bexus/{code,data,logs,config}
sudo chown -R $USER:$USER /bexus

echo "==> Setting recommended system configuration..."
# Reduce GPU memory to 16MB (no GUI needed)
sudo raspi-config nonint do_memory_split 16

# Optional: disable unnecessary services to reduce power draw
sudo systemctl disable --now bluetooth || true
sudo systemctl disable --now triggerhappy || true
sudo systemctl disable --now avahi-daemon || true
sudo systemctl disable --now dphys-swapfile || true

echo "==> Cleanup and reboot prompt..."
sudo apt autoremove -y
sudo apt clean

echo "============================================================"
echo " COATHEAL Raspberry Pi preparation complete."
echo " Please REBOOT now: sudo reboot"
echo "============================================================"

