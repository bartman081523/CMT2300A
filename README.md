# CMT2300A Driver – ESP32 + HopeRF sub-GHz transceiver

PlatformIO project for the ESP32 driving a HopeRF (CMOSTEK) CMT2300A
sub-GHz transceiver. The chip is a fully register-controlled OOK/FSK/MSK
transceiver for 127 – 1020 MHz with up to +20 dBm transmit power and a
32-byte TX / 32-byte RX FIFO. This project is geared toward *experimental
use* – arbitrary frequency tuning, arbitrary payload, serial or browser
control.

## Why this exists

The Amazon listing for the E49-900MBL-01 module describes a CMT2300A
868/915 MHz SMD board. The chip is *not* an NRF24L01 (it is sub-GHz
only) and the OpenDTU project, which uses 2.4 GHz NRF24 to talk to
Hoymiles inverters, does not apply. This driver treats the board as a
generic packet radio you can experiment with at will.

## What it does

- Soft-reset and full RFPDK register map load (0x0C – 0x5F, 84 bytes).
- Direct state-machine access: `goSleep / goStandby / goTfs / goRfs /
  goRx / goTx` (see Table 16 in the datasheet).
- Packet mode TX (up to 64 byte, with hardware preamble / sync / length
  / CRC defined by the RFPDK profile) and packet mode RX with RSSI
  readback.
- Frequency hopping: the RFPDK base frequency plus 256 channels of
  2.5 kHz × FH_OFFSET step (datasheet 4.3.9) for fast frequency changes
  without re-locking the PLL.
- TX power: –20 … +20 dBm in 1 dB steps.
- A small web UI (when `CMT2300A_USE_WEBUI=1`) for ad-hoc transmit and
  receive.

### Firmware modes (PlatformIO envs)

| env | Mode | What it does |
|-----|------|--------------|
| `esp32dev` | Experiment / WebUI | WiFi AP (`CMT2300A-Demo`) with a form for payload / frequency / power / channel; status panel with RSSI, chip state, VCO bank; arbitrary 127–1020 MHz tuning (~25 Hz resolution). |
| `esp32s3` | wM-Bus production | OMS T1/C1 receive profile at 868.95 MHz with hardware sync/length/CRC; polled RX windows with RSSI histogram; per-block bit repair validated by the EN 13757 block-CRC chain; BOK-gated ASCII feed (`C1;1;1;<ts>;<rssi>;0;0x<hex>`) for the production wmbusmeters stdin pipeline; CRC-less annex frames accepted; 2-second TX beacon with a per-transmit sequence number. |
| `im871a` | iM871A dongle emulation | Emulates an IMST iM871A USB dongle on the board's serial port (57600 8N1): wmbusmeters connects with `device=/dev/ttyACM0:im871a[<uid>]` and gets native RX indications (CRC-clean frames only) and TX support — no daemon patching. |

The full capability detail (driver internals, HCI protocol facts, detect
gates, feature flags per env) lives in
[docs/agents/project-specs.md](docs/agents/project-specs.md); the wiring
for the dongle path in [docs/wiring-wmbusmeters.md](docs/wiring-wmbusmeters.md).

## Hardware

This project targets the **E49-900MBL-01** testboard kit from EBYTE
(Amazon ASIN B0BY8DL1H9). The kit carries the CMT2300A sub-GHz
transceiver on a small carrier PCB that breaks out the QFN16 signals
to a 2 mm pin header.

> The E07-900MBL-01 manual that may be in the same EBYTE product
> family covers a *different* chip (TI CC1101) and is **not** the
> right reference. The matching module is E49-900MBL-01.

### Pin mapping (verified against the E15-EVB02 schematic)

The E15-EVB02 development board from EBYTE carries the E49-900M20S
module (CMT2300A, 868/915 MHz). The schematic
`E15-EVB02_E49-900M20S/2_原理图/E15-EVB02-SCH-V1.0.pdf` exposes the
following 22-pin module pin-out; the EBYTE STM8 demo board wires the
module this way, and the table below is the wiring we use from the
ESP32.

| CMT2300A signal | Module pin (E15-EVB02) | ESP32 GPIO |
|-----------------|------------------------|------------|
| SCLK            | 18 SCK                 | 18         |
| SDIO (bidir)    | 16 MISO  +  17 MOSI    | 19 + 23 joined (1kΩ/10kΩ divider) |
| CSB (registers)  | 5 NSS (CS)             | 5          |
| FCSB (FIFO)     | 19 NSS (FIFO)          | 17         |
| INT1 / GPIO1    | 11 DIO1                | 4          |
| INT2 / GPIO2    | 8  DIO2                | 16         |
| RST              | 15 RST                 | n.c. (use 0xFF to skip) |
| VCC              | 9, 10                  | 3V3        |
| GND              | 1, 2, 3, 12, 14, 20, 21| GND        |
| ANT              | 22                     | SMA on board |

