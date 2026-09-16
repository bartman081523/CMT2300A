# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

PlatformIO/Arduino project for an **ESP32 driving a HopeRF (CMOSTEK) CMT2300A sub-GHz transceiver** on the **EBYTE E49-900M20S** testboard (the same silicon/matching network as the E49-900MBL-01 USB dongle).

- ESP32 ↔ CMT2300A over 4-wire SPI (SCK/SDIO/CSB/FCSB) plus two interrupt lines
- A small WebUI (when `CMT2300A_USE_WEBUI=1`) lets you type a hex payload, set frequency / power / channel offset, and read the last received packet + RSSI
- The driver is polled from the main loop; INT1-driven FIFO draining is wired but not the default RX path

## Build & flash

```bash
pio run -t upload
pio device monitor          # 115200 baud (configured in platformio.ini)
```

Only one PlatformIO env exists: `[env:esp32dev]` (espressif32, arduino framework, esp32dev board).

Pin mapping and feature flags are set via `build_flags` in `platformio.ini`. The `CMT_PIN_*` macros (SCK, MISO, MOSI, CSB, FCSB, INT1, INT2) default to the values in `include/cmt2300a_pins.h` and can be overridden through `build_flags`. To skip a pin (e.g. RST), pass `0xFF`.

There is currently **no test target** — `test/` is empty. Validation is end-to-end on real hardware.

## Source layout

```
include/
  cmt2300a.h            Class declaration, register/state/IRQ constants
  cmt2300a_config.h     Default RFPDK register map (0x0C..0x5F, 84 bytes)
  cmt2300a_pins.h       Default pin mapping (override via build_flags)
src/
  cmt2300a_spi.cpp      SPI bus, state machine, setFrequencyHz, TX/RX helpers
  cmt2300a_init.cpp     begin(), power-on timing, Control Bank 1/2 setup, ISR
  main.cpp              WiFi AP + WebServer, RX polling, TX handler
tools/
  bsl_*.py, catch_*.py  STM8 BSL/SWIM flashing experiments (UART + ESP32-driven)
  stm8flash, stm8gal    Vendored STM8 programmer tools
  stm8_swim_programmer/ ESP32 SWIM-master sketch
```

## Architecture & gotchas worth knowing

### Driver (CMT2300A class)

Three files, one class:
- `cmt2300a_init.cpp::begin()` — GPIO + SPI setup, soft-reset, RFPDK map load (0x0C..0x5F), Control Bank 1/2 assertions, FIFO merge (bit 1 of 0x69 = 64-byte single FIFO), INT1 source = PKT_DONE.
- `cmt2300a_spi.cpp` — register/FIFO read/write, state machine (`goSleep/goStandby/goTfs/goRfs/goRx/goTx`), `setFrequencyHz()` (full PLL retune), TX/RX packet helpers.
- `cmt2300a.h` — register address constants, IRQ bit positions, enums for modulation/band/IRQ source.

### EBYTE reference divergences from the datasheet

The driver follows the **EBYTE ebyte_e49x.c** register layout, not always the datasheet. Concrete examples (documented in code comments):
- **State readback is in `0x61<3:0>`**, not the 0x6B the datasheet lists for some chip revisions.
- **IRQ flags are split**: low at `0x6A` (TX_DONE=bit3, RX_TMO=bit4), high at `0x6D` (PKT_OK=bit0, CRC_OK=bit1, …). `readIntFlag()` returns `(0x6D << 8) | 0x6A`.
- **IRQ clear is write-1-to-clear** to `0x6A` (low) and `0x6B` (high) — `0x6B` is the clear register, not the high-status register.
- **Soft reset**: `writeReg(0x7F, 0xFF)` + 20 ms wait. State codes: SLEEP=0x01, STBY=0x02, RX=0x05, TX=0x06.
- **State commands** to `0x60`: SLEEP=0x10, STBY=0x02, RX=0x08, TX=0x40 (EBYTE bit pattern, not the datasheet's command codes).
- **PLL relock** (bit 5 of `0x62`) must be pulsed after every `setFrequencyHz()` or the chip carries the old frequency.
- **FIFO merge** (bit 1 of `0x69`) glues the two 32-byte FIFOs into one 64-byte FIFO; we enable it.

### Frequency tuning (`setFrequencyHz`)

Arbitrary 127–1020 MHz tuning by computing N (integer) and 20-bit K (fractional) PLL words from a 26 MHz reference. The `kBands[]` table maps VCO_BANK / DIVX_CODE / divider / LO range, reverse-engineered from EBYTE STM8 demo register exports. RX uses a 26 MHz / 92 IF offset written to the RX-N/K registers. Resolution is ~25 Hz. Driver picks the highest divider for best phase noise.

### Default RFPDK map

`include/cmt2300a_config.h` ships with the EBYTE E49-900M20S profile (868 MHz, FSK, 2.4 kbps, 4.8 kHz deviation, hardware preamble/sync/length/CRC) as a single 84-byte array covering 0x0C..0x5F. Replace with a CMOSTEK RFPDK export for a different board/modulation; the driver only touches Control Bank 1/2 itself.

### Wiring quirk: SDIO join

The E49 module breaks the CMT2300A's bidirectional SDIO out as separate MISO and MOSI pins. The ESP32 hardware SPI joins both onto one GPIO (MISO=19, MOSI=23) with the standard 1kΩ+10kΩ divider from the CMOSTEK 4-wire-mode datasheet. The chip's FIFO_MERGE bit + SPI bit-banger handles direction switching.

### Power note

+20 dBm TX draws ~80 mA peak. The on-board LDO on most ESP32 DevKits folds back; use a bench supply or a switching regulator.

## WebUI

- AP mode: SSID `CMT2300A-Demo`, password `cmt2300a` (define `CMT_WIFI_SSID` to use STA mode instead).
- `GET /` — form (hex payload, freq, channel, power) + status panel.
- `GET /status` — last RX (seq/age/rssi/len/hex), chip state, INT/FIFO flags, current frequency, VCO bank, DIVX code.
- `POST /tx` — set frequency, power, channel, transmit a packet, return status.

## STM8 dongle programming (tools/)

The E49-900MBL-01 USB dongle is STM8L151G-based. The SPI lines to the CMT2300A are routed through the STM8 — the ESP32 cannot drive them directly. See `wiring-ESP32.md` for the full two-phase plan (SWIM flash once, then UART-bridge).

**Important constraint (from project memory):** the dongle's STM8 BSL (UART bootloader) is **not reachable over USB** — likely disabled in option bytes. The 3 SWIM pads (SWIM/NRST/GND, top-right of the dongle) are the only reliable flash path. `tools/bsl_*.py` and `tools/catch_*.py` are the experiments that established this; do not waste time retrying BSL over USB without first checking that constraint.

`tools/stm8flash` and `tools/stm8gal` are vendored; `tools/stm8_swim_programmer/` is the ESP32 SWIM-master sketch for one-shot flashing of a bridge firmware.
