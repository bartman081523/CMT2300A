#!/usr/bin/env python3
"""
USB-Boot-Fänger für E49-900MBL-01 (E15-EVB02 Eval-Kit)

Was passiert beim Reset der STM8Firmware:
  1. STM8 startet neu
  2. UART-Konsole sendet eine Boot-Message (z.B. Versionsinfo, Init-Status)
  3. Power-LED ist rot, RX-LED grün flackert wenn was reinkommt
  4. Wenn ein zweites Board per Funk pingen würde, würde die grüne LED anzeigen

Dieses Skript:
  - öffnet /dev/ttyUSB0
  - scannt mehrere Baudraten (9600, 19200, 38400, 57600, 115200)
  - sendet KEINE Bytes (Empfang only)
  - hört 60s lang
  - zeigt alles an, was reinkommt
  - am Ende: ASCII- und Hex-Dump + Heuristik (Handshake / Boot-MSG / Müll)

Verwendung:
  python3 catch_boot.py
  -> drücke den Reset-Knopf auf dem Board (3. Knopf, der nicht '按键' heißt)
"""
import serial, sys, time

PORT = '/dev/ttyUSB0'
BAUDS = [9600, 19200, 38400, 57600, 115200]
LISTEN_S = 12   # pro Baudrate

def hexdump(data: bytes) -> str:
    """Schöner 16-Byte-Hexdump"""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        asc_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'  {i:04X}  {hex_part:<47}  {asc_part}')
    return '\n'.join(lines)

def classify(data: bytes, baud: int) -> str:
    """Heuristik: Was haben wir gefangen?"""
    if not data:
        return f'  baud={baud:6d}: STILLE (0 Bytes)'
    # Prüfe auf STM8-BSL-ACK = 0x79
    if 0x79 in data[:4] or 0xA5 in data[:4]:
        return f'  baud={baud:6d}: STOKE BSL! (0x79 oder 0xA5 empfangen) [{len(data)} bytes]'
    # ASCII-dominant?
    printable = sum(1 for b in data if 32 <= b < 127 or b in (10, 13))
    if printable / len(data) > 0.8:
        txt = data.decode('latin-1', errors='replace')
        return f'  baud={baud:6d}: ASCII-Text empfangen ({len(data)} bytes):\n{hexdump(data)}\n  --- decoded ---\n{txt}\n  --- end ---'
    return f'  baud={baud:6d}: rohe {len(data)} Bytes empfangen:\n{hexdump(data)}'

print("=" * 64)
print("USB-Boot-Fänger für E49-900MBL-01 (E15-EVB02)")
print("=" * 64)
print(f"Port: {PORT}")
print(f"Baudraten zum Scannen: {BAUDS}")
print(f"Pro Baudrate: {LISTEN_S} Sekunden hören")
print(f"Gesamtdauer: ca. {len(BAUDS) * LISTEN_S}s")
print()
print(">>> Drücke den Reset-Knopf auf dem Board WÄHREND des Scans <<<")
print(">>> (3. Knopf, der nicht '按键1' / '按键2' heißt) <<<")
print()
print("Fange an...")
print("-" * 64)

all_results = []
for baud in BAUDS:
    print(f'\n>>> Baud {baud} <<<')
    try:
        s = serial.Serial(PORT, baud, timeout=0.2)
    except Exception as e:
        print(f'  FEHLER beim Öffnen: {e}')
        continue
    s.reset_input_buffer()
    s.reset_output_buffer()

    data = bytearray()
    last_print = time.time()
    start = time.time()
    try:
        while time.time() - start < LISTEN_S:
            chunk = s.read(1)
            if chunk:
                data.extend(chunk)
                # Live-Output jedes Bytes
                if 32 <= chunk[0] < 127 or chunk[0] in (10, 13):
                    sys.stdout.write(chr(chunk[0]))
                else:
                    sys.stdout.write(f'[{chunk[0]:02X}]')
                sys.stdout.flush()
                last_print = time.time()
    except KeyboardInterrupt:
        print('\n\nAbgebrochen mit Ctrl-C')
        s.close()
        sys.exit(1)
    s.close()

    print(f'\n{classify(bytes(data), baud)}')
    all_results.append((baud, bytes(data)))

print('\n' + '=' * 64)
print("ZUSAMMENFASSUNG")
print("=" * 64)
for baud, data in all_results:
    if not data:
        print(f'  baud={baud:6d}: STILLE')
    else:
        printable = sum(1 for b in data if 32 <= b < 127 or b in (10, 13))
        ratio = printable / len(data)
        print(f'  baud={baud:6d}: {len(data):4d} Bytes, '
              f'{ratio*100:5.1f}% druckbar, '
              f'erste 16: {data[:16].hex(" ")}')
