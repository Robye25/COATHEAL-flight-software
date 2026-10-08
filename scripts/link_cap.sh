#!/usr/bin/env bash
set -euo pipefail

# COATHEAL E-Link hard cap (docs/link-budget.md, "Hard cap").
#
# The kernel shapes everything this machine sends on its E-Link port with a
# token bucket, so that no bug, retransmission or other program can put more
# on the wire than this side's part of the 24 kbps:
#
#   role      rate       bucket   busiest second
#   onboard   800 B/s    1 000 B  1 800 B
#   ground    500 B/s      700 B  1 200 B
#                                 3 000 B = 24 000 bit/s
#
# Bytes are on-wire bytes: the frame (at least 60 B) plus 24 B of preamble,
# FCS and inter-frame gap. The port also runs at MTU 576 while capped, so that
# no frame is larger than a bucket.
#
#   link_cap.sh on      persist (systemd, when the unit is installed) and apply
#   link_cap.sh off     persist and remove
#   link_cap.sh apply   apply now (what the unit runs)
#   link_cap.sh clear   remove now
#   link_cap.sh status  what the kernel has; exit 0 only when capped as above
#
#   --role onboard|ground   default onboard
#   --iface <name>          the E-Link port. Default: $COATHEAL_ELINK_IFACE,
#                           else the port that routes to --peer, else the one
#                           carrying 169.254.10.10, else eth0
#   --peer <ip>             the other side's address (finds the port)
#   --state-file <path>     default /run/coatheal-link-cap.state
#
# The cap takes the whole port: ssh, git and apt on it crawl at the same rate.
# Turn it off for bench work on a shared LAN and on again before flight.

ROLE="onboard"
IFACE="${COATHEAL_ELINK_IFACE:-}"
PEER=""
STATE_FILE="${COATHEAL_LINK_CAP_STATE:-/run/coatheal-link-cap.state}"
ENV_FILE="${COATHEAL_LINK_CAP_ENV:-/etc/coatheal/link-cap.env}"
UNIT="coatheal-link-cap.service"

# Keep in step with onboard/include/coatheal/link_budget.hpp and
# ground-station/app/link_budget.py (ground-station/tests/test_link_cap.py
# compares the three).
ONBOARD_RATE_BYTES=800
ONBOARD_BUCKET_BYTES=1000
GROUND_RATE_BYTES=500
GROUND_BUCKET_BYTES=700
LINK_MTU=576
WIRE_OVERHEAD=24      # preamble/SFD, FCS, inter-frame gap
MIN_WIRE_FRAME=84     # 60 B minimum frame + the overhead
QUEUE_MS=1000         # a frame waiting longer than this is dropped

ACTION="${1:-}"
if [[ $# -gt 0 ]]; then shift; fi
# The port chosen when the cap was switched on, unless one is named now.
if [[ -z "$IFACE" && -r "$ENV_FILE" ]]; then
  IFACE="$(sed -n 's/^COATHEAL_ELINK_IFACE=//p' "$ENV_FILE" | tail -n 1)"
fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --role) ROLE="$2"; shift 2 ;;
    --iface) IFACE="$2"; shift 2 ;;
    --peer) PEER="$2"; shift 2 ;;
    --state-file) STATE_FILE="$2"; shift 2 ;;
    *) echo "[link-cap] unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$ROLE" in
  onboard) RATE_BYTES=$ONBOARD_RATE_BYTES; BUCKET_BYTES=$ONBOARD_BUCKET_BYTES ;;
  ground)  RATE_BYTES=$GROUND_RATE_BYTES;  BUCKET_BYTES=$GROUND_BUCKET_BYTES ;;
  *) echo "[link-cap] --role must be onboard or ground" >&2; exit 2 ;;
esac
RATE_BITS=$((RATE_BYTES * 8))
# tc adds one frame's overhead to the burst it is given: the bucket the kernel
# keeps is burst + overhead (measured, 2026-10-05).
TC_BURST=$((BUCKET_BYTES - WIRE_OVERHEAD))

die() { echo "[link-cap] $*" >&2; exit 1; }

SUDO=""
need_root() {
  if [[ $EUID -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "must run as root"
    SUDO="sudo"
  fi
}

resolve_iface() {
  if [[ -z "$IFACE" && -n "$PEER" ]]; then
    IFACE="$(ip -o route get "$PEER" 2>/dev/null | sed -n '1s/.* dev \([^ ]*\).*/\1/p')"
  fi
  if [[ -z "$IFACE" ]]; then
    IFACE="$(ip -o -4 addr show 2>/dev/null | awk '!found && $4 ~ /^169\.254\.10\.10\// {print $2; found = 1}')"
  fi
  [[ -n "$IFACE" ]] || IFACE="eth0"
  [[ -n "$(iface_mtu)" ]] || die "no such interface: $IFACE (pass --iface)"
  [[ "$IFACE" != "lo" ]] || die "refusing to cap the loopback interface"
}

# Asked over netlink, not sysfs: right inside a network namespace as well.
iface_mtu() {
  ip -o link show dev "$IFACE" 2>/dev/null | sed -n '1s/.* mtu \([0-9]*\).*/\1/p'
}

# "<rate bit/s> <bucket B>" of the root tbf on $IFACE, or nothing.
kernel_cap() {
  tc qdisc show dev "$IFACE" 2>/dev/null | awk '
    !found && $1 == "qdisc" && $2 == "tbf" && /root/ {
      for (i = 1; i <= NF; i++) {
        if ($i == "rate") rate = $(i + 1)
        if ($i == "burst") burst = $(i + 1)
      }
      print rate, burst
      found = 1
    }'
}

# tc prints sizes in its own units ("6400bit", "1Kb", "976b", "1000b/1").
to_bits() {
  local v="${1%bit}"
  case "$v" in
    *K) echo $(( ${v%K} * 1000 )) ;;
    *M) echo $(( ${v%M} * 1000000 )) ;;
    *) echo "$v" ;;
  esac
}
to_bytes() {
  local v="${1%%/*}"
  case "$v" in
    *Kb) echo $(( ${v%Kb} * 1024 )) ;;
    *b) echo "${v%b}" ;;
    *) echo "$v" ;;
  esac
}

