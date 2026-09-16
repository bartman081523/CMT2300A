#!/usr/bin/env python3
"""
Liest CH340 in einer Endlosschleife und speichert alles was kommt.
Drücke jetzt den Reset-Knopf auf dem Board.

Verwendung:
  python3 read_with_reset.py [baud]
"""
import serial, sys, time

baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
s = serial.Serial('/dev/ttyUSB0', baud, timeout=1)
print(f"Reading /dev/ttyUSB0 @ {baud}. Press RESET on the board now!")
print("Will run for 30s, then exit. Press Ctrl-C to abort earlier.")

start = time.time()
data = bytearray()
last_heartbeat = 0
try:
    while time.time() - start < 30:
        chunk = s.read(1)
        if chunk:
            data.extend(chunk)
            sys.stdout.write(chunk.decode('latin-1', errors='replace'))
            sys.stdout.flush()
        else:
            elapsed = int(time.time() - start)
            if elapsed - last_heartbeat >= 3:
                print(f"\n[still listening, t={elapsed}s, got {len(data)} bytes so far]\n", flush=True)
                last_heartbeat = elapsed
except KeyboardInterrupt:
    pass

print(f"\n\nTotal: {len(data)} bytes")
if data:
    print(f"Hex: {data.hex()}")
s.close()
