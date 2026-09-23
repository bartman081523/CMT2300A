# Project specs, tools, build & conventions

> **Purpose.** This file holds every project-specific detail an AI coding
> agent (or human contributor) needs to work in this repository. It is
> **loaded on demand by the metaprompt in [`AGENTS.md`](../../AGENTS.md)** —
> together with [`research.md`](research.md) (methodology) and
> [`devmind.md`](devmind.md) (engineering mindset).
>
> **Evidence rules.** Every factual claim here is backed by evidence: a
> file in this repo (cite as `file:line`), git history, CI configuration,
> or a command that was actually run. Documented commands carry a
> verification marker — `<!-- verified YYYY-MM-DD: <observation> -->` —
> or are marked `<!-- UNVERIFIED — source: <where it was read> -->`.
> Sections without evidence are deleted, never left as empty stubs.

## Build Commands

Three PlatformIO environments (`platformio.ini:10,42,155`):

```bash
pio run -e esp32dev    # classic ESP32, experiment/WebUI firmware
pio run -e esp32s3     # ESP32-S3, wM-Bus production firmware (BOK feed + repair)
pio run -e im871a      # ESP32-S3, iM871A-dongle emulation (HCI protocol)
pio run -e im871a -t upload --upload-port /dev/ttyACM0   # flash the S3 board
pio device monitor     # 115200 (esp32dev/esp32s3); NOT usable on im871a while
                       # the HCI line is the daemon's serial port
```
<!-- verified 2026-09-23: `pio run -e esp32dev` SUCCESS 13.9 s; `pio run -e esp32s3` SUCCESS 3.7 s (incremental) -->
<!-- verified 2026-09-22: `pio run -e im871a` SUCCESS 9.0 s; `pio run -e im871a -t upload --upload-port /dev/ttyACM0` SUCCESS 15.8 s, hash verified -->
<!-- UNVERIFIED — source: README.md "Build / flash", CLAUDE.md; `pio run -t upload` (default env) and `pio device monitor` not executed in this session -->

There is **no test target** — `test/` is empty; validation is end-to-end on
real hardware (see Testing).

## Specifications & references

| Reference | Role in this project |
|---|---|
| CMT2300A datasheet Rev 1.7 (Jul 2023), PDF in repo root | Register-level reference; several register behaviours diverge on the E49 silicon — the EBYTE reference code wins (see README "What the EBYTE reference taught us") |
| EBYTE `ebyte_e49x.c` demo code | Authoritative register layout used by the driver. Vendored copy lives in a **gitignored** local dir (`E15-EVB02_E49-900M20S/`), not in the repo |
| CMOSTEK app notes AN142 (quick start), AN143 (FIFO), AN144 (RSSI), AN197 (frequency hopping) | Referenced in README.md "Key references" |
| EN 13757 / OMS wireless M-Bus | The wire format the production firmware receives and transmits: DLL/TPL frames, block-CRC chain (used by the repair engine, `platformio.ini:77-80`), CRC-less annex frames |
| wmbusmeters (upstream source, local build tree) — `wmbus_im871a.cc`, `crc16.cc`, `wmbus.cc` | Authoritative for the iM871A HCI protocol the shim must reproduce: detect sequence (~`wmbus_im871a.cc:1189-1265`), frame parser `checkIM871AFrame`, CRC16 init 0xFFFF / poly 0x8408 LSB-first, permanent tty-ignore on detect failure (`wmbus.cc` "Ignoring tty") |
| IMST iM871A user manual (HCI) | Secondary reference for the emulated dongle; where manual and wmbusmeters source disagree, the wmbusmeters source decides (it is the consumer) |
| `tools/rfpdk-exp-format.md` | Format of the RFPDK register exports used to derive `include/cmt2300a_config.h` maps (`include/cmt2300a_config.h:92`) |

## Linting / Formatting

None configured: no `.clang-format`, `.editorconfig`, `.clang-tidy`, CI
lint jobs, or lint targets exist in the repo (checked 2026-09-23).

## Coding Conventions

Observed in a sample of `src/*.cpp`, `include/*.h`:

- 4-space indent, same-line braces (`src/im871a_hci.cpp`, `src/main.cpp`).
- Local includes use quotes: `#include "cmt2300a.h"` (`src/main.cpp:36-38`);
  Arduino libs angle-bracket (`#include <WebServer.h>`, `src/main.cpp:33`).
