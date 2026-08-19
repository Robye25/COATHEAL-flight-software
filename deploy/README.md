# COATHEAL Onboard systemd units

This directory contains the systemd unit files that run the onboard flight
software on the Pi. Installation is driven by
`scripts/install_onboard_service.sh`; uninstall by
`scripts/uninstall_onboard_service.sh`.

> **One-command deployment:** operators normally never run the installer
> directly — `coatheal-deploy` (see `deploy_onboard.sh` in the repo root and
> the README's *Deployment quickstart*) pulls, cleans up previous
> iterations, migrates the config, rebuilds, and drives this installer.

## Units

| Unit                              | Purpose                                                              | Enabled by installer? |
|-----------------------------------|----------------------------------------------------------------------|-----------------------|
| `coatheal-onboard.service`        | Flight profile; `COATHEAL_ENV=flight`; uses `onboard.example.ini`.   | Yes                   |
| `coatheal-onboard-debug.service`  | Debug profile; `COATHEAL_ENV=debug`; uses `onboard.debug.ini`. Conflicts with flight (they share port 5000). | No |
| `coatheal-link-watch.path`        | Watches `/sys/class/net` for NIC up/down / dongle replug.            | Yes                   |
| `coatheal-link-watch.service`     | Oneshot fired by the `.path`; restarts the onboard, cool-down 10 s.  | No (triggered)        |

## Which unit to enable

- Flight / integration / nominal: `coatheal-onboard.service` (default).
- Manual debug sessions on the bench: disable flight, enable the debug unit.

## Everyday ops

Watch onboard logs live:

```
journalctl -u coatheal-onboard -f
```

Watch link-watch activity:

```
journalctl -u coatheal-link-watch -f
```

Verify link-watch is armed:

```
systemctl status coatheal-link-watch.path
```

The `.path` unit should show `active (waiting)` and list the watched path.

## Swap profiles without editing files

```
sudo systemctl disable --now coatheal-onboard.service
sudo systemctl enable  --now coatheal-onboard-debug.service
```

Back to flight:

```
sudo systemctl disable --now coatheal-onboard-debug.service
sudo systemctl enable  --now coatheal-onboard.service
```

The `Conflicts=` directive on the debug unit also causes systemd to stop
the flight unit automatically when debug is started, so the two will never
race for TCP port 5000.

## Crash / link-flap behaviour

- `Restart=always`, `RestartSec=2`: crashes come right back.
- `StartLimitIntervalSec=300`, `StartLimitBurst=20`: tolerates frequent
  restarts during integration without systemd giving up.
- `WatchdogSec=10`: the main loop pings the watchdog every tick; systemd
  SIGKILLs after 10 s of silence (BEXUS User Manual §5.9).
- `coatheal-link-watch.service` uses a `/run/coatheal-link-watch.cooldown`
  timestamp file to refuse to fire more than once per 10 s. This is what
  prevents a restart -> NIC churn -> path trigger -> restart loop.

## Storage layout

Default log paths (from `config/onboard.example.ini`, relative to the
service's working directory, `/bexus/code/coatheal`):

| Path | Purpose |
|---|---|
| `logs/onboard_primary.csv` | Primary telemetry CSV (SD card) |
| `logs/onboard_usb_mirror.csv` | Secondary CSV mirror (USB drive) |
| `logs/telemetry-queue/` | Durable disk queue (survives restarts) |

For flight, point `storage.primary_log_path` at the SD card and
`storage.secondary_log_path` at a USB drive. `StorageManager` writes both
independently (`onboard/src/storage_manager.cpp`) — if one fails to open or
write, logging continues on the other.

## Ground-station firewall (Windows)

Run `ground-station/scripts/configure_firewall.ps1` elevated (or use the
ground station launcher's one-click firewall option — see the root
README's *Deployment quickstart*). It is idempotent and:

1. Flips any adapter holding a `169.254.x.x` link-local address from
   Public to Private, so inbound traffic is allowed.
2. Adds an inbound rule for TCP 4000 (telemetry, the Pi connects out to
   this port).
3. Adds an inbound rule for UDP 4100 (discovery beacons).
