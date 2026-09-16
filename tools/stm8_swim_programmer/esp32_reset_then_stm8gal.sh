#!/bin/bash
# Use ESP32 to give NRST pulse, then run stm8gal to talk to STM8 bootloader via CH340
#
# Wires:
#   ESP32 GPIO26 (NRST)  ->  Dongle TP1 (NRST)
#   ESP32 GND            ->  Dongle TP3 (GND)
#   CH340 (Dongle USB)   ->  STM8 UART (PA2/PA3) - already on board
#
# Two paths are tried:
#   Path A: NRST pulse via ESP32, then stm8gal -R 1 (manual reset)
#           This works if the STM8 enters BSL after reset by default.
#   Path B: stm8gal -R 3 (Re5eT! software reset via UART)
#           This is the standard way and does NOT need hardware NRST control.
#
# BSL entry condition for STM8L:
#   After NRST rising edge, BS pin (PD1/SWIM) must be HIGH.
#   Without controlling BS, we rely on default state.

set -e

ESP_PORT=/dev/ttyUSB1  # CP2102 = ESP32
STM8_PORT=/dev/ttyUSB0  # CH340 = Dongle
STM8GAL=/run/media/julian/ML4/CMT2300A/tools/stm8gal/stm8gal

esp32_nrst_pulse() {
    echo
    echo "=== [$1] ESP32 NRST pulse ==="
    timeout 15 python3 - <<PYEOF
import serial, time, sys

ESP_PORT = "$ESP_PORT"
s = serial.Serial(ESP_PORT, 115200, timeout=1)
s.dtr = True;  s.rts = True
time.sleep(0.2)
s.dtr = False; s.rts = True
time.sleep(0.2)
s.dtr = True;  s.rts = True
time.sleep(0.3)
s.reset_input_buffer()

# Read until READY
ready = False
start = time.time()
while time.time() - start < 8:
    line = s.readline()
    if not line: continue
    txt = line.decode('utf-8', errors='replace').rstrip()
    print(f"[esp32] {txt}")
    if 'READY' in txt:
        ready = True
        break

if not ready:
    print("[err] ESP32 not ready, aborting")
    s.close()
    sys.exit(1)

s.write(b'P\n')
time.sleep(0.1)

pulsed = False
start = time.time()
while time.time() - start < 3:
    line = s.readline()
    if not line: continue
    txt = line.decode('utf-8', errors='replace').rstrip()
    print(f"[esp32] {txt}")
    if 'PULSED' in txt:
        pulsed = True
        break

if not pulsed:
    print("[err] NRST pulse not confirmed, aborting")
    s.close()
    sys.exit(1)

print("[ok] NRST pulse done, STM8 should be in reset release")
s.close()
PYEOF
}

# -------------------------------------------------------------------------
# Path A: hardware reset via ESP32 + manual stm8gal reset
# -------------------------------------------------------------------------
esp32_nrst_pulse "A"
echo
echo "=== [A] stm8gal with manual reset (-R 1), reading 0x8000-0x807F ==="
$STM8GAL \
    -p "$STM8_PORT" \
    -R 1 \
    -i 0 \
    -u 0 \
    -b 115200 \
    -r 0x8000 0x807F console 2>&1 | tail -20 || true
