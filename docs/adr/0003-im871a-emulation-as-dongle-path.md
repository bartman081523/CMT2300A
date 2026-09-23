# 0003 — Emulate an IMST iM871A dongle instead of a custom ASCII protocol

## Status

Accepted, 2026-09-23 (Stufe A implemented, commit c211645)

Tags: `protocol`, `integration`, `wmbusmeters`

## Context

wmbusmeters should consume received wM-Bus frames from this radio over a
serial port. Three consumption paths existed:

1. The production rtlwmbus pipeline already consumes an **ASCII feed**
   (`C1;1;1;<ts>;<rssi>;0;0x<hex>`, grammar of
   wmbusmeters-`checkRTLWMBUSFrame`) from `[env:esp32s3]` via stdin —
   RX only, no daemon device abstraction, no TX command path.
2. A **custom protocol** with a custom wmbusmeters driver — requires
   patching the daemon upstream.
3. **Emulate a dongle the daemon already supports natively**: the IMST
   iM871A (`device=<tty>:im871a[<uid>]`, 57600 8N1, binary HCI frames).

The daemon's iM871A driver is binary-HCI with a strict detect sequence:
GET_DEVICEINFO / GET_CONFIG / SET_CONFIG responses must arrive byte-exact
within the detect window, and the first frame in the detect buffer must
not be a stray RX indication (permanent tty-ignore on failure).

## Decision

Build `[env:im871a]` — the same firmware with `CMT_HCI_IM871A=1` — that
emulates the iM871A: HCI frame parser FSM + RSP responses + RX IND
emitter + daemon-TX path (`src/im871a_hci.cpp`). The ASCII feed stays the
production path for `[env:esp32s3]`; the emulation is the dongle-grade
path (native daemon integration, RX + TX).

## Consequences

- wmbusmeters needs **no patch**: the board is specified as
  `device=/dev/ttyACM0:im871a[<uid>]` and treated like the commercial
  dongle (meter match, RSSI, JSON telegrams, TX).
- The shim is protocol-pinned to the daemon's parser: detect gates
  (`sRadioOn` boot-silent, 400 ms mute window), mandatory config-padding
  byte, CRC-less IND payload with re-inserted `L`, TX payload verbatim.
  A daemon update that changes these behaviours will need a shim update.
- The HCI line is exclusive at 57600 — debug prints must stay gated
  (only the boot banner prints before a client connects).

## Evidence

- `platformio.ini:155-193` — `[env:im871a]` with `CMT_HCI_IM871A=1`,
  `monitor_speed=57600`.
- `src/im871a_hci.cpp` (whole module; boot banner gate at :216) — FSM,
  RSP/IND emitters, TX path.
- `git show --stat c211645` — "Stufe A: im871a-HCI-Shim (RX+TX bewiesen)";
  body records wire test 6/6, TX receipt via the production RTL pipeline,
  13 meters decoded, false-fire 0 (verified 2026-09-23).
- `platformio.ini:71-80` — `[env:esp32s3]` feed flags (`CMT_SWFRAM=1`,
  `CMT_WMBUS_FEED=1`, `CMT_WMBUS_REPAIR=1`), the ASCII-path counterpart.

## Alternatives considered

- **Custom daemon driver** — rejected: requires upstream patching and a
  maintenance fork; emulation rides on a driver the daemon already ships.
- **ASCII feed as dongle replacement** — rejected for this purpose: no TX
  command path, no native meter/UID integration; kept as-is for the
  production stdin pipeline (see ADR 0006 for the feed's BOK gate).
- **Buy/use a real iM871A** — rejected: the E49 (CMT2300A) is the
  available radio, and matching the dongle's protocol makes the board a
  drop-in.