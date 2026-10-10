# E-Link Budget (24 kbps)

Owner rule, 2026-09-15: **all COATHEAL traffic on the E-Link stays at or below
24 000 bit/s in every 1-second window** — both directions, every byte on the
wire: Ethernet, IP and TCP/UDP headers, padding, FCS, preamble and
inter-frame gap. Telemetry, ACKs, commands, replies and discovery all count.

Neither side can see the other's traffic in time to react, so the cap is split
into fixed shares that each side enforces on its own:

| Share | Bytes in any 1 s | Who enforces | What it covers |
|---|---|---|---|
| Onboard | 1 600 | `LinkBudget` in `coatheal_onboard` | everything the onboard sends, plus the ground station's automatic answers to it (TCP ACKs, ACK lines, HELLO answers) and the resets our kernel sends back after an abort |
| Ground station | 1 150 | `app/link_budget.py` | command exchanges (connection set-up, request, teardown), discovery beacons and probes, closing a telemetry connection that went quiet |
| Unaccounted | 250 | — | traffic neither side schedules: ARP, and the rare frames listed under [Limits](#limits) |
| **Total** | **3 000** | | **24 kbps** |

Two layers keep to it. The **ledger** on each side schedules that side's own
traffic so that it fits ([Ledger](#ledger)). The **hard cap** is a kernel
shaper on each side's E-Link port that no program can talk its way past
([Hard cap](#hard-cap)): whatever the ledger does not know about — another
program, a retransmission, a bug — still cannot put more than 24 kbps on the
wire.

## Byte model

Sizes are on-wire bytes: the Ethernet frame (header 14 B, minimum 60 B) plus
24 B for preamble/SFD, FCS and inter-frame gap.

| Item | Bytes |
|---|---|
| TCP segment carrying `n` payload bytes (`n` ≤ 524) | `66 + n + 24` |
| Pure TCP ACK, FIN | `90` |
| SYN or SYN-ACK | `98` |
| Reset | `84` |
| UDP datagram carrying `n` bytes | `max(60, 42 + n) + 24` |

`66` is Ethernet 14 + IPv4 20 + TCP 32 (Linux sends the timestamp option).
The E-Link port runs at MTU 576 under the hard cap, so one segment carries at
most 524 B; longer payloads are split into several segments, each with its
own header.

## Ledger

Each side keeps a ledger of charges. A charge is placed **before** its bytes
can reach the wire and counts until one second after it is released; an open
hold counts for as long as it is open. Something may be sent only when the
bytes counting now plus the new charge fit in the share. Charges wait by
priority: a lower-priority sender never takes bytes a higher-priority waiter is
queued for.

A charge covers the worst case of what it can cause, including what the
kernels answer on their own, and is released only after the last of those
bytes can have been emitted. Bytes are refunded only once they provably never
reached the wire. Together this is what turns "the ledger never exceeds the
share" into "no 1-second window on the wire exceeds the share".

## Hard cap

On each side the kernel shapes **everything** that machine sends on its
E-Link port with a token bucket (`tc` `tbf`, installed by
`scripts/link_cap.sh`). A bucket of `B` bytes refilled at `R` bytes per second
lets at most `B + R` bytes out in any 1-second window, whoever sends them:

| Side | Rate `R` | Bucket `B` | Busiest second |
|---|---|---|---|
| Onboard port | 800 B/s (6.4 kbit/s) | 1 000 B | 1 800 B |
| Ground station port | 500 B/s (4 kbit/s) | 700 B | 1 200 B |
| **Both directions** | | | **3 000 B = 24 kbps** |

Bytes are counted as in the [byte model](#byte-model): the shaper adds the
24 B of preamble, FCS and inter-frame gap to every frame and counts a short
frame as 84 B. While a port is capped it runs at **MTU 576**, so that no frame
(614 B on the wire) is larger than a bucket — a larger one could never pass. A
frame that would have to wait more than a second is dropped.

`python3 scripts/link_cap_selftest.py` proves it on the machine it runs on:
two throwaway network namespaces, both shapers installed by the same script,
both ends flooding UDP of every size and a TCP stream each way at once. On
the development machine (Linux 6.14, 2026-10-05) the busiest second on the
wire was 1 582 B from the onboard port, 1 152 B from the ground port and
2 734 B (21.9 kbps) in all; without the shapers the same load reaches
gigabits and the test fails. Run it on the Pi and on the ground station
before relying on the cap there.

### The ledgers stay inside it

A shaper delays what does not fit, and a delayed ACK would trip the
[180 ms deadline](#no-retransmissions). So each ledger also **models the
bucket on its own port** and lets a charge go only while the model holds the
bytes its own side sends for it (the frame and our ACK of the ACK line; a
reply chunk with its header; a command's SYN, request, FIN and ACKs). The
model refills 3 % slower than the kernel's bucket. What cannot wait is taken
from the model without asking and leaves a debt (never more than one bucket,
as in the kernel) that later charges wait out:

- onboard: what its kernel sends for a command the ground station opened
  (SYN-ACK, ACK, FIN, ACK: 368 B) and its resets;
- ground station: every telemetry ACK line and HELLO answer, and the SYN-ACK
  of the telemetry connection.

On the ground station a command also leaves 260 B in the model for the ACK
lines of the next tick; safety commands (`HEATERS_OFF`, `STEPPER_STOP`, …) do
not keep that reserve. Both models run whether or not the kernel shaper is
installed, so the bench behaves like the flight.

Checked end to end on 2026-10-05 with the flight binary (simulated sensors)
and the ground station's receiver in two network namespaces, both shapers
installed, a command every 3–4 s throughout:

| Run | Telemetry / outage / replay | Frames arrived | Commands answered | Busiest second |
|---|---|---|---|---|
| 1 | 60 s / 120 s / 240 s | 423 of 423 | 80 of 80 | 2 237 B (17.9 kbps) |
| 2 (final build) | 40 s / 60 s / 160 s | 265 of 265 | 54 of 54 | 2 269 B (18.2 kbps) |

The backlog of each outage was replayed in full. A command took under 1 ms
at the median on that cable and at most 1.5 s (a 511 B `STATUS` reply waiting
for tokens). Neither shaper dropped a frame, and one frame in 3 661 had to
wait in one. The Pi's own kernel and the real E-Link have not been measured
yet.

### What it costs

A token bucket bounds a second by bucket + rate, but sustains only the rate:
1 300 B/s for both directions together (10.4 kbit/s), against 3 000 B in the
busiest second. A live frame with its ACKs takes about 440–500 B/s of the
onboard's 800 and 130 B/s of the ground station's 500. The rest paces
everything else:

- backlog replay moves at about two frames in three ticks when nothing else
  is sent. A replayed frame goes right after the live one, so the two (each
  the segment plus a 90 B ACK) must fit the 1 000 B bucket together: with
  compressed frames above about 340 B nothing would be replayed any more.
  Today's are 270–320 B;
- commands go about one every 1.5 s when sent back to back; a reply longer
  than one segment (520 B) waits for tokens between chunks;
- a second ground-station process, `ssh` or a file copy on the E-Link port
  now takes its bytes out of the same bucket, and the telemetry stalls
  instead of the cap breaking.

### Switching it on

| Where | Command |
|---|---|
| Pi, flight | `coatheal-deploy --flight` (switches it on and refuses to finish without it), or `coatheal-link-cap on` |
| Pi, bench LAN | `coatheal-link-cap off` — the cap takes the whole port, `ssh` and `git` included |
| Ground station (Linux), flight | `./COATHEAL-GroundStation.sh --link-cap <port>`; `--link-cap off` removes it |
| Either | `scripts/link_cap.sh status --role onboard\|ground [--iface <port>]` |

The Pi logs the state at every service start and reports it in `STATUS` as
`link_cap=on:<port>` (`off`; `stale:<port>` when something has reset the
port's MTU since). The console writes `[link cap] …` to its event log for the
port that reaches the onboard. A deploy lifts the cap while it pulls and
builds and puts it back at the end, also when it fails. A re-plugged USB
Ethernet adapter is a new port without a shaper; `coatheal-link-watch`
re-installs it.

## No retransmissions

Linux never retransmits a TCP segment sooner than 200 ms plus the smoothed
round trip after sending it (`TCP_RTO_MIN`; a tail-loss probe for a lone
segment waits at least as long). Whatever the onboard writes — a telemetry
frame, the HELLO line, a reply chunk — must be acknowledged within **180 ms
plus the connection's smoothed round trip** (at most 200 ms of it); otherwise
the connection is reset (`SO_LINGER` 0) before the kernel could retransmit.
A slow link costs a reconnect, never bytes over the cap, so no retransmission
has to be budgeted. `STATUS` counts telemetry frames that missed the deadline
as `link_ack_timeouts`.

After a reset, anything the other side sent before the reset reached it can
still arrive, and our kernel answers each such segment with a reset of its
own. Charges that end with a reset therefore stay held for another 0.5 s plus
a round trip.

### Onboard

| Traffic | Held while it is out | Refunded or released |
|---|---|---|
| Telemetry frame (DATA or EVT line, z1 or plain) | the frame; the ground station's TCP ACK of it and the reset our kernel answers a late one with; the ACK line and our ACK of it (or the reset answering a late one); our own reset | The frame's bytes are released the moment it is written. When the expected ACK line arrives in time, both resets are refunded, and so is the ground station's TCP ACK if the ACK line carried it (the Linux segment counters show one data segment in since the send). The rest is released 10 ms later, after `TCP_QUICKACK` has pushed our ACK out. A late or wrong ACK line resets the connection |
| Telemetry connect | SYN, SYN-ACK and our ACK; the HELLO line, the ground station's ACK of it and the reset for a late one; the answer (up to 16 B) and our ACK or reset for it; our reset | The connect times out after 900 ms, before the kernel would retransmit the SYN. Released when the answer arrives. A HELLO left unanswered keeps its answer and our ACK held until a frame's ACK shows the ground station went past it, or the connection closes |
| Command reply (remote peer) | per chunk of at most 520 B (one segment): the payload, its segment header for every chunk after the first (the ground station holds the first one's), the ground station's ACK of it, our reset and the reset answering a late ACK | The resets are refunded once the chunk is acknowledged. A chunk not acknowledged by the deadline resets the connection. Replies to loopback peers never touch the E-Link and are not paced |
| `ONBOARD_BEACON`, `ONBOARD_HELLO` | the datagram | sent only while no telemetry connection is up |

A z1 DATA frame of 280 B holds 846 B while it is out and still counts 588 B
once it was answered.

Priorities, highest first: command replies, live frame and events, discovery,
backlog replay. A live frame waits for room until 40 % of its tick. Backlog
replay stops early enough that the next tick's live frame still finds its
room within 150 ms of that tick's start. At 1 Hz a replayed frame can start
only in the first 150 ms of a tick. Above 1 Hz nothing is replayed, and slower
ticks stop replay at 70 % (`ReplayDeadline`).

### Ground station

| Traffic | Charge |
|---|---|
| Command exchange | a hold of `920 + len(request line)` placed before connecting: SYN, SYN-ACK, ACK, request, onboard ACK, reply header, FIN, ACK, FIN, ACK. Released 50 ms plus two round trips after a clean close. A failed exchange (timeout or error) is closed with a reset, so the onboard can no longer answer it, and stays held for 0.5 s plus a round trip |
| `GS_BEACON`, `GS_HELLO` | the datagram, once per target address |
| Closing a quiet telemetry connection | the FIN and the onboard's ACK of it (180 B), charged before the close; with no room, the close waits |
| Telemetry ACK lines, HELLO answers | not charged here: the onboard charges them |

Priorities, highest first: safety commands (`HEATERS_OFF`, `STEPPER_STOP`,
`DISARM`, `SHUTDOWN_SAFE`, `RADIO_SILENCE`, `RADIO_RESUME`), other commands,
background polls (`MOTOR_DEBUG` probe, and `TIME_SYNC` every ten minutes,
which never waits for room: a full share postpones it), discovery. A command whose request
line cannot fit the share (longer than 230 B) is refused locally. Exchanges go
one at a time: about one per second.

The ground station answers each telemetry frame with its ACK line before it
writes anything to disk, so that the ACK stays well inside the onboard's
deadline.

## Discovery cadence

| Sender | While no telemetry for 5 s | While telemetry arrives |
|---|---|---|
| Ground station `GS_BEACON` | every 2 s | every 15 s |
| Ground station `GS_HELLO` (legacy) | every 2 s | not sent |
| Ground station `PING` probe | every 2 s | not sent |
| Onboard `ONBOARD_BEACON` | every 2 s while not connected | not sent |
| Onboard `ONBOARD_HELLO` reply | while not connected | not sent |

## Telemetry framing

1. After the TCP connection opens, the onboard sends
   `HELLO,<session_id>,z1:<crc32>` — `crc32` is 8 lowercase hex digits of the
   CRC-32 of `protocol/telemetry-dictionary-z1.txt`.
2. A ground station with a byte-identical dictionary answers `HELLO,z1` within
   1.5 s; one without the dictionary, or with another one, answers
   `HELLO,plain`. Any other answer, or none, leaves the connection in plain
   mode (a ground station from before 2026-09-15 logs the HELLO as one
   unparseable line and does not answer).
3. In z1 mode every line is sent as `Z1,<base64>`: standard base64 (with
   padding) of the raw DEFLATE stream (zlib level 9, window 15 without zlib
   header, memory level 9, preset dictionary = the dictionary file) of the
   exact text line — a DATA line including its `,TX=<age>` stamp, or an EVT
   line — without the newline. The ground station inflates it and handles the
   result exactly like a plain line. ACK lines stay plain.

A typical DATA line shrinks from 1 145 B to 235 B (291 B at most over 644
frames from five bench sessions). The `loss:`/`unc:` keys of the step-loss
protection (2026-10-05) are not in the dictionary and add about 22 B to each
compressed frame; the dictionary was left alone because a ground station with
another dictionary gets no DATA frame at all.

`STATUS` reports `link_codec=<z1|plain|>` (empty while disconnected),
`link_bytes=<counting>/<share>`, `link_ack_timeouts=<n>` (frames whose ACK
missed the deadline since start), `link_cap=<on:port|off|stale:port>` (the
kernel shaper of the [hard cap](#hard-cap)) and `link_tokens=<n>/<bucket>`
(the ledger's model of that shaper's bucket; negative while in debt).

## Replay order

The durable queue keeps every frame the ground station has not acknowledged.
Each tick the onboard sends, while the budget allows:

1. this tick's DATA frame,
2. pending EVT frames, oldest first,
3. backlog frames by bisection: take the longest run of consecutive
   unacknowledged frames (oldest run on a tie) and send its middle frame.

A 15-frame outage replays as 7, 3, 11, 1, 5, 9, 13, then the rest in order:
the middle of the gap first, then the quarters, then the eighths, so an
interrupted replay still leaves an even picture of the outage. Every frame is
acknowledged exactly by `(session, seq)` (EVT frames by `ACK,<session>,0`),
so the order does not matter to the queue; the ground station deduplicates by
the set of `(session, seq)` it has received and inserts replayed points at
their onboard time.

Frames the budget does not let out on their own tick — a live frame that had
to yield to a command reply, for example — simply stay queued and are filled
in by the same bisection.

`tests/unit/test_link_budget.cpp` replays a ten-minute outage at 1 Hz against
the real ledger, queue and drain. In every case below, each live frame leaves
on its own tick:

- The ground station's TCP ACK rides in its ACK line: 136 frames are
  replayed in 180 s, and the busiest second of onboard traffic is 1 166 B.
- The TCP ACK comes as a separate frame every time (the worst case): 136
  frames are replayed in 180 s, and the busiest second is 1 346 B.
- With the hard cap's shaper modelled as well: 120 frames are replayed in
  180 s, the onboard port's busiest second is 916 B, and the kernel's bucket,
  fed what the port sent, never runs dry — nothing of ours waits in it.

## Limits

- **The codec is required.** A plain bench DATA line (about 1 150 B) holds
  about 1 700 B while it is out, more than the whole onboard share. With a
  ground station that does not answer `HELLO,z1`, no DATA frame is sent: the
  onboard logs it once, and `STATUS` shows `link_codec=plain`.
- **1 Hz is the highest rate that carries every frame.** Faster ticks leave
  the frames the budget cannot carry queued, and nothing is replayed until the
  rate is back at 1 Hz or slower.
- **One ground station on the E-Link.** Each ground-station process keeps its
  own 1 150 B share. A second one (the CLI telemetry server next to the GUI, or
  a standby laptop that beacons and probes) adds traffic outside the split.
  With the hard cap on, two processes on one machine share that machine's
  bucket: the cap holds and the telemetry suffers. A second *machine* brings
  a second ground port; cap one ground station only, and keep the other off
  the E-Link.
- **The hard cap needs Linux on both ends.** `scripts/link_cap.sh` uses `tc`.
  A Windows ground station has no such shaper here: its direction is held by
  the ledger alone (the console says so in its event log). The onboard port,
  which carries most of the traffic, is capped either way.
- **The hard cap is switched on by hand** (or by a `--flight` deploy), because
  it takes the whole port. `STATUS`, the service log and the console's event
  log all say whether it is on.
- **The unaccounted 250 B** is for what neither side schedules:
  - ARP, which only runs while no TCP traffic confirms the neighbour;
  - the reset the onboard sends when radio silence starts or a ground-station
    failover drops the telemetry connection;
  - a SYN-ACK slower than the 900 ms connect timeout;
  - anything else the operating system sends on its own. Keep IPv6 neighbour
    discovery and mDNS off the E-Link interface.