**Why the SDIO join:** the CMT2300A uses a single bidirectional SDIO
line, but the E49 module breaks it out as separate MISO/MOSI pins
(see EBYTE's `ebyte_e49x.c` – they bit-bang SDIO with one STM8 pin
and a direction switch). To use the ESP32's hardware SPI we wire
both module pins to a single ESP32 SDIO GPIO, with the standard
1kΩ+10kΩ resistor divider that CMOSTEK's datasheet shows for 4-wire
mode. The ESP32's MISO/MOSI are joined to one physical wire: the
chip's direction register (selected by the EBYTE `0x69` bit 1
FIFO_MERGE we set in `init.cpp`) plus the standard SPI bit-banger
handles the rest.

**Power:** E49 board needs a clean 3.3 V supply. The on-board LDO on
most ESP32 DevKits will fold back during +20 dBm TX (80 mA peak), use
a bench supply or a separate switching regulator.

## Build / flash

```bash
pio run -t upload
pio device monitor
```

## Configuration

`include/cmt2300a_config.h` holds the **default RFPDK register map**
which we copy verbatim from the EBYTE STM8 demo code
(`E15-EVB02_E49-900M20S/3_代码工程/.../ebyte_e49x.c`).  The 84-byte
array covers 0x0C – 0x5F and is the same profile the E49-900M20S
ships with: 868 MHz, FSK, 2.4 kbps, 4.8 kHz deviation, hardware
preamble / sync / length / CRC.  Replace it with a CMOSTEK RFPDK
export for your own board if you need a different modulation or
channel filter.  The driver only touches Control Bank 1 / 2 (state
machine, GPIO routing, interrupt sources, FIFO merge) itself.

### What the EBYTE reference taught us

- **Soft reset** is `writeReg(0x7F, 0xFF)` + 20 ms wait.
- **State readback** lives in `0x61<3:0>` (the datasheet's 0x6B is
  wrong for the E49 chip).  Codes: SLEEP=0x01, STBY=0x02, RX=0x05,
  TX=0x06.
- **State commands** are written to `0x60` with EBYTE's bit pattern:
  SLEEP=0x10, STBY=0x02, RX=0x08, TX=0x40.
- **Interrupt flags** are split between `0x6A` (low: TX_DONE=bit3,
  RX_TMO=bit4) and `0x6D` (high: PKT_OK=bit0, CRC_OK=bit1, LBD=bit7).
  `readIntFlag()` returns a 16-bit word `(0x6D << 8) | 0x6A`.
- **Interrupt clear** is `writeReg(0x6A, mask)` for low, `writeReg(0x6B, mask)`
  for high (write-1-to-clear).  The EBYTE code maps COL_ERR/PKT_ERR
  in 0x6D to the PKT_DONE bit in 0x6B.
- **PLL relock** is mandatory after a frequency change: pulse bit 5
  of `0x62`.  The driver does this in `setFrequencyHz()`.
- **FIFO merge** (bit 1 of `0x69`) glues the two 32-byte FIFOs into a
  single 64-byte FIFO.  We enable it.

## Key references

- CMT2300A datasheet Rev 1.7 (Jul 2023) – the `*.pdf` in this folder.
- AN142 – CMT2300A Quick Start Guide.
- AN143 – CMT2300A FIFO and Data Packet Usage Guideline.
- AN144 – CMT2300A RSSI Usage Guideline.
- AN197 – CMT2300A frequency hopping calculation tool.

## Documentation

- **Agent guidance**: [AGENTS.md](AGENTS.md) routes to
  [docs/agents/project-specs.md](docs/agents/project-specs.md)
  (specs, build, conventions) with
  [research.md](docs/agents/research.md) and
  [devmind.md](docs/agents/devmind.md).
- **Architecture decisions**: [docs/adr/](docs/adr/) — start at
  [0001](docs/adr/0001-adr-process.md); the register-divergence policy is
  [ADR 0005](docs/adr/0005-ebyte-register-layout-wins.md).
- **Architecture diagrams**: [docs/architecture/](docs/architecture/) —
  system context, HCI handshake, RX data flow, module graph.
- **Wiring**: E49 → ESP32-S3 for wmbusmeters:
  [docs/wiring-wmbusmeters.md](docs/wiring-wmbusmeters.md) ·
  E49-900MBL-01 dongle / STM8 two-phase plan:
  [wiring-ESP32.md](wiring-ESP32.md) · dongle board anatomy:
  [Board-Description.md](Board-Description.md).

## Known limitations

- The driver is polled from the main loop. It is sufficient for
  interactive experiments but not for hard real-time traffic.
- Frequency switching during RX is not supported – for that you need
  the super-low-power (SLP) mode and the RSSI-based receive extension
  logic (datasheet 7.2).
- The default register map is the EBYTE E49-900M20S profile
  (868 MHz, 2.4 kbps, 4.8 kHz deviation, FSK).  The CMOSTEK RFPDK
  rotates reserved bits between releases, so for a different board
  you should use a fresh RFPDK export.