is_capped() {
  local cap rate burst mtu
  cap="$(kernel_cap)"
  [[ -n "$cap" ]] || return 1
  rate="$(to_bits "${cap%% *}")"
  burst="$(to_bytes "${cap##* }")"
  mtu="$(iface_mtu)"
  # tc reports the bucket it keeps (burst + overhead), rounded to its unit.
  [[ "$rate" == "$RATE_BITS" ]] && (( burst <= BUCKET_BYTES + WIRE_OVERHEAD )) && (( mtu <= LINK_MTU ))
}

write_state() {
  $SUDO sh -c "umask 022; printf '%s\n' '$1' > '$STATE_FILE'" 2>/dev/null || true
}

do_apply() {
  need_root
  resolve_iface
  local prev_mtu
  prev_mtu="$(iface_mtu)"
  # Re-applying must not record the capped MTU as the one to restore.
  if [[ -r "$STATE_FILE" ]]; then
    read -r s_state s_iface _ _ _ s_prev _ < "$STATE_FILE" || true
    if [[ "${s_state:-}" == "on" && "${s_iface:-}" == "$IFACE" && -n "${s_prev:-}" ]]; then
      prev_mtu="$s_prev"
    fi
  fi
  $SUDO ip link set dev "$IFACE" mtu "$LINK_MTU" || die "could not set MTU $LINK_MTU on $IFACE"
  $SUDO tc qdisc replace dev "$IFACE" root tbf \
    rate "${RATE_BITS}bit" burst "$TC_BURST" latency "${QUEUE_MS}ms" \
    overhead "$WIRE_OVERHEAD" mpu "$MIN_WIRE_FRAME" \
    || die "tc could not install the shaper on $IFACE (sch_tbf missing?)"
  is_capped || die "the shaper on $IFACE is not what was asked for: $(tc qdisc show dev "$IFACE")"
  write_state "on $IFACE $RATE_BYTES $BUCKET_BYTES $LINK_MTU $prev_mtu $ROLE"
  echo "[link-cap] $ROLE cap on $IFACE: $RATE_BYTES B/s, bucket $BUCKET_BYTES B, MTU $LINK_MTU" \
       "(at most $((RATE_BYTES + BUCKET_BYTES)) B in any second)"
}

do_clear() {
  need_root
  local prev_mtu=1500
  if [[ -r "$STATE_FILE" ]]; then
    read -r s_state s_iface _ _ _ s_prev _ < "$STATE_FILE" || true
    if [[ "${s_state:-}" == "on" ]]; then
      if [[ -z "$IFACE" ]]; then IFACE="${s_iface:-}"; fi
      if [[ -n "${s_prev:-}" ]]; then prev_mtu="$s_prev"; fi
    fi
  fi
  resolve_iface
  $SUDO tc qdisc del dev "$IFACE" root 2>/dev/null || true
  $SUDO ip link set dev "$IFACE" mtu "$prev_mtu" 2>/dev/null || true
  write_state "off $IFACE"
  echo "[link-cap] cap removed from $IFACE (MTU $prev_mtu)"
}

do_status() {
  resolve_iface
  if is_capped; then
    echo "[link-cap] ON: $ROLE cap on $IFACE: $(tc qdisc show dev "$IFACE" | sed -n 1p)"
    return 0
  fi
  echo "[link-cap] OFF: no $ROLE cap on $IFACE (qdisc: $(tc qdisc show dev "$IFACE" | sed -n 1p);" \
       "MTU $(iface_mtu))"
  return 1
}

have_unit() {
  command -v systemctl >/dev/null 2>&1 && systemctl cat "$UNIT" >/dev/null 2>&1
}

persist() {
  need_root
  $SUDO mkdir -p "$(dirname "$ENV_FILE")"
  {
    echo "COATHEAL_LINK_CAP=$1"
    if [[ -n "$IFACE" ]]; then echo "COATHEAL_ELINK_IFACE=$IFACE"; fi
  } | $SUDO tee "$ENV_FILE" >/dev/null
}

case "$ACTION" in
  apply) do_apply ;;
  clear) do_clear ;;
  status) do_status ;;
  on)
    if have_unit; then
      persist on
      $SUDO systemctl enable "$UNIT" >/dev/null
      $SUDO systemctl restart "$UNIT"
      do_status
    else
      do_apply
    fi
    ;;
  off)
    if have_unit; then
      persist off
      $SUDO systemctl disable --now "$UNIT" >/dev/null 2>&1 || true
    fi
    do_clear
    ;;
  *)
    sed -n '4,33p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
