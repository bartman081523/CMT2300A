#!/bin/bash
# Power-On-Reset BSL-Entry: Dongle-USB aus-/einstecken mit gedrücktem Reset-Knopf
#
# Prozedur:
#   1. ESP32 muss laufen, SWIM-Pin (GPIO25) dauerhaft HIGH
#   2. USB-Knopf am Dongle gedrückt halten
#   3. Dongle-USB einstecken
#   4. Reset-Knopf loslassen
#   5. Innerhalb von 100ms muss stm8gal syncen
#
# Wichtig: STM8L sampelt BS-Pin nur bei Power-On-Reset!
# Wir hoffen, dass ESP32-SWIM HIGH den BS-Pin (= PD1) auf HIGH hält beim POR.

set -e

ESP_PORT=/dev/ttyUSB0
STM8_PORT=/dev/ttyUSB1
STM8GAL=/run/media/julian/ML4/CMT2300A/tools/stm8gal/stm8gal

echo "=== Power-On-Reset BSL-Entry ==="
echo
echo "Vorbereitung:"
echo "  1. ESP32 muss eingesteckt sein und 'READY' zeigen"
echo "  2. Dongle-USB ist AUSGESTECKT"
echo "  3. Reset-Knopf (K1) am Dongle gedrückt halten"
echo
echo "Wenn bereit, USB einstecken, 2s warten, Knopf loslassen,"
echo "dann sofort Enter drücken..."
echo
read -p ">>> Enter wenn Knopf losgelassen wurde <<<"

echo
echo "--- Hämmere 0x7F so schnell wie möglich (3s) ---"
timeout 10 python3 - <<PYEOF
import serial, time, sys

ESP_PORT = "$ESP_PORT"
STM_PORT = "$STM8_PORT"

# ESP32 verifizieren
s_esp = serial.Serial(ESP_PORT, 115200, timeout=0.2)
s_esp.dtr = True; s_esp.rts = True
s_esp.reset_input_buffer()
ready = False
start = time.time()
while time.time() - start < 1.5:
    line = s_esp.readline()
    if line and b'READY' in line:
        ready = True
        break
if not ready:
    print("[err] ESP32 nicht bereit")
    s_esp.close()
    sys.exit(1)
print("[ok] ESP32 bereit, SWIM=HIGH garantiert")
s_esp.close()

# CH340 öffnen und sofort hämmern
bsl = serial.Serial(STM_PORT, 9600, timeout=0.05,
                    parity=serial.PARITY_EVEN)
bsl.reset_input_buffer()

print("Hämmere 0x7F...")
end = time.time() + 3
count = 0
got = []
while time.time() < end:
    bsl.write(bytes([0x7F]))
    count += 1
    time.sleep(0.003)
    r = bsl.read(1)
    if r:
        got.append(r[0])
        if r[0] in (0x79, 0x1F):
            print(f"*** SYNC 0x{r[0]:02X} bei {count} Versuchen ***")
            bsl.close()
            sys.exit(0)
        if r[0] == 0x7F:
            r2 = bsl.read(1)
            if r2 and r2[0] in (0x79, 0x1F):
                print(f"*** 7F+ACK/NACK 0x{r2[0]:02X} bei {count} ***")
                bsl.close()
                sys.exit(0)
print(f"Empfangene Bytes: {' '.join(f'{b:02X}' for b in got[:30])}")
print(f"Kein Sync nach {count} Versuchen")
bsl.close()
PYEOF

echo
echo "--- Falls 0x7F-Sync fehlschlägt, versuche stm8gal direkt ---"
$STM8GAL -q -B -p "$STM8_PORT" -R 1 -i 0 -u 0 -b 9600 \
    -r 0x8000 0x8010 console 2>&1 | tail -5 || true