- Naming: camelCase methods (`goRx`, `setFrequencyHz`, `readIntFlag`),
  `k`-prefixed file-local constants (`kBands`, `src/cmt2300a_spi.cpp:376`),
  `CMT_*` / `CMT_PIN_*` build-flag macros (`platformio.ini:16-34`),
  `s`-prefixed file-static state in the HCI module (`sRadioOn`, `sMuteUntil`,
  `src/im871a_hci.cpp`), `g_`-prefixed globals (`g_beaconSeq`,
  `src/main.cpp:116`).
- Comments are mixed German/English; German comments avoid non-ASCII
  ("Ue" for ü, `src/im871a_hci.cpp` Runde-88 block). Dated round notes
  ("Runde 88 (2026-09-22): ...") are the established pattern for
  behavioural change rationale (`platformio.ini:57-64`, `src/main.cpp:112`).
- Header guards in `include/*.h` (e.g. `include/cmt2300a.h`).

## Commit Message Style

Nothing enforced (no CI, no hooks). Observed history — titles are
`<topic>: <description>`, body prose German or English, no conventional-
commit prefixes:

- `Stufe A: im871a-HCI-Shim (RX+TX bewiesen)` — body lists mechanism and
  the hardware proofs (`git show c211645`)
- `Snapshot r87: CMT2300A firmware before im871a-HCI mode` — body lists
  live features, soak numbers, and the rollback target (`git show ac43acc`)

One logical change per commit; stage runs (Stufe A/B) are snapshot +
feature commit pairs.

## Common Workflows

1. **Build everything**: `pio run -e esp32dev -e esp32s3 -e im871a`.
2. **Flash the S3 board as iM871A dongle** (the wmbusmeters integration
   path): `pio run -e im871a -t upload --upload-port /dev/ttyACM0`, then
   start wmbusmeters with a config pointing at `/dev/ttyACM0:im871a[<uid>]`
   — see "Runtime Configuration / Usage".
3. **Local wmbusmeters test instance** (session artifacts live in the
   gitignored `scratches/` dir): config `scratches/im871a_conf/`
   (device, `listento=c1,t1`, `logtelegrams=true`, meter dir copied from
   the production config), run
   `wmbusmeters --useconfig=scratches/im871a_conf --exitafter=25m
   --logfile=Log/<name>.log`. Runs as the user's process, never as the
   production root daemon.
4. **HCI wire test** (hardware acceptance): `python3
   scratches/hci_wire_test.py` — sends DEVICEINFO / GET_CONFIG / PING /
   SET_CONFIG / TX requests over `/dev/ttyACM0` and compares responses
   byte-exactly against the wmbusmeters oracle. Last run 2026-09-22: 6/6.
5. **Soak bilan** (after a soak): grep the logfile for `for me?` (per-meter
   match attempts), `yes for me` (decoded), `wrong crc` (false-fire
   indicator — must stay 0), and `"_":"telegram"` (JSON telegram lines).

## Architecture Overview

One class + one protocol module, orchestrated by `main.cpp`:

| File | Purpose |
|---|---|
| `src/cmt2300a_init.cpp` | `begin()`: GPIO/SPI setup, soft reset, RFPDK map load, Control Bank 1/2, FIFO merge, INT1 source, ISR |
| `src/cmt2300a_spi.cpp` | SPI bus, register/FIFO access, state machine (`goSleep/goStandby/goTfs/goRfs/goRx/goTx`), `setFrequencyHz()` (N/K PLL words from 26 MHz reference, `kBands` table), TX/RX packet helpers |
| `include/cmt2300a.h` | Register/state/IRQ constants, modulation/band/IRQ enums |
| `include/cmt2300a_config.h` | Default RFPDK register maps (84 bytes, 0x0C–0x5F); EBYTE profile vs OMS T1 profile selected by `CMT_OMS_T1` (`include/cmt2300a_config.h:92`) |
| `src/im871a_hci.cpp` + `include/im871a_hci.h` | iM871A dongle emulation: HCI frame parser FSM, RSP frames, IND emitter, daemon-TX path, boot banner (`src/im871a_hci.cpp:216`) |
| `src/main.cpp` | Mode orchestration: WiFi AP + WebUI, RX polling, TX handling, wM-Bus feed/repair/SWFRAM, beacon TX, HCI hooks |
| `stm8_swim_tool/` | Standalone PlatformIO project: ESP32 as SWIM master for the dongle's STM8 |
| `tools/` | STM8 BSL experiments (constraint: BSL unreachable over USB), vendored `stm8flash`/`stm8gal` |

