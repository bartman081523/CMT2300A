#!/usr/bin/env python3
"""
Robust USB-Fänger mit Reset-Trigger.
- CH340 muss zwischen Baudraten kurz atmen
- Reset-Knopf wird per Skript angekündigt, du drückst
- Daten werden sofort live angezeigt
- Am Ende: Statistik + Heuristik
"""
import serial, sys, time, os

PORT = '/dev/ttyUSB0'
BAUDS = [9600, 19200, 38400, 57600, 115200]
LISTEN_S = 8
PAUSE_S = 2  # CH340 erholen

def hexdump_short(data: bytes, n: int = 64) -> str:
    return ' '.join(f'{b:02X}' for b in data[:n])

def classify(data: bytes, baud: int) -> str:
    if not data:
        return f'  STILLE'
    if 0x79 in data[:8]:
        return f'  ** BSL-ACK (0x79) gefunden! **'
    if data.count(0x7E) >= 2:
        return f'  0x7E-Frame-Marker {data.count(0x7E)}x — wahrscheinlich Funkdaten oder Frame-Protokoll'
    printable = sum(1 for b in data if 32 <= b < 127 or b in (10, 13))
    if printable / len(data) > 0.7:
        return f'  ASCII-Text: {data[:80].decode("latin-1", errors="replace")!r}'
    return f'  rohe Bytes'

print("=" * 64)
print("USB-Fänger v2: Reset-getriggert, mit CH340-Erholungspausen")
print("=" * 64)
print(f"Port: {PORT}")
print(f"Baudraten: {BAUDS}")
print(f"Hörzeit pro Baudrate: {LISTEN_S}s")
print(f"Pause dazwischen: {PAUSE_S}s")
print()
print(">>> Drücke den Reset-Knopf auf dem Board WÄHREND jeder Baudrate-Phase <<<")
print(">>> Du hast {LISTEN_S} Sekunden pro Baudrate, Reset so oft du willst <<<")
print()
input("Drücke Enter zum Start...")
print()

results = []
for i, baud in enumerate(BAUDS):
    if i > 0:
        print(f"\n[CH340-Erholungspause {PAUSE_S}s ...]")
        time.sleep(PAUSE_S)

    print(f"\n{'='*64}")
    print(f"PHASE {i+1}/{len(BAUDS)}: BAUD {baud}")
    print(f"{'='*64}")
    print(f">>> JETZT RESET DRÜCKEN (so oft du willst, du hast {LISTEN_S}s) <<<")
    print()

    try:
        s = serial.Serial(PORT, baud, timeout=0.2, exclusive=True)
    except Exception as e:
        print(f"FEHLER: {e}")
        continue

    s.reset_input_buffer()
    s.reset_output_buffer()

    data = bytearray()
    start = time.time()
    last_print = time.time()
    try:
        while time.time() - start < LISTEN_S:
            chunk = s.read(1)
            if chunk:
                data.extend(chunk)
                b = chunk[0]
                if 32 <= b < 127 or b in (10, 13):
                    sys.stdout.write(chr(b))
                else:
                    sys.stdout.write(f'[{b:02X}]')
                sys.stdout.flush()
                last_print = time.time()
            # Heartbeat alle 2s
            if time.time() - last_print > 2 and len(data) == 0:
                elapsed = int(time.time() - start)
                print(f"  [t={elapsed}s, noch nichts...]", flush=True)
                last_print = time.time()
    except KeyboardInterrupt:
        print("\n\nAbgebrochen.")
        s.close()
        sys.exit(1)
    except serial.SerialException as e:
        print(f"\n[Serial-Fehler: {e}]")
        s.close()
        time.sleep(PAUSE_S)
        continue

    s.close()

    print(f"\n  -> {len(data)} bytes empfangen")
    print(f"  -> {classify(bytes(data), baud)}")
    if data:
        print(f"  -> erste 64 bytes: {hexdump_short(bytes(data))}")
    results.append((baud, bytes(data)))

print()
print("=" * 64)
print("ZUSAMMENFASSUNG")
print("=" * 64)
print(f"{'Baud':>8}  {'Bytes':>6}  Status")
print("-" * 64)
for baud, data in results:
    if not data:
        print(f"{baud:>8}  {0:>6}  stille")
    elif 0x79 in data[:8]:
        print(f"{baud:>8}  {len(data):>6}  *** BSL ACK ***")
    elif data.count(0x7E) >= 2:
        print(f"{baud:>8}  {len(data):>6}  Funk-/Frame-Stream ({data.count(0x7E)}x 0x7E)")
    elif sum(1 for b in data if 32 <= b < 127) / max(len(data), 1) > 0.7:
        print(f"{baud:>8}  {len(data):>6}  ASCII-Text")
    else:
        print(f"{baud:>8}  {len(data):>6}  rohe Bytes")

print()
print("Rohdaten in: /run/media/julian/ML4/CMT2300A/tools/capture_results.bin")
with open('/run/media/julian/ML4/CMT2300A/tools/capture_results.bin', 'wb') as f:
    for baud, data in results:
        f.write(f"=== BAUD {baud} ({len(data)} bytes) ===\n".encode())
        f.write(data)
        f.write(b"\n")
print("Fertig.")
