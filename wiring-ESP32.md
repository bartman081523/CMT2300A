# E49-900MBL-01 Dongle: ESP32 als Programmierer + SPI-Controller

## Konzept

```
Phase 1 (einmalig):   ESP32 flashed den STM8 via SWIM mit einer Bridge-Firmware
Phase 2 (laufend):    ESP32 spricht über UART mit dem STM8, der STM8 steuert CMT2300A via SPI
```

**Warum nicht direkt SPI vom ESP32 zum CMT2300A?**
- Die SPI-Leitungen des CMT2300A sind auf dem Dongle über den STM8 geführt
- Der ESP32 müsste sich die Leitungen mit dem STM8 teilen → komplexes Pin-Multiplexing
- Saubere Trennung: ESP32 ↔ STM8 (UART), STM8 ↔ CMT2300A (SPI)


## Phase 1: SWIM-Programmierung (einmalig)

### 3-Pin-Stiftleiste einlöten

Die 3 leeren Pads oben rechts auf dem Dongle sind SWIM. Stiftleiste einlöten.
Reihenfolge der Pins (laut `Board-Description.md`):
1. **SWIM** (Daten, bidirektional)
2. **NRST** (Reset, active-low)
3. **GND** (Masse)

Wichtig: Die Versorgung (3.3V) läuft weiterhin über USB. **Nicht** an einem 3.3V-Pin der Leiste anzapfen — der ist nur für Logik-Referenz, nicht für Versorgung gedacht.

### SWIM-Software auf dem ESP32

