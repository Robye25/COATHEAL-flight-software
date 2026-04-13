# COATHEAL ground-station firewall/profile auto-configure helper.
#
# Run elevated. Idempotent — safe to re-run.
#
#   powershell -ExecutionPolicy Bypass -File scripts/configure_firewall.ps1
#
# Actions:
#   1. Flip every adapter holding a 169.254.x.x (link-local) IPv4 address
#      from Public -> Private so inbound TCP/UDP + ICMP are allowed.
#   2. Add inbound firewall rule "COATHEAL Telemetry" (TCP 4000) if absent.
#   3. Add inbound firewall rule "COATHEAL Discovery" (UDP 4100) if absent.
#
# Prints one line per step: OK / SKIP(already) / FAIL(reason). Exit 0 on
# success, non-zero if any step failed.

$ErrorActionPreference = "Continue"
$failed = 0

function Step-Result {
    param([string]$Label, [string]$Status)
    Write-Host ("{0}: {1}" -f $Label, $Status)
}

# ── 1. Link-local adapters -> Private ──────────────────────────────────────
try {
    $llAddrs = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
        Where-Object { $_.IPAddress -like "169.254.*" }
    if (-not $llAddrs) {
        Step-Result "link-local profile" "SKIP(no 169.254.x.x adapter)"
    } else {
        foreach ($addr in $llAddrs) {
            $idx = $addr.InterfaceIndex
            try {
                $prof = Get-NetConnectionProfile -InterfaceIndex $idx -ErrorAction Stop
                if ($prof.NetworkCategory -eq "Private") {
                    Step-Result ("link-local if{0} ({1})" -f $idx, $addr.IPAddress) "SKIP(already Private)"
                } else {
                    Set-NetConnectionProfile -InterfaceIndex $idx -NetworkCategory Private -ErrorAction Stop
                    Step-Result ("link-local if{0} ({1})" -f $idx, $addr.IPAddress) "OK"
                }
            } catch {
                Step-Result ("link-local if{0}" -f $idx) ("FAIL({0})" -f $_.Exception.Message)
                $failed++
            }
        }
    }
} catch {
    Step-Result "link-local profile" ("FAIL({0})" -f $_.Exception.Message)
    $failed++
}

# ── 2 & 3. Firewall rules ──────────────────────────────────────────────────
function Ensure-Rule {
    param(
        [string]$DisplayName,
        [string]$Protocol,
        [int]$LocalPort
    )
    try {
        $existing = Get-NetFirewallRule -DisplayName $DisplayName -ErrorAction SilentlyContinue
        if ($existing) {
            Step-Result $DisplayName "SKIP(already)"
            return
        }
        New-NetFirewallRule -DisplayName $DisplayName `
                            -Direction Inbound `
                            -LocalPort $LocalPort `
                            -Protocol $Protocol `
                            -Action Allow `
                            -Profile Any `
                            -ErrorAction Stop | Out-Null
        Step-Result $DisplayName "OK"
    } catch {
        Step-Result $DisplayName ("FAIL({0})" -f $_.Exception.Message)
        $script:failed++
    }
}

Ensure-Rule -DisplayName "COATHEAL Telemetry" -Protocol TCP -LocalPort 4000
Ensure-Rule -DisplayName "COATHEAL Discovery" -Protocol UDP -LocalPort 4100

if ($failed -gt 0) { exit 1 } else { exit 0 }
