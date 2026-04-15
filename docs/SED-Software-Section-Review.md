# SED §4.8 (Software Design) — Deprecation Review

Comparison of **BX38_COATHEAL_SED_v2-0_08Mar2026.pdf §4.8** (pages 70–73) against the current implementation in [onboard/](../onboard/) and [ground-station/](../ground-station/), using [docs/architecture.md](architecture.md) as the authoritative current design.

> Note: the separate `PDR_Software.pdf` is image-only (no text layer) and was not usable for comparison.

---

## Deprecated / outdated items in SED §4.8

### 1. Thermal & phase model (largest drift)

- SED describes only 3 informal phases ("heat to 70 °C ascent / maintain float / off descent") with manual FORCE_START/STOP.
- Current FSM has **5 states**: `ASCENT_HOLD (−30 °C)` → `ACTIVATION_RAMP (0.85 °C/s → +70 °C)` → `FLOAT_HOLD (+70 °C, 90 min)` → `DESCENT_FLOOR (−20 °C)` → `STOPPED` (see [architecture.md](architecture.md#L105-L135)).
- No mention of **PID control**. Current uses **9 sample PIDs + 1 box PID**.
- No mention of the **HeaterScheduler** power/count constraint (max 4 active, 40 W budget).
- Missing commands: `RESET_CTRL`, `SHUTDOWN_SAFE`.

### 2. Transport / link layer

- SED says "TCP/IP over E-Link, newline-delimited records" as a single channel.
- Current splits into **three channels**:
  - TCP **:4000** telemetry (Pi → GS, Pi is client)
  - TCP **:5000** commands (Pi is server)
  - UDP **:4100** discovery beacons
- No mention of the **durable disk-backed telemetry queue**, per-frame **ACK** protocol, or **72 h / 8 GiB replay-on-reconnect**.
- No command vocabulary documented (PING / STATUS / FORCE_START / FORCE_STOP / RESET_CTRL / SHUTDOWN_SAFE).
- No auto-discovery, reconnect, or failover group (A/B/C/D) behavior.

### 3. Sensor & interface table (Table 4-6)

| Item | SED says | Current |
|---|---|---|
| UV sensor | Explicit I²C UV sensor | Not present in architecture — confirm if descoped |
| RTC bus | SPI (per block-diagram text) | **I²C** |
| Ambient | "T/P/humidity" (generic) | **BME280** (T/P/humidity) |
| Temperature sensors | "SPI" generic | **PT100 × 9 samples + 1 box** via SPI |
| ADS1115 ADC | Not mentioned | Present on I²C |
| USB storage | "USB Stick via MSC" protocol | Mounted filesystem (mirror.csv), not MSC |
| OBC platform | Not specified | **Raspberry Pi 4**, C++17 |

### 4. Telemetry packet format (§4.8.d)

- SED example: `SEQ, TIMESTAMP (+RTC-valid flag), T_ambient, P_ambient, UV, T_box, HEATER_DUTY, STATUS{SD_OK,USB_OK,I2C_OK,SPI_OK,LINK_OK}`.
- Current `TelemetryRecord` carries **9 sample temps, 10 heater duties, phase enum, scheduler state, link/queue statistics**.
- The SED STATUS bitfield definition is a stale subset.

### 5. Ground station description

- SED: one paragraph — "parse / compute / display readable graph / send commands."
- Current: **PyQt6** application with:
  - Two-thread model (main Qt thread + `TelemetryReceiver` QThread)
  - Qt queued-connection signals (`packet_received`, `connection_changed`, `log_message`)
  - Dock panels: Control, Plot tabs (PyQtGraph), Values, Log
  - CSV logging on GS side
  - One-shot TCP command sender with 3 s timeout

### 6. Missing entirely from SED §4.8

- Auto-discovery + reconnect + failover groups A/B/C/D (commits `c5abb82`, `bd77364`).
- Simulated vs GPIO `PwmController` abstraction.
- Override-flag pattern for safe command application at tick boundaries.
- Configuration via `config/onboard.*.ini`.
- `SystemController` 10-step tick loop sequence.

---

## Suggested rewrite outline for §4.8

1. **Purpose** — onboard + GS (expanded command list).
2. **Platform** — RPi 4, C++17, Python 3 / PyQt6.
3. **Onboard architecture** — SystemController tick loop (10 steps) + subsystem block diagram.
4. **Phase FSM** — 5 states, thresholds, transition triggers.
5. **Thermal control** — 9 sample PIDs + 1 box PID + HeaterScheduler (4 active / 40 W).
6. **Interfaces table** — corrected (RTC=I²C, PT100×9 SPI, UV only if kept).
7. **Telemetry record schema** — full field list with units.
8. **Link layer** — 3-port plan, UDP discovery, ACK protocol, durable queue, replay policy.
9. **Command protocol** — full vocabulary + safety semantics.
10. **Ground station** — threading model + GUI layout.
11. **Storage** — SD primary + USB mirror (CSV), rotation/retention.
12. **Failure modes & degraded-mode behavior**.
