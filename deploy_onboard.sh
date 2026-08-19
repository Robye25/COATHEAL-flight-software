#!/usr/bin/env bash
set -euo pipefail

# COATHEAL one-command onboard deployment.
#
#   First time :  bash /bexus/code/coatheal/deploy_onboard.sh
#   Afterwards :  coatheal-deploy
#
# Idempotent end-to-end: pulls the latest repository state, retires any
# previously installed COATHEAL service iteration, migrates an existing local
# config to the current schema (retired keys dropped, v3 defaults injected,
# original backed up), rebuilds, proves the config loads with the flight
# binary itself, reinstalls + starts the systemd units, and verifies the
# service is up. Any failing step aborts loudly — it never leaves a
# half-deployed service enabled.
#
#   --dry-run   print every action instead of executing it
#   <dir>       project directory (default /bexus/code/coatheal)

PROJECT_DIR="/bexus/code/coatheal"
DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --post-pull) ;;  # internal re-exec marker, handled below
    *) PROJECT_DIR="$arg" ;;
  esac
done

LOCAL_INI="$PROJECT_DIR/config/onboard.local.ini"
BINARY="$PROJECT_DIR/build/onboard/coatheal_onboard"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mDEPLOY FAILED: %s\033[0m\n' "$*" >&2; exit 1; }
run()  {
  if [[ "$DRY_RUN" == "1" ]]; then
    printf '\033[0;33m[dry-run]\033[0m %s\n' "$*"
  else
    "$@"
  fi
}

if [[ "$DRY_RUN" != "1" && "$(uname -s)" != "Linux" ]]; then
  die "this script deploys onto the Raspberry Pi (Linux); use --dry-run elsewhere"
fi

[[ -d "$PROJECT_DIR/.git" ]] || die "no git repository at $PROJECT_DIR"

# --- 1. Self-update: pull, then re-exec the freshly pulled copy of this
#        script exactly once, so the deploy logic in use is always current.
if [[ "${COATHEAL_DEPLOY_PULLED:-0}" != "1" ]]; then
  say "Pulling latest repository state"
  run git -C "$PROJECT_DIR" pull --ff-only || die "git pull failed (diverged history? resolve manually)"
  say "Re-executing the updated deploy script"
  export COATHEAL_DEPLOY_PULLED=1
  exec bash "$PROJECT_DIR/deploy_onboard.sh" "$@" --post-pull
fi

# --- 2. Retire every previously installed COATHEAL service iteration.
#        Wildcard sweep so old iterations are caught regardless of the unit
#        names they used. Data and logs are never touched.
say "Retiring previously installed COATHEAL services"
mapfile -t OLD_UNITS < <(systemctl list-unit-files 'coatheal*' --no-legend 2>/dev/null | awk '{print $1}')
if [[ ${#OLD_UNITS[@]} -gt 0 ]]; then
  for unit in "${OLD_UNITS[@]}"; do
    echo "    stopping/disabling $unit"
    run sudo systemctl disable --now "$unit" || true
  done
else
  echo "    none found"
fi

# --- 3. System dependencies, service user, interfaces. Fast no-ops when
#        already satisfied.
say "Installing system dependencies"
run sudo apt-get update -y
run sudo apt-get install -y \
  build-essential cmake git pkg-config \
  libgpiod-dev libi2c-dev i2c-tools \
  python3 python3-pip python3-venv

say "Ensuring the coatheal service user exists"
if ! id -u coatheal >/dev/null 2>&1; then
  run sudo useradd --system --home /bexus --shell /usr/sbin/nologin coatheal
fi
# The service reads I2C/SPI/GPIO as its own user.
for grp in gpio i2c spi dialout; do
  getent group "$grp" >/dev/null 2>&1 && run sudo usermod -aG "$grp" coatheal || true
done

say "Enabling I2C and SPI"
if command -v raspi-config >/dev/null 2>&1; then
  run sudo raspi-config nonint do_i2c 0
  run sudo raspi-config nonint do_spi 0
else
  echo "    raspi-config not present; assuming interfaces already enabled"
fi

# Obsolete kernel chip-select remap from an earlier iteration: remove if found.
for BOOT_CONFIG in /boot/firmware/config.txt /boot/config.txt; do
  [[ -f "$BOOT_CONFIG" ]] || continue
  SPI_OVERLAY='dtoverlay=spi0-2cs,cs0_pin=22,cs1_pin=23'
  if grep -Fxq "$SPI_OVERLAY" "$BOOT_CONFIG" 2>/dev/null; then
    say "Removing obsolete SPI chip-select overlay from $BOOT_CONFIG"
    run sudo sed -i "\|^${SPI_OVERLAY}\$|d" "$BOOT_CONFIG"
    echo "    NOTE: a reboot is required for this overlay removal to take effect"
  fi
  break
done

say "Preparing directories"
run sudo mkdir -p /bexus/data /bexus/logs "$PROJECT_DIR/logs"
run sudo chown -R coatheal:coatheal /bexus/data /bexus/logs "$PROJECT_DIR/logs"

# --- 4. Build. Always through a normal reconfigure; a stale build/ from a
#        previous iteration is handled by CMake itself.
say "Building the onboard software"
run cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build"
run cmake --build "$PROJECT_DIR/build" -j"$(nproc)"
if [[ "$DRY_RUN" != "1" ]]; then
  [[ -x "$BINARY" ]] || die "build produced no binary at $BINARY"
fi

# --- 5. Config: migrate whatever local config exists (any prior iteration's)
#        to the current schema. Retired keys are dropped, v3 defaults
#        injected, and the original is backed up beside itself. With no local
#        config, this generates a fresh one from the v3 template. The
#        migration also validates the result against the freshly built
#        binary, so a bad config stops the deploy here — before any service
#        starts.
say "Migrating configuration to the current schema"
run python3 "$PROJECT_DIR/scripts/hardware_setup.py" migrate-config \
  --config "$LOCAL_INI" --migrate-from "$LOCAL_INI" --yes \
  || die "config migration failed — the previous config could not be brought to the v3 schema"

say "Proving the migrated config loads in the flight binary"
run "$BINARY" --config "$LOCAL_INI" --check-config || die "the flight binary rejected $LOCAL_INI"

# --- 6. Install and start the canonical systemd units (idempotent installer;
#        enables + starts flight and link-watch, leaves debug disabled).
say "Installing and starting the systemd units"
if [[ "$DRY_RUN" == "1" ]]; then
  run bash "$PROJECT_DIR/scripts/install_onboard_service.sh" "$PROJECT_DIR" "$LOCAL_INI"
else
  bash "$PROJECT_DIR/scripts/install_onboard_service.sh" "$PROJECT_DIR" "$LOCAL_INI"
fi

# --- 7. Make the one-word command available for next time.
say "Installing the coatheal-deploy command"
run sudo ln -sf "$PROJECT_DIR/deploy_onboard.sh" /usr/local/bin/coatheal-deploy

# --- 8. Verify and report.
if [[ "$DRY_RUN" == "1" ]]; then
  say "Dry run complete — no system state was changed"
  exit 0
fi

say "Verifying the service"
sleep 3
if ! systemctl is-active --quiet coatheal-onboard.service; then
  systemctl --no-pager --full status coatheal-onboard.service || true
  die "coatheal-onboard.service is not active — status above, logs: journalctl -u coatheal-onboard.service"
fi

printf '\n\033[1;32m================================================================\033[0m\n'
printf '\033[1;32m  COATHEAL onboard DEPLOYED and RUNNING\033[0m\n'
printf '  Service : coatheal-onboard.service (active)\n'
printf '  Config  : %s\n' "$LOCAL_INI"
printf '  IPs     : %s\n' "$(hostname -I 2>/dev/null || echo unknown)"
printf '  The ground station can now discover this onboard automatically.\n'
printf '  Next deploy: just run   coatheal-deploy\n'
printf '\033[1;32m================================================================\033[0m\n'
