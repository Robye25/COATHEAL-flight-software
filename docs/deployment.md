# Deployment Guide

This guide covers first-time Raspberry Pi 4 setup, building the onboard software, installing the systemd service, and network configuration.

For the current Rev C operator procedure, including plug-and-play Ethernet,
manual-first operation, and the current component/pin map, use
[rev-c-installation-and-hardware-setup.md](rev-c-installation-and-hardware-setup.md).

---

## Prerequisites

- Raspberry Pi 4 running Raspberry Pi OS (64-bit, Bookworm or Bullseye)
- Ethernet connection to the ground station laptop
- `git`, `cmake ≥ 3.16`, `g++ ≥ 10` (C++17 support required)
- `libgpiod-dev` (for real PWM output on GPIO pins)

### Install build dependencies

```bash
sudo apt update
sudo apt install -y git cmake g++ libgpiod-dev
```

---

## First-Time Pi Setup

### 1. Clone the repository

This repository is private. GitHub rejects account passwords for Git over
HTTPS, so use either an SSH deploy key or a GitHub Personal Access Token. The
recommended Pi setup is a read-only deploy key:

```bash
mkdir -p ~/.ssh
chmod 700 ~/.ssh
ssh-keygen -t ed25519 -C "coatheal-pi-$(hostname)" -f ~/.ssh/coatheal_github -N ""
cat ~/.ssh/coatheal_github.pub
```

Add the printed public key in GitHub under:

```text
Robye25/COATHEAL-flight-software -> Settings -> Deploy keys -> Add deploy key
```

Then configure and clone:

```bash
sudo mkdir -p /bexus/code
sudo chown $USER:$USER /bexus/code
cat > ~/.ssh/config <<'EOF'
Host github.com-coatheal
  HostName github.com
  User git
  IdentityFile ~/.ssh/coatheal_github
  IdentitiesOnly yes
  StrictHostKeyChecking accept-new
EOF
chmod 600 ~/.ssh/config
git clone git@github.com-coatheal:Robye25/COATHEAL-flight-software.git /bexus/code/coatheal
cd /bexus/code/coatheal
```

If the repo already exists and was cloned over HTTPS, switch it to SSH:

```bash
cd /bexus/code/coatheal
git remote set-url origin git@github.com-coatheal:Robye25/COATHEAL-flight-software.git
git fetch origin
git pull --ff-only origin main
```

### 2. Configure the INI file

Copy and edit the example configuration:

```bash
cp config/onboard.example.ini config/onboard.ini
nano config/onboard.ini
```

Key settings to review before flight:

| Key | Flight value |
|---|---|
| `runtime.bench_mode` | `false` |
| `runtime.use_simulated_pwm` | `false` |
| `runtime.debug_arm_code` | Change from default |
| `comms.telemetry_host` | Empty for plug-and-play, or a fixed ground-station laptop IP |
| `storage.primary_log_path` | SD card mount path |
| `storage.secondary_log_path` | USB drive mount path |

### 3. Build the onboard software

```bash
cd /bexus/code/coatheal
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The binary is at `build/onboard/coatheal_onboard`.

### 4. Verify the build

```bash
# Run the unit tests
ctest --test-dir build --output-on-failure

# Quick bench-mode smoke test
./build/onboard/coatheal_onboard --config config/onboard.example.ini
# (bench_mode=false in the example, so this will attempt real hardware)
# To smoke-test without hardware, temporarily set bench_mode=true
```

---

## Network Configuration

The Pi connects **out** to the ground station for telemetry (port 4000) and **listens** for commands (port 5000).

### Ethernet — Static Link-Local

For a direct Pi-to-laptop cable, assign static link-local addresses:

**On the Pi** (`/etc/dhcpcd.conf` or `nmcli`):
```
interface eth0
static ip_address=169.254.10.10/16
```

**On the laptop** (Windows Network Adapter settings):
```
IP: 169.254.10.11
Subnet: 255.255.0.0
```

Or let both sides use auto-assigned `169.254.x.x` addresses and rely on UDP discovery to find each other.

### Firewall (Pi)

If `ufw` is active on the Pi:

```bash
sudo ufw allow 5000/tcp comment "COATHEAL command server"
# The Pi connects out on port 4000 — no inbound rule needed for telemetry
```

### Firewall (Ground Station — Windows)

Allow inbound TCP on port 4000 in Windows Defender Firewall (the Pi connects to this port).

---

## systemd Service

### Install and start

```bash
cd /bexus/code/coatheal
./scripts/install_onboard_service.sh /bexus/code/coatheal config/onboard.ini
```

This script:
1. Verifies the binary and config exist
2. Writes `/etc/systemd/system/coatheal-onboard.service`
3. Runs `systemctl daemon-reload`
4. Enables and starts the service

### Service behavior

| Property | Value |
|---|---|
| User | Current user (determined at install time) |
| Working directory | `/bexus/code/coatheal` |
| Pre-start check | `scripts/preflight_healthcheck.sh` — verifies config paths and write permissions |
| Restart policy | `always`, 2-second delay, up to 10 restarts in 120 seconds |
| Graceful stop | `SIGINT`, 20-second timeout |
| Security | `NoNewPrivileges=true` |

### Service management

```bash
# Status
sudo systemctl status coatheal-onboard

# Start / stop / restart
sudo systemctl start coatheal-onboard
sudo systemctl stop coatheal-onboard
sudo systemctl restart coatheal-onboard

# View live logs
sudo journalctl -u coatheal-onboard -f

# Disable autostart
sudo systemctl disable coatheal-onboard
```

---

## Preflight Healthcheck

`scripts/preflight_healthcheck.sh` is run by the systemd `ExecStartPre` directive before every start. It:

1. Checks the config file exists
2. Reads `storage.primary_log_path`, `storage.secondary_log_path`, and `storage.queue_dir`
3. Creates parent directories if missing
4. Touches the log files (verifies write permission)
5. Exits non-zero on any failure (prevents the service from starting with broken storage)

Run manually:

```bash
./scripts/preflight_healthcheck.sh config/onboard.ini
# [preflight] ok config=config/onboard.ini
```

---

## Storage Layout

Default log paths (relative to working directory):

| Path | Purpose |
|---|---|
| `logs/onboard_primary.csv` | Primary telemetry CSV (SD card) |
| `logs/onboard_usb_mirror.csv` | Secondary CSV mirror (USB drive) |
| `logs/telemetry-queue/` | Durable disk queue (survives restarts) |

For flight, set `storage.primary_log_path` to a path on the SD card and `storage.secondary_log_path` to a path on a USB drive. The software writes to both independently; if one fails to open, it continues writing to the other.

---

## Updating the Software

```bash
cd /bexus/code/coatheal
git pull
cmake --build build --parallel
sudo systemctl restart coatheal-onboard
```

---

## Uninstalling

```bash
sudo systemctl stop coatheal-onboard
sudo systemctl disable coatheal-onboard
sudo rm /etc/systemd/system/coatheal-onboard.service
sudo systemctl daemon-reload
```
