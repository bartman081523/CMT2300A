# Architecture views

Mermaid diagram hub. Each view lives in its own file; every view
**complements** the textual documentation — facts are not duplicated here,
the prose sources stay canonical:

| Canonical prose source | What it owns |
|---|---|
| [README.md](../../README.md) | capabilities overview, pin mapping, EBYTE divergences |
| [docs/agents/project-specs.md](../agents/project-specs.md) | build/flash commands, full capability detail, data-flow prose |
| [wiring-ESP32.md](../../wiring-ESP32.md) | dongle/STM8 two-phase wiring plan |
| [docs/wiring-wmbusmeters.md](../wiring-wmbusmeters.md) | verified E49→S3 wiring for the wmbusmeters path |
| [docs/adr/](../adr/) | why the architecture is as it is |

## Views

| View | File | Shows | Complements |
|---|---|---|---|
| System context | [`system.md`](system.md) | the three firmware modes and their consumers (daemon, feed pipeline, WebUI) around one radio | README "What it does" (prose), ADR 0003 |
| HCI handshake | [`hci-handshake.md`](hci-handshake.md) | the iM871A detect sequence byte-for-byte and the RX-indication/TX loop | project-specs "iM871A dongle emulation" prose, ADR 0003 |
| RX data flow | [`rx-dataflow.md`](rx-dataflow.md) | on-air frame → hardware classification → verdict → BOK gate → feed/IND, incl. repair | project-specs data-flow section, ADR 0006 |
| Module graph | [`modules.md`](modules.md) | source-file dependency graph of the firmware | project-specs "Source Layout" |

No diagrams existed in the repo before this hub (checked 2026-09-23) —
nothing was migrated or duplicated.