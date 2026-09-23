# 0001 — ADR process for this repository

## Status

Accepted, 2026-09-23 (process bootstrap)

Tags: `process`, `documentation`

## Context

The firmware evolved through long hardware-forensics rounds (register-level
divergences between the CMT2300A datasheet and the EBYTE reference code,
FIFO-drain strategies, HCI detect-window behaviour) where the *reason* for a
design choice lives in round notes and commit bodies, not in any browsable
document. `git log` is colon-titled snapshots (`git show ac43acc`,
`git show c211645`) — good for rollback, poor for "why is it this way".

## Decision

Use Architecture Decision Records in `docs/adr/`, MADR-inspired but lean:

- Thematic numbering (0001 = process, then wiring, protocol, tooling …),
  not chronological.
- Mandatory sections: `## Status`, `## Context`, `## Decision`,
  `## Consequences`, `## Evidence`, `## Alternatives considered`.
- Status is a real `## Status` heading (grep-able), never a bullet.
- Accepted ADRs are never edited in place — changes become a new ADR that
  supersedes the old one; the old one gains `Superseded by NNNN`.
- Negative decisions (rejected paths) are documented like accepted ones.
- Every Evidence section cites only commit hashes verified via
  `git show --stat <hash>` plus `file:line` pointers into the tracked tree.

## Consequences

- Architecture reasoning is greppable and reviewable; supersession keeps
  history intact instead of rewriting.
- Cost: new decisions need a file. Keep them short — the evidence is the
  value, not the prose.

## Evidence

- `git show --stat ac43acc` — snapshot commit containing the divergent-
  register code and STM8 tooling; `git show --stat c211645` — HCI shim
  commit. Both hashes verified 2026-09-23.
- Commit-message convention documented in
  [`docs/agents/project-specs.md` → Commit Message Style](../agents/project-specs.md).

## Alternatives considered

- **Keep rationale in commit bodies only** — rejected: bodies are not
  discoverable by topic, and round notes reference hardware states that no
  longer exist.
- **Long-form architecture doc** (single document) — rejected: merges
  independent decisions; ADRs stay independently supersedeable.