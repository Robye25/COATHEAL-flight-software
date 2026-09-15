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

## Byte model

Sizes are on-wire bytes: the Ethernet frame (header 14 B, minimum 60 B) plus
24 B for preamble/SFD, FCS and inter-frame gap.

| Item | Bytes |
|---|---|
| TCP segment carrying `n` payload bytes (`n` ≤ 1 448) | `66 + n + 24` |
| Pure TCP ACK, FIN | `90` |
| SYN or SYN-ACK | `98` |
| Reset | `84` |
| UDP datagram carrying `n` bytes | `max(60, 42 + n) + 24` |

`66` is Ethernet 14 + IPv4 20 + TCP 32 (Linux sends the timestamp option).
Payloads over 1 448 B are split into several segments, each with its own
header.

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
| Command reply (remote peer) | per chunk of at most 600 B: the payload, its segment header for every chunk after the first (the ground station holds the first one's), the ground station's ACK of it, our reset and the reset answering a late ACK | The resets are refunded once the chunk is acknowledged. A chunk not acknowledged by the deadline resets the connection. Replies to loopback peers never touch the E-Link and are not paced |
| `ONBOARD_BEACON`, `ONBOARD_HELLO` | the datagram | sent only while no telemetry connection is up |

A z1 DATA frame of 292 B holds 858 B while it is out and still counts 600 B
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
background polls (`MOTOR_DEBUG` probe), discovery. A command whose request
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
frames from five bench sessions).

`STATUS` reports `link_codec=<z1|plain|>` (empty while disconnected),
`link_bytes=<counting>/<share>` and `link_ack_timeouts=<n>` (frames whose ACK
missed the deadline since start).

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
the real ledger, queue and drain. In both cases below, every live frame leaves
on its own tick:

- The ground station's TCP ACK rides in its ACK line: one backlog frame is
  replayed every tick, and the busiest second of onboard traffic is 1 122 B.
- The TCP ACK comes as a separate frame every time (the worst case): 136
  frames are replayed in 180 s, and the busiest second is 1 306 B.

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
- **The unaccounted 250 B** is for what neither side schedules:
  - ARP, which only runs while no TCP traffic confirms the neighbour;
  - the reset the onboard sends when radio silence starts or a ground-station
    failover drops the telemetry connection;
  - a SYN-ACK slower than the 900 ms connect timeout;
  - anything else the operating system sends on its own. Keep IPv6 neighbour
    discovery and mDNS off the E-Link interface.