Es gibt fertige Projekte:
- [esp-stlink](https://github.com/rumpeltux/esp-stlink) — generischer SWIM-Master für ESP32
- Selbst geschrieben basierend auf STM8 SWIM Protocol (Datenblatt RM0016 Kap. 27)

Pin-Belegung für SWIM am ESP32 (alle frei wählbar):

```
E49 SWIM-Pad  →  ESP32-Pin
──────────────────────────────
SWIM          →  GPIO 25  (z.B.)
NRST          →  GPIO 26
GND           →  GND
```

Während der Programmierung:
- ESP32 zieht NRST auf LOW
- ESP32 sendet SWIM-Entry-Sequenz (mind. 16x LOW-Pulse auf SWIM-Leitung bei LOW-NRST)
- ESP32 kann jetzt über SWIM Befehle schicken (SRST, ROTF, WOTF)
- STM8-Firmware lesen/schreiben
- NRST loslassen → STM8 bootet mit neuer Firmware

Für das Schreiben einer fertigen `.ihx`/`.hex`/`s19`-Datei reicht die SWIM-Geschwindigkeit (~1 KB/s) problemlos aus.


## Phase 2: Laufender Betrieb (ESP32 ↔ STM8 über UART)

Nach dem Flash läuft die neue STM8-Firmware. Sie muss:
- USART1 (PA2=TX, PA3=RX) auf 115200 8N1 konfigurieren
- Auf Befehle vom ESP32 warten und SPI-Befehle an CMT2300A weiterleiten
- Funkmodul-Ergebnisse zurückmelden

### Verdrahtung ESP32 ↔ Dongle

Dongle-Stiftleiste (9 Pins, von links nach rechts):

```
Pin  Funktion  STM8L  ESP32             Anmerkung
─────────────────────────────────────────────────────────────
 1   GND       GND    GND               Masse
 2   VIO       VDD    3.3V              Versorgung Logik
 3   PC0       PC0    —                 E49 CSB (vom STM8 kontrolliert)
 4   PB7       PB7    —                 E49 FCSB (vom STM8 kontrolliert)
 5   PB6       PB6    —                 E49 SDIO (vom STM8 kontrolliert)
 6   PB5       PB5    —                 E49 SCK (vom STM8 kontrolliert)
 7   PB4       PB4    —                 E49 NSS / ungenutzt
 8   TXD       PA2    GPIO 16 (RX)      UART RX vom ESP32
 9   RXD       PA3    GPIO 17 (TX)      UART TX zum ESP32
```

**Nur 4 Drähte zwischen ESP32 und Dongle:**
```
Dongle          ESP32
─────────       ──────
GND    ──────── GND
VIO    ──────── 3.3V
TXD    ──────── GPIO 16 (RXD, UART-Empfang)
RXD    ──────── GPIO 17 (TXD, UART-Senden)
```

**Die SPI-Pins (PC0, PB5, PB6, PB7) bleiben beim STM8!** Der ESP32 fasst sie nicht an.
Dasselbe gilt für die IRQ-Pins PC1, PC2 — die gehen direkt an den STM8 (für CMT2300A-Interrupts).

### UART-Protokoll zwischen ESP32 und STM8 (Vorschlag)

Binär, Big-Endian, mit Start-Byte + Längen-Byte + Payload + XOR-Checksum:

```
Byte 0:  0xAA           Start-Marker
Byte 1:  CMD            Befehls-Code
Byte 2:  LEN            Payload-Länge (0-64)
Byte 3..3+LEN-1:        Payload
Letztes: XOR            XOR über Byte 1..Ende Payload
```

Befehle (Beispiel):
- `0x10  CMD_FREQ_SET`:    payload = 4 Bytes Frequenz in Hz (BE)
- `0x11  CMD_TX_DATA`:     payload = 1 Byte LEN + N Bytes Daten
- `0x12  CMD_RX_START`:    payload leer
- `0x13  CMD_RX_STOP`:     payload leer
- `0x20  EVT_PKT_RX`:      payload = 1 Byte LEN + N Bytes Daten + 1 Byte RSSI
- `0x21  EVT_TX_DONE`:     payload leer
- `0xFF  RSP_ERROR`:       payload = 1 Byte Fehler-Code

Alternativ könnte man das `Ebyte_BSP_*`-Framework der EBYTE-Original-Firmware nachbilden und mit identischen UART-Befehlen arbeiten. Das wäre kompatibel zur Original-Software.


## Schaltplan

```
                          ┌──────────────────┐
                          │  E49-900MBL-01   │
   [3-Pin SWIM]           │   USB-Dongle     │         [9-Pin Stiftleiste]
   ┌────────────┐         │                  │         ┌────────────────────┐
   │  SWIM  ────┼─────────┤ SWIM pad         │    1 GND ─────────┐
   │  NRST  ────┼─────────┤ NRST pad         │    2 3V3 ──┐      │
   │  GND   ────┼─────────┤ GND pad          │    3 PC0   │      │
   └────────────┘         │                  │    4 PB7   │      │
                          │  [STM8L151G]     │    5 PB6   │      │ (nur an STM8,
                          │       │SPI       │    6 PB5   │      │  ESP32 fasst
                          │       │          │    7 PB4   │      │  diese nicht an)
                          │  [CMT2300A]      │    8 PA2 ──┼──────┼──── ESP32 GPIO 16 (RX)
                          │   Funkmodul      │    9 PA3 ──┼──────┼──── ESP32 GPIO 17 (TX)
                          │                  │            │      │
                          └──────────────────┘            │      │
                                                          │      │
                                                          └──────┴──── ESP32 GND & 3.3V
```

Während Phase 1:
```
ESP32                    E49-900MBL-01
─────                    ─────────────
GPIO 25 (SWIM-Master) ── SWIM-Pad
GPIO 26                 ── NRST-Pad
GND                     ── GND-Pad
```

Während Phase 2:
```
ESP32                    E49-900MBL-01
─────                    ─────────────
GPIO 16 (UART-RX)    ─── PA2 (TXD)
GPIO 17 (UART-TX)    ─── PA3 (RXD)
GND                  ─── GND
3.3V                 ─── VIO (3.3V)
```


## Test-Reihenfolge

1. **STM8 flashen** (Phase 1):
   - ESP32 mit SWIM-Master-Sketch flashen
   - ESP32 starten, NRST auf LOW
   - ESP32 schickt "Flash Bridge-Firmware auf STM8"
   - ESP32 lässt NRST los
   - Warten 1s

2. **UART testen** (Phase 2 Setup):
   - ESP32 öffnet Serial2 auf GPIO 16/17 @ 115200 8N1
   - ESP32 schickt 0xAA 0x00 0x00 0x00 (CMD_NOP, keine Payload, XOR=0)
   - STM8 sollte mit 0xAA 0x80 0x00 0x80 (RSP_OK) antworten

3. **Funk testen**:
   - ESP32 schickt CMD_FREQ_SET 868000000
   - ESP32 schickt CMD_TX_DATA "Hallo Welt"
   - Mit zweitem E49-Dongle oder SDR prüfen ob was auf 868 MHz rauskommt

4. **Empfang testen**:
   - ESP32 schickt CMD_FREQ_SET 868000000
   - ESP32 schickt CMD_RX_START
   - STM8 meldet EVT_PKT_RX sobald ein Paket ankommt


## Hinweise

1. **CMT2300A bekommt keinen Reset-Pin** vom ESP32 — der Chip hat Power-On-Reset, das reicht. Nach dem ersten Power-Up läuft er.

2. **3.3V-Pegel**: STM8, ESP32, CMT2300A arbeiten alle mit 3.3V. **Keine Pegelwandler nötig.**

3. **Während Phase 1 darf das Funkmodul nichts senden!** — STM8 ist im Reset, kann also nicht stören. Die Stromversorgung vom Funkmodul läuft aber weiter.

4. **SWIM-Geschwindigkeit**: SWIM läuft mit ~1 MHz maximal. Eine 32 KB Flash-Seite braucht ~30s. Das ist ok, weil einmalig.

5. **Rückfall ohne SWIM**: Falls der Flash fehlschlägt, ist der STM8 nicht gebricktet — er hat ROM-Bootloader (BSL) der über UART geht. Aber wie wir gemerkt haben, ist der auf diesem Board **nicht per USB erreichbar** (kein NRST-Zugriff). In dem Fall hilft nur: nochmal SWIM versuchen, oder STM8-Chip tauschen.

6. **Original-Firmware sichern**: Vor dem Flashen die Original-Firmware aus dem STM8 auslesen! Mit `esp-stlink` und dem Befehl `read_flash 0x8000 0x17FFF` oder ähnlich. Falls die neue Firmware nicht funktioniert, kannst du sie wiederherstellen.
