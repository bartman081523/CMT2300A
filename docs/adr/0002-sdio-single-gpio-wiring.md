# 0002 — SDIO carried on a single GPIO, resistor-free on the S3 wiring

## Status

Accepted, 2026-09-23

Tags: `hardware`, `wiring`, `spi`

## Context

The CMT2300A uses a 4-wire SPI in which SDIO is bidirectional. The E49
module breaks SDIO out as two pins (module MISO + MOSI), so the host must
rejoin them. Two wiring variants exist:

1. The CMOSTEK 4-wire-mode datasheet wiring: both module pins onto one
   host GPIO with a 1 kΩ/10 kΩ divider (documented in README "Pin
   mapping", used on the classic-ESP32 testboard: MISO 19 + MOSI 23
   joined).
2. A direct join without resistors.

The S3 board (E49 dongle-header wiring, `platformio.ini:135-144`) drives
MISO and MOSI onto a **single** GPIO 6 with no divider, and this firmware
has operated that wiring live: HCI wire test 6/6 byte-exact and the
wmbusmeters integration soak (13 meters decoded) ran on it
(commit c211645, 2026-09-22).

## Decision

Treat the **resistor-free direct join on a single GPIO** as the verified
wiring for the S3/dongle-header setup, and keep the datasheet 1 kΩ/10 kΩ
divider as the documented alternative for the classic-ESP32 testboard
(README pin table). Wiring details live in
[`docs/wiring-wmbusmeters.md`](../wiring-wmbusmeters.md) (canonical).

## Consequences

- One fewer resistor and no divider error budget on the S3 path; the
  join works because the firmware drives the bus in half-duplex phases
  (FIFO merge + register access never overlap driver phases).
- Anyone reproducing on a classic ESP32 devkit should follow the README
  divider variant unless they re-verify the direct one.
- The direction-switching mechanism (FIFO_MERGE bit 1 of 0x69, plus the
  half-duplex SPI access pattern) is unchanged by this decision.

## Evidence

- `platformio.ini:135-144` — S3 pin flags: `CMT_PIN_SCK=7`,
  `CMT_PIN_MISO=6`, `CMT_PIN_MOSI=6` (same GPIO), CSB 4, FCSB 5.
- README.md "Pin mapping" — classic ESP32: SDIO as "MISO 19 + MOSI 23
  joined (1kΩ/10kΩ divider)" with the datasheet reference.
- `git show --stat c211645` — commit containing the S3 wiring env used
  for the wire test and soak (verified 2026-09-23).

## Alternatives considered

- **Divider on the S3 too** — rejected for this setup: the direct join is
  proven in live operation; adding the divider would change a verified
  path without benefit.
- **Bit-banged SDIO with explicit direction pin** — not applicable: the
  E49 has no separate direction signal; the join is done on the host
  side.