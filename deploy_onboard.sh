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
# binary itself, installs the boot-time GPIO safe-state block in config.txt,
# reinstalls + starts the systemd units, and verifies the service is up. Any
# failing step aborts loudly — it never leaves a half-deployed service
# enabled.
#
#   --dry-run   print every action instead of executing it
#   --flight    refuse a config that is not at flight values (bench mode,
#               simulated backends, a bench heater.max_duty); without it
#               those are only warned about, loudly
#   <dir>       project directory (default /bexus/code/coatheal)

PROJECT_DIR="/bexus/code/coatheal"
DRY_RUN=0
FLIGHT=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --flight) FLIGHT=1 ;;
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

# --- 2. System dependencies, service user, interfaces. Fast no-ops when
#        already satisfied. (The previously installed service keeps running
#        and stays enabled until the new build and config have both been
#        proven, in step 5c -- a deploy that dies half-way must never leave
#        the Pi with no enabled onboard service.)
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
# Release build, and refuse a binary built without libgpiod: that one
# compiles and "runs" with every heater and motor output stubbed out.
run cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" \
  -DCMAKE_BUILD_TYPE=Release -DCOATHEAL_REQUIRE_LIBGPIOD=ON
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

# --- 5a. Flight invariants. migrate-config deliberately keeps whatever the
#         local INI says for these keys (a bench-tuned heater.max_duty, bench
#         mode, simulated backends), and none of them may fly. Always shown;
#         fatal with --flight.
say "Checking flight invariants in $LOCAL_INI"
FLIGHT_VIOLATIONS=""
if [[ -f "$LOCAL_INI" ]]; then
  FLIGHT_VIOLATIONS="$(python3 - "$LOCAL_INI" <<'PY'
import sys
values = {}
for raw in open(sys.argv[1], encoding="utf-8"):
    line = raw.strip()
    if not line or line[0] in "#;" or "=" not in line:
        continue
    key, value = line.split("=", 1)
    values[key.strip()] = value.strip()
def truthy(v):
    return v.lower() in ("1", "true", "yes", "on")
bad = []
for key in ("runtime.bench_mode", "runtime.use_simulated_pwm", "runtime.use_simulated_sensors"):
    if key in values and truthy(values[key]):
        bad.append(f"{key}={values[key]} (flight: false)")
if "heater.max_duty" in values:
    try:
        if float(values["heater.max_duty"]) < 0.999:
            bad.append(f"heater.max_duty={values['heater.max_duty']} (flight: 1.0)")
    except ValueError:
        bad.append(f"heater.max_duty={values['heater.max_duty']} (unparseable)")
# Owner rule 2026-09-11: the TMC5160s fly in spreadCycle.
for key in ("motor0.stealth_chop", "motor1.stealth_chop"):
    if key in values and truthy(values[key]):
        bad.append(f"{key}={values[key]} (flight: false = spreadCycle)")
print("\n".join(bad))
PY
)"
fi
if [[ -n "$FLIGHT_VIOLATIONS" ]]; then
  printf '\033[1;33m  NOT AT FLIGHT VALUES:\033[0m\n'
  while IFS= read -r violation; do
    printf '\033[1;33m    %s\033[0m\n' "$violation"
  done <<< "$FLIGHT_VIOLATIONS"
  if [[ "$FLIGHT" == "1" ]]; then
    die "flight invariants violated (above); fix $LOCAL_INI, or deploy without --flight for a bench deploy"
  fi
  printf '\033[1;33m  bench deploy: pass --flight to refuse these values\033[0m\n'
else
  echo "    all flight invariants hold (bench mode and simulated backends off, heater.max_duty 1.0, spreadCycle)"
fi

# --- 5b. Boot-time GPIO safe states. Schematic v4 fits no pull resistors on
#         the heater or motor-driver control lines, and the Pi powers on with
#         BCM 5/6 pulled UP (HEATER4/HEATER3 driven on) and BCM 20-22/27
#         pulled DOWN (both TMC5160s enabled and selected). A managed gpio=
#         block in config.txt, derived from the config just validated, pins
#         every one of those lines to its safe level from firmware boot until
#         the service claims it. Idempotent; a changed block needs a reboot.
say "Installing boot-time GPIO safe states in config.txt"
BOOT_CONFIG=""
for candidate in /boot/firmware/config.txt /boot/config.txt; do
  if [[ -f "$candidate" ]]; then BOOT_CONFIG="$candidate"; break; fi
done
REBOOT_NEEDED=0
if [[ "$DRY_RUN" == "1" ]]; then
  run sudo python3 "$PROJECT_DIR/scripts/hardware_setup.py" boot-gpio \
    --config "$LOCAL_INI" --install "${BOOT_CONFIG:-/boot/firmware/config.txt}"
else
  [[ -n "$BOOT_CONFIG" ]] || die "no config.txt under /boot/firmware or /boot — is this a Raspberry Pi?"
  BOOT_GPIO_RESULT="$(sudo python3 "$PROJECT_DIR/scripts/hardware_setup.py" boot-gpio \
    --config "$LOCAL_INI" --install "$BOOT_CONFIG")" \
    || die "could not install the boot-time GPIO block into $BOOT_CONFIG"
  echo "    $BOOT_GPIO_RESULT"
  if [[ "$BOOT_GPIO_RESULT" == *updated* ]]; then REBOOT_NEEDED=1; fi
fi

# --- 5c. Retire every previously installed COATHEAL service iteration.
#         Wildcard sweep so old iterations are caught regardless of the unit
#         names they used. Data and logs are never touched. Deliberately the
#         last step before the new units go in: everything that can fail
#         (pull, build, config, GPIO block) has already succeeded.
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
if [[ "$REBOOT_NEEDED" == "1" ]]; then
  printf '\n\033[1;33m  REBOOT REQUIRED: the boot-time GPIO block in %s changed.\033[0m\n' "$BOOT_CONFIG"
  printf '\033[1;33m  Until the Pi reboots, heaters/drivers are only safe once the service claims them.\033[0m\n'
fi
if [[ -n "$FLIGHT_VIOLATIONS" ]]; then
  printf '\n\033[1;33m  BENCH VALUES IN %s -- not a flight deploy:\033[0m\n' "$LOCAL_INI"
  while IFS= read -r violation; do
    printf '\033[1;33m    %s\033[0m\n' "$violation"
  done <<< "$FLIGHT_VIOLATIONS"
fi
printf '\033[1;32m================================================================\033[0m\n'
