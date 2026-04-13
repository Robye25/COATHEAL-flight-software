# Ground Station — Windows Firewall & Network Profile

The COATHEAL ground station GUI auto-configures the Windows laptop so the
onboard Raspberry Pi can reach it over a direct Ethernet link. If the GUI
detects that configuration is missing on startup, it prompts:

> Allow GS auto-configure firewall? (requires admin)

Clicking **Yes** re-launches `scripts/configure_firewall.ps1` elevated via
the UAC prompt. Clicking **No** continues with a banner explaining that the
Pi may not reach the GS. The decision is remembered for the current session
only — next launch prompts again if the state is still not right.

Use `--no-firewall-check` on `gui_app.py` to skip this entirely.

## What gets installed and why

| Rule / Change                                  | Why                                   |
|------------------------------------------------|---------------------------------------|
| Inbound **TCP 4000** — "COATHEAL Telemetry"    | Onboard streams telemetry to GS.      |
| Inbound **UDP 4100** — "COATHEAL Discovery"    | Discovery broadcasts & responses.     |
| Link-local (`169.254.x.x`) adapter -> Private  | Windows blocks inbound ICMP/TCP on Public profiles even when a per-app rule exists. |

Outbound TCP 5000 (GS dials the onboard command server) is allowed by
Windows default — no rule needed.

## Link-local profile — why flip it

When the GS laptop and the Pi are directly cabled with no DHCP, both sides
pick an IPv4 from `169.254.0.0/16`. Windows classifies such "unidentified"
networks as **Public**, which blocks inbound ICMP and silently drops
inbound TCP even when a per-app firewall rule is in place. Flipping the
link-local adapter to **Private** lets the Pi's pings and connect attempts
land.

## Run the helper manually

From an elevated PowerShell prompt inside `ground-station/`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/configure_firewall.ps1
```

The helper is idempotent: re-running it will not create duplicate rules.
Each step prints `OK`, `SKIP(already)`, or `FAIL(reason)`.

## Remove the rules

```powershell
Remove-NetFirewallRule -DisplayName "COATHEAL Telemetry"
Remove-NetFirewallRule -DisplayName "COATHEAL Discovery"
```

To revert the network profile back to Public:

```powershell
Set-NetConnectionProfile -InterfaceIndex <idx> -NetworkCategory Public
```

(Use `Get-NetConnectionProfile` to list interface indices.)

## Fallback for old Windows without PowerShell NetSecurity

If PowerShell is unavailable or lacks the `NetSecurity` module, the GUI
falls back to `netsh`:

```cmd
netsh advfirewall firewall add rule name="COATHEAL Telemetry" ^
    dir=in action=allow protocol=TCP localport=4000
netsh advfirewall firewall add rule name="COATHEAL Discovery" ^
    dir=in action=allow protocol=UDP localport=4100
```

`netsh` cannot flip a network profile; on such hosts an operator must set
the link-local adapter to Private manually via Settings -> Network.
