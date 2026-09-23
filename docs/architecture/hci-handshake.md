# HCI handshake view (iM871A emulation)

Byte sequences verified against the wmbusmeters iM871A parser and the
on-wire test (`scratches/hci_wire_test.py`, 6/6 byte-exact, 2026-09-22).
Frame grammar: `[0xA5][(ctrl<<4)|ep][msgid][len][payload][crc16?]`,
CRC16 init 0xFFFF / poly 0x8408 LSB-first.

```mermaid
sequenceDiagram
    participant D as wmbusmeters
    participant F as firmware im871a_hci

    Note over F: boot silent — sRadioOn false,<br/>no INDs before first SET_CONFIG
    D->>F: GET_DEVICEINFO_REQ (ep0 msgid 0x0F)
    F-->>D: RSP 0x10: 33 00 15 16 + uid(4, LE) + crc16
    D->>F: GET_CONFIG_REQ (ep0 msgid 0x05)
    F-->>D: RSP 0x06: 12 + linkmode + uid + 00 00 (padding byte mandatory)
    D->>F: SET_CONFIG_REQ (ep0 msgid 0x03, e.g. 0x0A = c1,t1)
    F-->>D: RSP 0x04: payload echo
    Note over F: sRadioOn = true; 400 ms IND mute window starts

    loop every SWFRAM radio window
        F-->>D: RX IND (ep2 msgid 3): CRC-less content + raw rssi + crc16
        Note over F: CRC-failed frames are never sent (ADR 0006)
    end

    D->>F: RADIOLINK WMBUSMSG_REQ (ep2 msgid 1), payload = CRC-less content
    F-->>D: RADIOLINK_RSP (ep2 msgid 2, empty)
    Note over F: builds sync + L + content into FIFO [54 CD][L][content]
```

- Prose detail + detect-gate rationale: `docs/agents/project-specs.md` →
  "iM871A dongle emulation".
- Daemon-side parser facts: [ADR 0003](../adr/0003-im871a-emulation-as-dongle-path.md).