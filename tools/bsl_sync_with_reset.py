#!/usr/bin/env python3
"""
Sendet 0x7F (BSL-Sync-Byte) in einer Schleife, während du den
Reset-Knopf drückst. Wenn der STM8L in den Bootloader springt,
antwortet er mit 0x79 oder 0xA5 (ACK oder Echo).

Verwendung: Drücke den Reset-Knopf am Anfang der Schleife.
"""
import serial, sys, time

s = serial.Serial('/dev/ttyUSB0', 9600, timeout=0.05)
print("Sending 0x7F every 50ms. Press RESET on the board NOW!")
print("Will run for 15s. Press Ctrl-C to abort earlier.")

start = time.time()
acks = 0
try:
    while time.time() - start < 15:
        s.write(b'\x7F')
        time.sleep(0.05)
        # Try to read response
        resp = s.read(1)
        if resp:
            print(f"  Got byte: 0x{resp[0]:02X}  (ACK expected: 0x79)", flush=True)
            acks += 1
        time.sleep(0.05)
except KeyboardInterrupt:
    pass

print(f"\nTotal: {acks} bytes received during 15s.")
s.close()
