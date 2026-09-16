#!/usr/bin/env python3
"""
Manueller BSL-Reset-Catcher.

Die STM8L151G hat einen ROM-Bootloader (AN3155) auf USART1 @ 9600 8E1.
Der wird aktiviert wenn beim Reset der BOOT-Pin HIGH ist (oder wenn
das Flash leer ist).

Da wir den BOOT-Pin nicht kennen, probieren wir es mit dem Reset-Knopf
in einer schnellen Schleife. Drücke den Reset-Knopf EXAKT WÄHREND
der 'PRESS RESET NOW' Phase.

Verwendung:
  python3 bsl_sync_manual_reset.py
"""
import serial, time, sys

PORT = '/dev/ttyUSB0'

def try_sync(baud, listen_s=4):
    """Versuche BSL-Sync bei einer Baudrate."""
    print(f"\n--- Versuche {baud} Baud, 8E1 (BSL-Modus) ---")
    print(">>> DRÜCKE JETZT DEN RESET-KNOPF (1-3x in den nächsten Sekunden) <<<")

    # 8E1 = 8 data bits, even parity, 1 stop bit
    s = serial.Serial(
        PORT, baud, timeout=0.05,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_EVEN,
        stopbits=serial.STOPBITS_ONE,
        exclusive=True
    )
    s.reset_input_buffer()
    s.reset_output_buffer()

    start = time.time()
    acks = 0
    all_rx = bytearray()
    try:
        while time.time() - start < listen_s:
            s.write(b'\x7F')  # BSL sync byte
            time.sleep(0.01)
            chunk = s.read(8)
            if chunk:
                all_rx.extend(chunk)
                if 0x79 in chunk or 0xA5 in chunk:
                    print(f"  >>> BSL-ACK! 0x{chunk.hex()} <<<")
                    acks += 1
                    break
    except KeyboardInterrupt:
        print("\nAbbruch")
        s.close()
        sys.exit(1)

    s.close()

    if acks:
        return True, all_rx
    print(f"  Kein ACK bei {baud} (empfangen: {len(all_rx)} bytes: {all_rx.hex() or 'nichts'})")
    return False, all_rx

print("=" * 64)
print("BSL-Sync mit manuellem Reset")
print("=" * 64)
print("Die STM8L antwortet mit 0x79 wenn sie in den Bootloader geht.")
print("Halte den Reset-Knopf gedrückt WÄHREND der 'PRESS RESET NOW'-Phase.")
print()
print("Start in 3 Sekunden...")
time.sleep(3)

# Probiere alle relevanten Baudraten
for baud in [9600, 19200, 38400, 115200]:
    ok, rx = try_sync(baud, listen_s=5)
    if ok:
        print(f"\n*** BSL-Synchronisiert bei {baud} Baud! ***")
        print(f"Empfangene Bytes: {rx.hex()}")
        # Wechsle zu 8E1 für den Flash-Read
        print(f"\nBereit für stm8gal mit:")
        print(f"  ./stm8gal -p {PORT} -b {baud} -r 0x8000 0xFFFF flash.bin")
        break
else:
    print("\nKein BSL-Sync bei keiner Baudrate. Möglich:")
    print("  - BOOT-Pin ist LOW und nicht über Pin-Header erreichbar")
    print("  - ROM-Bootloader ist gesperrt (Readout Protection)")
    print("  - Reset-Knopf ist nicht der echte Reset, sondern ein anderer Pin")
    print()
    print("Alternative: SWIM über die 3-Pin-Leiste oben (mit ST-Link V2)")