### Firmware capabilities (all three firmware modes)

**1. Experiment / WebUI mode (`[env:esp32dev]`)**

- CMT2300A driver: soft reset (`writeReg(0x7F, 0xFF)` + 20 ms), full RFPDK
  register-map load (84 bytes, 0x0C–0x5F), Control Bank 1/2, direct state
  machine access (`goSleep/goStandby/goTfs/goRfs/goRx/goTx`).
- Arbitrary frequency tuning 127–1020 MHz, ~25 Hz resolution: N/K PLL
  words from a 26 MHz reference, highest-divider band preference
  (`src/cmt2300a_spi.cpp:376,411-412`), mandatory PLL relock (bit 5 of
  0x62) after every retune. Frequency hopping via base + 256 channels
  × 2.5 kHz `FH_OFFSET` step.
- TX power −20 … +20 dBm in 1 dB steps; packet TX/RX with hardware
  preamble/sync/length/CRC; RSSI readback; 64-byte FIFO (FIFO merge,
  bit 1 of 0x69); INT1 = PKT_DONE. The INT1 ISR/FIFO-drain path is wired
  (`src/cmt2300a_init.cpp`) but **not** the default RX path — RX is polled
  from the main loop.
- WiFi AP WebUI (AP `CMT2300A-Demo` / `cmt2300a`, STA via `CMT_WIFI_SSID`;
  `src/main.cpp:87`): `GET /` (payload/frequency/power/channel form +
  status panel), `GET /status` (last RX, RSSI, chip state, INT/FIFO flags,
  VCO bank, DIVX code), `POST /tx` (`src/main.cpp:926-928`).

**2. wM-Bus production mode (`[env:esp32s3]`)**

- OMS T1/C1 receive profile at 868.95 MHz (GFSK 32.8 kbps, Manchester,
  50 kHz deviation) selected by `CMT_OMS_T1=1`; variable-length T1 frames
  (`CMT_T1_VARLEN=1`).
- **SWFRAM RX engine** (`CMT_SWFRAM=1`): polled 25-s RX windows, RSSI
  histogram read (0x6F), SYNC_OK polling (0x6D), room-capped FIFO drain,
  RX verdict with RSSI and CRC classification.
- **Bit-repair engine** (`CMT_WMBUS_REPAIR=1`): per-block bit repair of
  BBAD frames (flip/insert/drop) verified via the EN 13757 block-CRC
  chain; repaired frames are BOK-gated into the feed
  (`platformio.ini:77-80`).
- **BOK feed** (`CMT_WMBUS_FEED=1`): CRC-ok frames as ASCII
  `C1;1;1;<ts>;<rssi>;0;0x<hex>` lines on the UART — the
  wmbusmeters-`checkRTLWMBUSFrame` grammar, consumed by the production
  rtlwmbus daemon via stdin.
- **BOKA annex acceptance**: CRC-less annex frames (25-byte AES annex
  without block CRCs) accepted as full frames.
- **TX beacon**: 2-s cadence with per-TX sequence in the V-field
  (`g_beaconSeq`, `src/main.cpp:112-116,992-993`) — used as the RF oracle
  for the production RTL pipeline.
- Probe harnesses (sync-triggered capture, RSSI envelope logger,
  frequency-offset sweep) exist behind flags, all currently 0
  (`platformio.ini:107-133`).

**3. iM871A dongle emulation (`[env:im871a]`, `CMT_HCI_IM871A=1`)**

- Emulates an IMST iM871A USB dongle on UART0 @ 57600 8N1 (board CH343
  bridge → host `/dev/ttyACM0`); wmbusmeters connects with
  `device=/dev/ttyACM0:im871a[46d4db1c]` (UID = ESP32 efuse MAC, wire LE).
- HCI frame format `[0xA5][(ctrlbits<<4)|endpoint][msgid][len][payload]
  [+timestamp?4][+rssi?1][+crc16?2]`; CRC16 init 0xFFFF, poly 0x8408,
  LSB-first, wire `~crc` LE; CRC spans `fr[1..n-1]`.
