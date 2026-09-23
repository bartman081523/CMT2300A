# Verdrahtung: E49 + ESP32-S3 als wmbusmeters-Dongle

Diese Anleitung ist der **bewiesene Pfad** für
`device=/dev/ttyACM0:im871a[...]` in wmbusmeters: das E49-Modul direkt am
ESP32-S3-Board, Firmware `[env:im871a]`, Daemon behandelt das Board wie
einen IMST iM871A-USB-Dongle. Für den **Dongle-/STM8-Pfad**
(E49-900MBL-01-USB-Stick, SPI über den STM8) siehe
[`wiring-ESP32.md`](../wiring-ESP32.md); für die Dongle-Anatomie
[`Board-Description.md`](../Board-Description.md).

## Verdrahtung (verifiziert im Live-Betrieb)

Pin-Zuordnung aus `platformio.ini:135-144` (`[env:esp32s3]`/`[env:im871a]`):

| E49-Modul-Signal | ESP32-S3 GPIO | `CMT_PIN_*` |
|------------------|---------------|-------------|
| SCK              | 7             | `CMT_PIN_SCK=7` |
| SDIO (MISO **und** MOSI direkt verbunden) | 6 (ein GPIO) | `CMT_PIN_MISO=6`, `CMT_PIN_MOSI=6` |
| CSB (Register)   | 4             | `CMT_PIN_CSB=4` |
| FCSB (FIFO)      | 5             | `CMT_PIN_FCSB=5` |
| INT1 / GPIO1     | 15            | `CMT_PIN_INT1=15` |
| INT2 / GPIO2     | 16            | `CMT_PIN_INT2=16` |
| NRST             | 8             | `CMT_PIN_NRST=8` |
| VCC (Pins 9, 10) | 3V3           | — |
| GND (Pins 1, 2, 3, 12, 14, 20, 21) | GND | — |
| ANT (Pin 22)     | SMA           | — |

**Ohne Widerstände:** die SDIO-Leitung wird **direkt** auf einen GPIO
gelegt — MISO- und MOSI-Pin des Moduls verbunden, keine 1 kΩ/10 kΩ-Teilung.
Diese Variante ist im Betrieb bewiesen (HCI-Draht-Test 6/6 byte-exakt,
Integration-Soak mit 13 dekodierten Zählern, False-Fire 0). Begründung und
Abwägung: [ADR 0002](adr/0002-sdio-single-gpio-wiring.md). Die
Datasheet-Variante mit Divider bleibt die dokumentierte Alternative fürs
klassische ESP32-Testboard (README „Pin mapping").

**Stromversorgung:** +20 dBm TX zieht ~80 mA Peaks — DevKit-LDO klappt
zusammen; Netzteil oder Schaltregler verwenden.

## Firmware

- Env: `[env:im871a]` (`CMT_HCI_IM871A=1`, `CMT_WMBUS_FEED=0`,
  `monitor_speed=57600`) — emuliert den iM871A-Dongle auf UART0
  57600 8N1; das Board-USB (CH343) erscheint als `/dev/ttyACM0`.
- Flashen:

  ```bash
  pio run -e im871a -t upload --upload-port /dev/ttyACM0
  ```

- Nach dem Flash bootet die Firmware **stumm** (kein IND, kein Print bis
  auf das Boot-Banner) — sie meldet sich erst nach dem ersten
  `SET_CONFIG` des Daemons. Das ist Absicht (Detect-Schutz, „Runde 88").

## wmbusmeters-Anbindung

```
device=/dev/ttyACM0:im871a[<uid>]
listento=c1,t1
logtelegrams=true
```

- Die Device-Spec ist `<tty>:im871a[<uid>]` — tty **zuerst**, Bracket
  dahinter; `im871a[file]` ist ungültig.
- `<uid>` = Display-Form der ESP32-efuse-MAC, **boardspezifisch**
  (Beispiel dieses Boards: `46d4db1c`; Draht-Wireform LE = `1C DB D4 46`).
  UID am eigenen Board auslesen: HCI-Frame-Log beim Boot-Banner oder den
  DEVICEINFO-Test in `scratches/hci_wire_test.py` nachführen.
- Detect-Fail-Verhalten: schlägt die Erkennung fehl, ignoriert der Daemon
  das tty **endgültig** („Ignoring tty!") — Instanz neu starten. In der
  Praxis brauchten 2 von 3 Starts nach einem Flash einen Retry; danach
  verbindet der Daemon zuverlässig.

## Beweise

- Draht-Test 6/6 byte-exakt (DEVICEINFO/GET_CONFIG/PING/SET_CONFIG/TX-RSP):
  Session-Artefakt `scratches/hci_wire_test.py`, Log in `Log/`
  (gitignored, nicht im Repo).
- Integration-Soak: Daemon verbindet (`are you there? yes ... firmware: 15`),
  13 qsmoke-Zähler dekodiert, False-Fire 0 — Commit `c211645`
  („Stufe A: im871a-HCI-Shim (RX+TX bewiesen)").
- Protokoll-/Gate-Details: [ADR 0003](adr/0003-im871a-emulation-as-dongle-path.md),
  Architektur-Sicht [`architecture/hci-handshake.md`](architecture/hci-handshake.md).