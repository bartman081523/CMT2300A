#!/usr/bin/env python3
"""
Aufnahme bei 19200 Baud, 60s lang, mit Frame-Erkennung.
Zielt darauf, periodische Muster zu finden, die zeigen ob der
Datenstrom absichtlich oder zufällig ist.
"""
import serial, time, sys
from collections import Counter

s = serial.Serial('/dev/ttyUSB0', 19200, timeout=1)
s.reset_input_buffer()

print("Aufnahme 19200 Baud, 60s lang.")
print("Drücke KEINEN Reset — wir wollen sehen ob es einen konstanten Stream gibt.")
print()

start = time.time()
all_data = bytearray()
last_report = 0
frame_starts = []
try:
    while time.time() - start < 60:
        chunk = s.read(256)
        if chunk:
            all_data.extend(chunk)
            for b in chunk:
                if b == 0x7E:
                    frame_starts.append(len(all_data) - 1)
        elapsed = int(time.time() - start)
        if elapsed - last_report >= 5:
            last_report = elapsed
            print(f"  t={elapsed:3d}s: {len(all_data):6d} bytes, "
                  f"{len(frame_starts):4d} 0x7E-Marker", flush=True)
except KeyboardInterrupt:
    print("\nAbgebrochen")

s.close()

print(f"\nGesamt: {len(all_data)} Bytes, {len(frame_starts)} 0x7E-Marker")
if frame_starts:
    diffs = [frame_starts[i+1] - frame_starts[i] for i in range(len(frame_starts)-1)]
    if diffs:
        avg = sum(diffs) / len(diffs)
        print(f"  0x7E-Marker-Abstände: avg={avg:.1f} bytes, "
              f"min={min(diffs)}, max={max(diffs)}")
        # Häufigste Differenz
        c = Counter(diffs)
        top = c.most_common(5)
        print(f"  Häufigste Abstände: {top}")

# Erste 3 "Frames" (zwischen 0x7E-Markern) anzeigen
print("\nErste 3 Frames:")
if len(frame_starts) >= 4:
    for i in range(3):
        f = all_data[frame_starts[i]:frame_starts[i+1]]
        print(f"  Frame {i}: {len(f):4d} bytes")
        print(f"    {f[:32].hex(' ')}")

# Häufigste Bytes
print("\nByte-Häufigkeit (Top 10):")
c = Counter(all_data)
for byte, count in c.most_common(10):
    pct = 100 * count / len(all_data)
    print(f"  0x{byte:02X} ({chr(byte) if 32 <= byte < 127 else '?'}): {count:5d} ({pct:5.1f}%)")