- Responds to the daemon's commands: PING (→ 0x02), GET_DEVICEINFO
  (→ 0x10, type 0x33 / mode 0 / firmware 0x15 / hci 0x16 / uid),
  GET_CONFIG (→ 0x06), SET_CONFIG (→ 0x04 echo; sets link mode,
  `c1,t1` → 0x0A = CT_N1A).
- IND emitter: every CRC-ok received frame becomes an IND (endpoint 2,
  msgid 3) carrying the CRC-less content, raw RSSI and CRC16; BBAD
  frames are **not** forwarded (false-fire = 0 policy).
- Daemon TX: WMBUSMSG_REQ payload = CRC-less content verbatim (no L, no
  sync); firmware builds on-air sync + `L` + content into the
  `[54 CD][L][content]` FIFO; TX-RSP endpoint 2 msgid 2.
- **Detect gates (Runde 88)**: the firmware boots silent (`sRadioOn`
  stays false until the first SET_CONFIG) and mutes INDs for 400 ms from
  the moment a request's control byte is recognised (`sMuteUntil`,
  P_CTRL transition) — the daemon's 100 ms detect window parses the
  *first* frame in its buffer, so a stray IND there breaks detection
  permanently (wmbusmeters then ignores the tty until instance restart).
  Proof: detect sim 10/10 boot + 10/10 post-SET_CONFIG, response bytes
  ~4 ms after request.

**STM8 dongle tooling**: the E49-900MBL-01 USB dongle is STM8L151G-based;
its SPI lines route through the STM8. The BSL (UART bootloader) is not
reachable over USB — the three SWIM pads are the only reliable flash path
(`tools/bsl_*.py`/`catch_*.py` established this; `wiring-ESP32.md` holds
the two-phase plan; `stm8_swim_tool/` is the ESP32 SWIM-master sketch).

### Data flow (iM871A mode, the wmbusmeters integration path)

1. Radio window opens (SWFRAM); the CMT2300A receives wM-Bus frames in
   hardware (preamble/sync/length/CRC per the RFPDK profile).
2. The inner loop polls RSSI/SYNC_OK, drains the FIFO room-capped, and
   classifies each frame (CRC ok / BBAD / repairable).
3. `hciPoll()` runs at the top of every inner-loop iteration — no delay
   in the loop, so HCI request parsing is at µs granularity.
4. CRC-ok frames are pushed as HCI INDs (gated by `sRadioOn`/mute
   window); the daemon decodes them (meter match, RSSI, JSON telegram).
5. Daemon TX requests parse into the FSM; the payload goes verbatim into
   the TX FIFO; the beacon loop exercises TX independently.

## Testing

No test target — `test/` is empty; validation is end-to-end on real
hardware. The verification scripts live in the **gitignored** `scratches/`
directory (session artifacts, not shipped):

- `scratches/hci_wire_test.py` — byte-exact HCI handshake test (6/6 on
  2026-09-22).
- `scratches/hci_detect_sim2.py` — daemon-detect simulation, boot phase +
  post-SET_CONFIG phase (10/10 + 10/10 on 2026-09-22).
- `scratches/hci_mock_emitter_check.py` — verbatim mock of the RSP/IND/
  TX-frame emitters against the wmbusmeters oracle (offline, no hardware).
- `scratches/hci_crc_check.py` — CRC16 reproduction checked against the
  daemon implementation.
- `scratches/im871a_conf/` — local wmbusmeters config + meter dir for
  integration soaks.

A "single test" = run one script against the flashed board; expected
output is the oracle comparison in the script, written to `Log/`.

## CI

None: no `.github/`, `.gitlab-ci.yml`, `tox.ini` or equivalent exists
(checked 2026-09-23).

## Runtime Configuration / Usage

- **Feature flags** are `CMT_*` build flags in `platformio.ini`; the
  production S3 env sets `CMT_OMS_T1=1`, `CMT_T1_VARLEN=1`,
  `CMT_SWFRAM=1`, `CMT_WMBUS_FEED=1`, `CMT_WMBUS_REPAIR=1` and keeps the
  probe harnesses at 0. The im871a env flips `CMT_WMBUS_FEED=0` (the
  daemon wants binary INDs, not ASCII feed lines) and sets
  `CMT_HCI_IM871A=1`.
