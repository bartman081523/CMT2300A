# 0004 — Dongle firmware: SWIM pads only; BSL over USB is unreachable

## Status

Accepted, 2026-09-23 (negative decision — recorded to prevent re-litigation)

Tags: `hardware`, `stm8`, `tooling`

## Context

The E49-900MBL-01 USB dongle routes the CMT2300A SPI lines through an
STM8L151G. Reprogramming the STM8 requires either the UART bootloader
(BSL) or SWIM. The BSL was attempted over the dongle's USB connection via
the CH340X serial bridge — experiments (`tools/bsl_*.py`,
`tools/catch_*.py`) could not reach it: the STM8 reset required to enter
the bootloader is not observable/triggerable over USB, and the bootloader
entry cannot be provoked without it (likely disabled in option bytes).

The three SWIM pads (SWIM/NRST/GND, top-right of the dongle) are the
reliable access path.

## Decision

Flash the dongle's STM8 **only via the SWIM pads** (one-shot: ESP32 as
SWIM master). Do not spend further time on BSL-over-USB without new
hardware access to the reset line. `wiring-ESP32.md` holds the two-phase
plan; `stm8_swim_tool/` is the SWIM-master tool.

## Consequences

- A pin header (Stiftleiste) on the SWIM pads is a prerequisite for
  Stufe B (porting the HCI shim onto the dongle STM8).
- The BSL experiments remain in `tools/` as the record of *why*.

## Evidence

- `Board-Description.md` — dongle anatomy: STM8L151G, CH340X, header
  pinout, the SWIM pads.
- `wiring-ESP32.md` (BSL section) — the constraint and the two-phase plan.
- `tools/bsl_*.py`, `tools/catch_*.py` — the experiments that
  established unreachability (tracked in `git show --stat ac43acc`,
  verified 2026-09-23).

## Alternatives considered

- **BSL over USB with reset tricks** — rejected: exhausted by the
  `tools/bsl_*`/`catch_*` experiments; reset is not reachable over USB.
- **Auto-reset via CH340 DTR/RTS wiring** — rejected: no known route from
  the CH340 to the STM8 NRST on this board (per Board-Description.md
  header/jumper map).