#!/usr/bin/env python3
"""
Komplett-Test: protokolliert 60s lang alles auf 9600 Baud und
zusammenfasst am Ende.
"""
import serial, time
from collections import Counter

s = serial.Serial('/dev/ttyUSB0', 9600, timeout=0.2, exclusive=True)
s.reset_input_buffer()

print("=" * 64)
print(f"60s Test @ 9600 Baud auf {s.name}")
print("=" * 64)
print("Drücke in dieser Zeit:")
print("  - Reset (1x): Board bootet neu, du siehst die Boot-Message")
print("  - Button 1:  Funk-Sendung 'ping' (#SEND: ping)")
print("  - Button 2:  (andere Funktion, prüfen)")
print("=" * 64)
print()

DURATION = 60
all_data = bytearray()
send_count = 0
recv_count = 0
boot_count = 0
start = time.time()
last_print = time.time()
try:
    while time.time() - start < DURATION:
        chunk = s.read(1)
        if chunk:
            all_data.extend(chunk)
            b = chunk[0]
            if 32 <= b < 127 or b in (10, 13):
                sys.stdout.write(chr(b) if b >= 32 else {'\n':'\\n','\r':'\\r'}[chr(b)])
            else:
                sys.stdout.write(f'[{b:02X}]')
            sys.stdout.flush()
        # Heartbeat
        if time.time() - last_print > 5:
            elapsed = int(time.time() - start)
            print(f"\n[ t={elapsed:3d}s / {DURATION}s, Bytes: {len(all_data)} ]\n", flush=True)
            last_print = time.time()
except KeyboardInterrupt:
    print("\n\nAbgebrochen.")

s.close()
text = all_data.decode('latin-1', errors='replace')

# Statistik
print("\n" + "=" * 64)
print("AUSWERTUNG")
print("=" * 64)
print(f"Gesamt:  {len(all_data)} Bytes in {int(time.time()-start)}s")

send_count = text.count('#SEND')
recv_count = text.count('#RECV')
boot_count = text.count('Wireless module initialization')

print(f"#SEND:   {send_count}x  (= Button 1 gedrückt)")
print(f"#RECV:   {recv_count}x  (= Ping-Antwort empfangen)")
print(f"Boot:    {boot_count}x  (= Reset gedrückt)")

# Sende-Inhalte extrahieren
import re
print()
print("Gefundene Tags:")
for tag in re.findall(r'#(SEND|RECV)[^\r\n]*', text):
    print(f"  #{tag[0]}: {tag[0]!r}")
print()
if all_data:
    # Erste 200 Zeichen
    print("Empfangener Text (erste 400 Zeichen):")
    print("-" * 64)
    print(text[:400])
    print("-" * 64)
