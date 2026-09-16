#!/usr/bin/env python3
"""
Synchronisiert mit dem BSL und ruft direkt danach stm8gal auf.
Das BSL-Fenster ist sehr kurz - wir dürfen keine Zeit verlieren.

Verwendung:
  python3 bsl_then_dump.py
  -> drücke Reset sobald du 'PRESS RESET NOW' siehst
"""
import serial, time, subprocess, sys, os

PORT = '/dev/ttyUSB0'
BAUD = 9600
STM8GAL = '/run/media/julian/ML4/CMT2300A/tools/stm8gal/stm8gal'
DUMP_FILE = '/run/media/julian/ML4/CMT2300A/tools/stm8_flash_dump.bin'

print("=" * 64)
print("BSL Sync + Flash Dump in einem Schritt")
print("=" * 64)
print(f"Port: {PORT}, Baud: {BAUD}, 8E1")
print(f"Flash-Bereich: 0x8000 - 0xFFFF (32 KB)")
print(f"Dump-Datei: {DUMP_FILE}")
print()
print(">>> Wenn 'PRESS RESET NOW' erscheint: Reset-Knopf drücken <<<")
print()
print("Start in 3s...")
time.sleep(3)

# Schritt 1: BSL-Synchronisieren
print("\n--- Phase 1: BSL-Sync ---")
print(">>> PRESS RESET NOW <<<")
s = serial.Serial(
    PORT, BAUD, timeout=0.05,
    bytesize=serial.EIGHTBITS,
    parity=serial.PARITY_EVEN,
    stopbits=serial.STOPBITS_ONE,
    exclusive=True
)
s.reset_input_buffer()
s.reset_output_buffer()

synced = False
start = time.time()
while time.time() - start < 8:
    s.write(b'\x7F')
    time.sleep(0.01)
    chunk = s.read(8)
    if chunk and (0x79 in chunk or 0xA5 in chunk):
        print(f">>> SYNC OK! empfangen: {chunk.hex()}")
        synced = True
        break

if not synced:
    s.close()
    print("\n!!! Kein Sync bekommen. Versuche es nochmal, Reset früher drücken !!!")
    sys.exit(1)

# Schritt 2: Port geschlossen halten damit stm8gal ihn öffnen kann
# Aber: STM8 bleibt nur kurze Zeit im BSL nach Reset - wir müssen
# den Reset-Status aufrecht erhalten oder sofort übernehmen.
# Trick: Wir schicken direkt stm8gal.
print("\n--- Phase 2: Flash lesen via stm8gal ---")
print("stm8gal startet in 1s, drücke Reset NOCHMAL wenn nötig...")
time.sleep(1)
s.close()
time.sleep(0.5)

# stm8gal mit Reset-Methode 'manual' (default) aufrufen
# Wir wollen: -p PORT -b BAUD -r 0x8000 0xFFFF DUMP_FILE -B (background, keine Prompts)
cmd = [
    STM8GAL,
    '-p', PORT,
    '-b', str(BAUD),
    '-r', '0x8000', '0xFFFF', DUMP_FILE,
    '-B',           # background mode (no prompts)
    '-u', '0',      # UART duplex
    '-R', '1',      # reset: manual (1 = press button)
    '-v', '2',
]
print(f"\n$ {' '.join(cmd)}\n")
result = subprocess.run(cmd, capture_output=False)
print(f"\nstm8gal exit: {result.returncode}")

if os.path.exists(DUMP_FILE):
    size = os.path.getsize(DUMP_FILE)
    print(f"\n>>> DUMP ERFOLGREICH: {DUMP_FILE} ({size} bytes) <<<")
    print(f"Erste 32 Bytes: ", end="")
    with open(DUMP_FILE, 'rb') as f:
        print(f.read(32).hex(' '))
else:
    print("\n!!! DUMP FEHLGESCHLAGEN !!!")
