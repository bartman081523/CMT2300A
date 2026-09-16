#!/bin/bash
# Flash ESP32 SWIM programmer + capture serial output
#
# Wires:
#   ESP32 GPIO25 (SWIM_PIN)  ->  Dongle TP2 (SWIM)
#   ESP32 GPIO35             ->  Dongle TP2 (SWIM, LA listen-only)
#   ESP32 GPIO26 (NRST)      ->  Dongle TP1 (NRST)
#   ESP32 GND                ->  Dongle TP3 (GND)
#
# Identified ports on this machine:
#   /dev/ttyUSB0  = ESP32 (CP2102)  -- flash target, monitor output
#   /dev/ttyUSB1  = E49 Dongle (CH340) -- not used in this test

set -e

ESP_PORT=/dev/ttyUSB0
PROJECT_DIR=$(cd "$(dirname "$0")" && pwd)
BAUD=115200

cd "$PROJECT_DIR"

echo "=== [1/3] Building ==="
pio run 2>&1 | tail -5

echo
echo "=== [2/3] Uploading to ESP32 ($ESP_PORT) ==="
pio run -t upload --upload-port "$ESP_PORT" 2>&1 | tail -10

echo
echo "=== [3/3] Capturing serial output (30s timeout) ==="
echo "    Press RESET button on ESP32 if nothing appears within 5 seconds."
echo

# Use timeout so the script doesn't hang if ESP32 hangs
timeout 30 python3 -c "
import serial, time, sys
s = serial.Serial('$ESP_PORT', $BAUD, timeout=1)
time.sleep(0.5)
# Toggle DTR to reset ESP32 (auto-reset on most dev boards)
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setDTR(True);  s.setRTS(False); time.sleep(0.1)
s.setDTR(False); s.setRTS(False); time.sleep(0.5)
s.reset_input_buffer()
start = time.time()
while time.time() - start < 25:
    line = s.readline()
    if not line: continue
    sys.stdout.write(line.decode('utf-8', errors='replace'))
    sys.stdout.flush()
    if b'Diagnostic complete' in line:
        break
s.close()
" 2>&1

echo
echo "=== Done ==="
