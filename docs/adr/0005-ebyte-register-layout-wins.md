# 0005 — The EBYTE register layout wins over the datasheet

## Status

Accepted, 2026-09-23

Tags: `driver`, `registers`, `rf`

## Context

The CMT2300A datasheet (Rev 1.7, PDF in repo root) and the EBYTE reference
code (`ebyte_e49x.c`, vendored in a gitignored local dir) disagree on
several register behaviours. The E49 silicon is matched to the EBYTE
firmware's expectations; following the datasheet on the divergent points
silently breaks the driver (wrong state readback, un-cleared IRQs, stale
frequency). Observed divergences are documented in README "What the EBYTE
reference taught us" and in code comments.

Key examples (see README for the full list): state readback in
`0x61<3:0>`; IRQ flags split over `0x6A` (low) / `0x6D` (high) with
write-1-to-clear at `0x6A`/`0x6B`; state commands to `0x60` using the
EBYTE bit pattern; soft reset `0x7F=0xFF` + 20 ms; mandatory PLL relock
(bit 5 of `0x62`) after every retune; FIFO merge (bit 1 of `0x69`).

## Decision

The driver follows the **EBYTE `ebyte_e49x.c` register layout wherever it
diverges from the datasheet**; the datasheet remains the reference for
everything EBYTE code does not touch. The register-map *payloads* (RFPDK
profiles, `include/cmt2300a_config.h`) come from CMOSTEK RFPDK exports,
not from hand-derived datasheet values — see `tools/rfpdk-exp-format.md`.

## Consequences

- New register work must check both sources and record which one was
  followed (the README divergences list is the running record).
- Register exports from RFPDK are treated as spec, reverse-engineered
  where the format is undocumented (`tools/rfpdk-exp-format.md`).

## Evidence

- README.md "What the EBYTE reference taught us" — the divergence list.
- `include/cmt2300a_config.h:92` — OMS T1 profile selection with the
  RFPDK-export reference.
- `src/cmt2300a_spi.cpp:376-412` — `kBands` band table reverse-engineered
  from EBYTE STM8 demo register exports.
- `git show --stat ac43acc` — snapshot containing the divergent-register
  implementation (verified 2026-09-23).

## Alternatives considered

- **Datasheet-first** — rejected empirically: datasheet behaviour on the
  divergent points produced non-functioning state/FIRQ/PLL handling on
  this silicon.
- **CMOSTEK reference code** — not available at the register level; the
  EBYTE code is the shipped, verified alternative.