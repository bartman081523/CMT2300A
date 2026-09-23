# 0006 — False-fire zero: only CRC-clean frames leave the firmware

## Status

Accepted, 2026-09-23

Tags: `protocol`, `quality`, `wmbusmeters`

## Context

A "false fire" is a frame the consumer (wmbusmeters) processes although it
was not a clean on-air frame — noise decoded as a telegram, or a bit-error
frame repaired into a wrong-but-CRC-valid variant. Consumers cannot see
the difference once a frame is forwarded. The production pipeline's
decoding metrics (meter match rate, false-fire count) are the acceptance
metering for this firmware, so the firmware side must guarantee the
gate, not hope the consumer filters.

The iM871A daemon also tolerates CRC-failed frames ("wrong crc" warning
only) — forwarding them would still produce false fires.

## Decision

**Only CRC-clean frames are ever forwarded**, in both consumer paths:

1. `[env:esp32s3]` ASCII feed (`CMT_WMBUS_FEED=1`): frames are BOK-gated
   (block-CRC ok) before writing a feed line. Repaired frames count as
   BOK only if the EN 13757 block-CRC chain validates after repair
   (`CMT_WMBUS_REPAIR=1`).
2. `[env:im871a]` HCI INDs: BBAD frames are never sent as INDs, even
   though the daemon would process them with a warning.

Measured: 0 false fires across the Stufe-A soak (commit c211645 body).

## Consequences

- Frames with correct sync but failed CRC are dropped silently — this is
  the price of the policy; repair exists to recover what can be verified.
- The false-fire metric stays a clean RF-path indicator (path quality
  shows up as *missed* frames, not as fabricated ones).
- Any future forwarding path (e.g. debug taps) must respect the gate.

## Evidence

- `platformio.ini:76` — `CMT_WMBUS_FEED=1` (BOK-gated feed).
- `platformio.ini:80` — `CMT_WMBUS_REPAIR=1` (repair verified per the
  EN13757 block-CRC chain).
- `src/im871a_hci.cpp` — IND emitter sends CRC-ok frames only.
- `git show --stat c211645` — body: "13 qsmoke-Meter dekodiert, False-Fire 0"
  (verified 2026-09-23).

## Alternatives considered

- **Forward BBAD frames with a warning** (daemon-tolerated) — rejected:
  converts RF noise into decoder output and poisons the acceptance metric.
- **Repair-then-forward unconditionally** — rejected: repair is only
  trustworthy once the block-CRC chain validates; forwarding unvalidated
  repairs is a false fire by definition.