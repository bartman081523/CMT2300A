# RX data-flow view

From on-air frame to consumer. The BOK gate is the quality boundary —
nothing CRC-dirty leaves the firmware ([ADR 0006](../adr/0006-false-fire-zero-policy.md)).

```mermaid
flowchart LR
    RF["868.95 MHz<br/>OMS T1/C1"] --> HW["CMT2300A hardware:<br/>preamble / sync / length / CRC"]
    HW --> W["SWFRAM window:<br/>RSSI histogram 0x6F,<br/>SYNC_OK poll 0x6D"]
    W --> DR["room-capped<br/>FIFO drain"]
    DR --> V{"frame verdict"}

    V -- "CRC ok" --> G["BOK gate"]
    V -- "BBAD" --> R["repair engine:<br/>flip / insert / drop<br/>per block"]
    R -- "block-CRC chain<br/>validates" --> G
    R -- "no valid repair" --> X["drop"]

    G -- "[env:esp32s3] CMT_WMBUS_FEED" --> F["ASCII feed line:<br/>C1;1;1;ts;rssi;0;0xhex"]
    G -- "[env:im871a] CMT_HCI_IM871A" --> I["HCI IND (ep2 msgid 3)<br/>content + rssi + crc16"]
```

- Window/verdict mechanics: `src/main.cpp` (`CMT_SWFRAM` section,
  `platformio.ini:71`), repair flag `platformio.ini:80`.
- Prose detail: `docs/agents/project-specs.md` → "wM-Bus production mode".