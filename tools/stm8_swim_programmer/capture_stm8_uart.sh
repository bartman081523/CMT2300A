#!/bin/bash
# ============================================================================
# STM8 UART1 Fuzzy-Reader (PARALLEL)
# ============================================================================
# Liest CH340 mit ALLEN Konfigurationen GLEICHZEITIG in Threads.
# So verpasst man keine UART-Ausgabe, egal welche Baudrate der STM8 nutzt.
#
# Während des Tests soll der User RESET, BUTTON1, BUTTON2 am Dongle drücken.
# ============================================================================

CH340=/dev/ttyUSB1
LOGFILE=/tmp/stm8_uart_capture.log

# Farben
RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'; CYAN='\033[36m'; BOLD='\033[1m'; RESET='\033[0m'
info() { echo -e "${CYAN}[INFO]${RESET} $*"; }
ok()   { echo -e "${GREEN}[OK]${RESET} $*"; }

if [ ! -e "$CH340" ]; then
    echo -e "${RED}[ERR]${RESET} $CH340 nicht gefunden"
    exit 1
fi

> "$LOGFILE"
info "Logge nach $LOGFILE"
info "Dauer: ${DURATION}s, alle Konfigurationen parallel"
echo
echo -e "${BOLD}Drücke jetzt (oder gleich):${RESET}"
echo "  - RESET-Knopf (K1) am Dongle"
echo "  - BUTTON1 (PA4)"
echo "  - BUTTON2 (PA5)"
echo

# Starte EINEN Python-Prozess, der ALLE Konfigurationen parallel in Threads liest
timeout $((DURATION + 5)) python3 << 'PYEOF' 2>&1
import serial
import threading
import time
import re
import sys
import os
from datetime import datetime

PORT = "/dev/ttyUSB1"
DURATION = int(os.environ.get("DURATION", "30"))
LOGFILE = os.environ.get("LOGFILE", "/tmp/stm8_uart_capture.log")

BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200]
PARITIES = [
    ("N", serial.PARITY_NONE),
    ("E", serial.PARITY_EVEN),
    ("O", serial.PARITY_ODD),
]

stop_event = threading.Event()
results_lock = threading.Lock()
all_hits = []  # (timestamp, baud, parity, text)

def reader_thread(baud, parity_name, parity):
    """Liest eine Konfiguration kontinuierlich bis stop_event gesetzt."""
    ser = None
    try:
        ser = serial.Serial(
            PORT, baud, timeout=0.1,
            bytesize=serial.EIGHTBITS,
            parity=parity,
            stopbits=serial.STOPBITS_ONE,
            exclusive=True
        )
    except Exception as e:
        with results_lock:
            all_hits.append((time.time(), baud, parity_name, f"OPEN-ERR: {e}"))
        return

    # Warum exclusive? Damit andere Threads die selbe Konfiguration nicht stören.
    # Bei verschiedenen Baudraten ist das sowieso ok, aber wir wollen den CH340
    # nicht durch zu viele offene Handles überlasten.

    buffer = bytearray()
    last_print = time.time()
    local_start = time.time()

    while not stop_event.is_set():
        try:
            r = ser.read(64)
            if r:
                buffer.extend(r)
        except Exception:
            break

        # Alle 1s prüfen ob wir echten ASCII-Text haben
        if time.time() - last_print >= 1.0 and len(buffer) > 0:
            # Suche ALPHANUMERISCHE Sequenzen (≥4 Zeichen, nur a-zA-Z0-9 Leerzeichen Satzzeichen)
            printable = buffer.decode('ascii', errors='replace')
            # Filtere alles weg, was nicht „echter Text" ist:
            #   ≥4 aufeinanderfolgende druckbare Zeichen, mit mindestens 50% alphanumerisch
            runs = re.findall(r'[\x20-\x7E]{4,}', printable)
            good_runs = []
            for run in runs:
                alnum = sum(1 for c in run if c.isalnum())
                if alnum >= max(4, len(run) * 0.4):
                    good_runs.append(run.strip())
            if good_runs:
                joined = ' | '.join(good_runs[:5])
                ts = time.time() - local_start
                hit = f"[t+{ts:.1f}s] BAUD={baud:6d} P={parity_name}: {joined[:120]}"
                with results_lock:
                    all_hits.append((time.time(), baud, parity_name, hit))
                # Live ausgeben
                print(f"\033[33m{hit}\033[0m", flush=True)
                with open(LOGFILE, 'a') as f:
                    f.write(hit + "\n")
            buffer.clear()
            last_print = time.time()

    if ser:
        try:
            ser.close()
        except:
            pass

# Alle Threads starten
threads = []
print(f"\033[36m[INFO]\033[0m Starte {len(BAUDS)*len(PARITIES)} Reader-Threads...", flush=True)
for baud in BAUDS:
    for pname, pval in PARITIES:
        t = threading.Thread(target=reader_thread, args=(baud, pname, pval), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(0.05)  # kleine Pause damit die Handles sauber öffnen

print(f"\033[36m[INFO]\033[0m Alle Threads laufen. Drücke jetzt Buttons!", flush=True)

# Hauptschleife: alle 5s Status ausgeben
start = time.time()
while time.time() - start < DURATION:
    time.sleep(5)
    elapsed = time.time() - start
    with results_lock:
        n_hits = len(all_hits)
    print(f"\033[36m[INFO]\033[0m t+{elapsed:.0f}s/{DURATION}s - {n_hits} Treffer bisher. Drücke weiter!", flush=True)

# Stop signal
stop_event.set()
time.sleep(1)

# Auf alle Threads warten
for t in threads:
    t.join(timeout=2)

print()
print("=" * 60)
print(f"Aufnahme beendet. {len(all_hits)} Treffer insgesamt.")
print(f"Log-Datei: {LOGFILE}")
print("=" * 60)
PYEOF

DURATION="$DURATION" LOGFILE="$LOGFILE"  # nur damit env weitergegeben wird
echo
echo "Letzte 30 Treffer aus dem Log:"
echo "------------------------------------------------------------"
tail -30 "$LOGFILE"
