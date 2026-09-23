# System context view

One radio silicon, three firmware personalities. Each PlatformIO env
selects a mode (`platformio.ini`); the CMT2300A driver is common to all.

```mermaid
flowchart TB
    subgraph host["Host (Linux)"]
        D["wmbusmeters daemon<br/>(native iM871A driver)"]
        P["production wmbusmeters pipeline<br/>(rtlwmbus stdin)"]
    end
    B["Browser"]

    subgraph fw["ESP32/ESP32-S3 firmware (this repo)"]
        M1["[env:esp32dev]<br/>WebUI experiment"]
        M2["[env:esp32s3]<br/>wM-Bus production<br/>(BOK feed + repair)"]
        M3["[env:im871a]<br/>iM871A emulation"]
    end

    C["EBYTE E49<br/>CMT2300A sub-GHz transceiver"]

    M1 -- "WiFi AP + HTTP<br/>(GET /, /status, POST /tx)" --> B
    M2 -- "ASCII feed line<br/>C1;1;1;ts;rssi;0;0xhex" --> P
    D -- "HCI serial 57600 8N1<br/>/dev/ttyACM0:im871a[uid]" --> M3
    M1 & M2 & M3 -- "4-wire SPI + INT1/INT2" --> C
    C -. "868 MHz wM-Bus + beacon" .-> M2
```

- Which env gets flashed where: `docs/agents/project-specs.md` →
  "Build Commands" and "Runtime Configuration / Usage".
- Why the emulation mode exists: [ADR 0003](../adr/0003-im871a-emulation-as-dongle-path.md).
- Why the feed is BOK-gated: [ADR 0006](../adr/0006-false-fire-zero-policy.md).