- **Pin mapping** is `CMT_PIN_*` build flags, different per env:
  classic ESP32 (SCK 18, SDIO via MISO/MOSI 19+23, CSB 5, FCSB 17,
  INT1 4, INT2 16 — README pin table) vs ESP32-S3 on the E49 dongle
  header (SCK 7, SDIO 6, CSB 4, FCSB 5, INT1 15, INT2 16, NRST 8 —
  sniffed wiring, `platformio.ini:136-144`). Defaults live in
  `include/cmt2300a_pins.h`; pass `0xFF` for a pin to skip it (e.g. RST).
  Wiring details incl. the resistor-free direct connection:
  `docs/wiring-wmbusmeters.md`.
- **wmbusmeters config** (local instance):
  `device=/dev/ttyACM0:im871a[46d4db1c]`, `listento=c1,t1`,
  `logtelegrams=true`. The device spec must be `<file>:im871a[<uid>]`;
  `im871a[file]` is invalid.
- **Detect-failure behaviour**: wmbusmeters ignores the tty *permanently*
  after a failed detect ("Ignoring tty!") — restart the instance. Two of
  three observed instance starts after a flash needed one retry.
- **Serial speeds**: 115200 (esp32dev/esp32s3), 57600 (im871a HCI line —
  occupied by the daemon while connected; debug prints are gated off in
  the HCI build to keep the wire protocol-clean, only the boot banner
  prints before a client connects, `src/im871a_hci.cpp:216`).

## Dependencies

- PlatformIO platform `espressif32`, Arduino framework, boards
  `esp32dev` / `esp32-s3-devkitc-1` (`platformio.ini:10-14,42-46,155-160`).
- Arduino core libs only (WiFi, WebServer) — no external library
  dependencies.
- Vendored tools: `tools/stm8flash` and `tools/stm8gal` (STM8 programmers,
  vendored as git clones — upstreams: `github.com/vdudouyt/stm8flash`,
  `github.com/gicking/stm8gal`), `tools/stm8_swim_programmer/` (ESP32
  SWIM-master sketch + shell scripts), `stm8_swim_tool/` (own standalone
  PlatformIO project: STM8 flash reader/writer over SWIM, ported from
  `esp-stlink`).
- Hardware: EBYTE E49-900M20S module (CMT2300A) or E49-900MBL-01 dongle
  board; +20 dBm TX draws ~80 mA peaks — the DevKit LDO folds back, use a
  bench supply.

## Source Layout

```
include/          driver headers (cmt2300a.h, cmt2300a_config.h,
                  cmt2300a_pins.h, im871a_hci.h)
src/              driver (cmt2300a_init.cpp, cmt2300a_spi.cpp),
                  HCI shim (im871a_hci.cpp), orchestration (main.cpp)
stm8_swim_tool/   standalone SWIM-master PlatformIO project
tools/            STM8 BSL experiments (bsl_*.py, catch_*.py),
                  rfpdk-exp-format.md, vendored stm8flash/stm8gal
                  (git clones), stm8_swim_programmer/ (older SWIM sketch
                  + shell scripts)
docs/agents/      this bundle; docs/adr/, docs/architecture/,
                  docs/wiring-wmbusmeters.md
Log/, scratches/  session artifacts (gitignored)
.pio/             build output (gitignored)
```

## Verification log

| Command | Status | Evidence | Date |
|---------|--------|----------|------|
| `pio run -e esp32dev` | verified | exit 0, SUCCESS 13.9 s | 2026-09-23 |
| `pio run -e esp32s3` | verified | exit 0, SUCCESS 3.7 s | 2026-09-23 |
| `pio run -e im871a` | verified | exit 0, SUCCESS 9.0 s | 2026-09-22 |
| `pio run -e im871a -t upload --upload-port /dev/ttyACM0` | verified | exit 0, 15.8 s, hash verified | 2026-09-22 |
| `scratches/hci_wire_test.py` | verified | 6/6 byte-exact vs oracle | 2026-09-22 |
| `scratches/hci_detect_sim2.py` | verified | 10/10 boot + 10/10 post-SET_CONFIG, first RSP byte ~4 ms | 2026-09-22 |
| local wmbusmeters soak (`scratches/im871a_conf`) | verified | daemon connect + 13 qsmoke meters decoded, 0 wrong crc | 2026-09-22 |
| `pio device monitor` | UNVERIFIED | read in README/CLAUDE.md, not executed this session | 2026-09-23 |
| `pio run -t upload` (default env) | UNVERIFIED | read in README "Build / flash" | 2026-09-23 |