# COATHEAL Onboard systemd units

This directory contains the systemd unit files that run the onboard flight
software on the Pi. Installation is driven by
`scripts/install_onboard_service.sh`; uninstall by
`scripts/uninstall_onboard_service.sh`.

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
