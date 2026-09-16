#!/usr/bin/env python3
"""
Live-Monitor: hört 9600 Baud, zeigt alles was reinkommt, mit
Zeitstempel. Drücke auf dem Board Button 1 oder Button 2 während
das Skript läuft.

Verwendung:
  python3 button_monitor.py
  -> drücke Button 1 oder 2 (NICHT Reset!)
"""
import serial, sys, time
from datetime import datetime

s = serial.Serial('/dev/ttyUSB0', 9600, timeout=0.1, exclusive=True)
s.reset_input_buffer()

print("=" * 64)
print(f"Live-Monitor @ 9600 Baud auf {s.name}")
print("=" * 64)
print("Drücke auf dem Board einen der Buttons (按键1 oder 按键2).")
print("Reset-Knopf nicht drücken!")
print("Ctrl-C zum Beenden.")
print()
print("-" * 64)
print(f"{'Zeit':>12}  Hex                       ASCII")
print("-" * 64)

start = time.time()
try:
    while True:
        chunk = s.read(1)
        if chunk:
            t = datetime.now().strftime('%H:%M:%S.%f')[:-3]
            elapsed = time.time() - start
            b = chunk[0]
            asc = chr(b) if 32 <= b < 127 else '.'
            if b in (10, 13):
                asc = {'\n': '\\n', '\r': '\\r'}.get(chr(b), '?')
            print(f"{t}  {b:02X}                        {asc}")
            sys.stdout.flush()
except KeyboardInterrupt:
    print()
    print("Beendet.")
finally:
    s.close()
