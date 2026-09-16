#!/bin/bash
# ============================================================================
# STM8L BSL Flash-Dump mit Power-On-Reset (vollautomatisch)
# ============================================================================
# Wartet automatisch auf Dongle-Trennung/Wiederverbindung, triggert NRST-Pulse
# und liest den Flash. Keine Eingabe erforderlich.
#
# Prozedur (vom User):
#   1. Skript starten
#   2. Dongle ausstecken
#   3. Reset-Knopf K1 am Dongle gedrückt halten
#   4. Dongle einstecken (Knopf weiter halten)
#   5. 1-2s warten, Knopf loslassen
#   6. Skript macht den Rest automatisch
# ============================================================================

set +e  # Fehler NICHT sofort beenden - wir wollen es mehrfach versuchen

# === Konfiguration ===
ESP_PORT=/dev/ttyUSB0
STM8_PORT=/dev/ttyUSB1
STM8GAL=/run/media/julian/ML4/CMT2300A/tools/stm8gal/stm8gal
DUMP_FILE=/tmp/stm8_flash_dump.bin
FLASH_START=0x8000
FLASH_END=0xFFFF
CHUNK=128
BAUD=9600
MAX_RETRIES=10  # viele Versuche, da Timing entscheidend ist

# === Farben ===
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
BLUE='\033[34m'
CYAN='\033[36m'
BOLD='\033[1m'
RESET='\033[0m'

info()    { echo -e "${CYAN}[INFO]${RESET} $*"; }
ok()      { echo -e "${GREEN}[OK]${RESET} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET} $*"; }
err()     { echo -e "${RED}[ERR]${RESET} $*"; }
header()  { echo -e "\n${BOLD}${BLUE}=== $* ===${RESET}"; }

port_exists() {
    [ -e "$1" ]
}

wait_port_gone() {
    local port=$1
    while port_exists "$port"; do
        sleep 0.1
    done
}

wait_port_present() {
    local port=$1 timeout=${2:-60}
    local count=0
    while ! port_exists "$port"; do
        sleep 0.1
        count=$((count + 1))
        if [ $count -gt $((timeout * 10)) ]; then
            return 1
        fi
    done
    return 0
}

# Reset ESP32 (DTR-Toggle) damit Firmware frisch startet
reset_esp32() {
    python3 << 'PYEOF' >/dev/null 2>&1
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
s.dtr = True; s.rts = True; time.sleep(0.1)
s.dtr = False; s.rts = False; time.sleep(0.5)
s.dtr = True; s.rts = True; time.sleep(2.5)
s.reset_input_buffer()
start = time.time()
while time.time() - start < 2:
    line = s.readline()
    if line and b'READY' in line: break
s.close()
PYEOF
}

# NRST-Pulse via ESP32 ('P'-Kommando)
trigger_nrst_pulse() {
    python3 << 'PYEOF' >/dev/null 2>&1
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.5)
s.dtr = True; s.rts = True
time.sleep(0.2)
s.reset_input_buffer()
start = time.time()
while time.time() - start < 2:
    line = s.readline()
    if line and b'READY' in line: break
s.write(b'P\n')
time.sleep(0.1)
start = time.time()
while time.time() - start < 2:
    line = s.readline()
    if not line: continue
    if b'PULSED' in line: break
s.close()
PYEOF
}

# BSL-Sync-Versuch. Gibt "ACK", "NACK" oder "FAIL:bytes" aus
try_bsl_sync() {
    local port=$1 baud=$2
    python3 << PYEOF
import serial, time, sys

bsl = serial.Serial("$port", $baud, timeout=0.05,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_EVEN,
                    stopbits=serial.STOPBITS_ONE)
bsl.reset_input_buffer()

end = time.time() + 5
count = 0
got = []
sync = None
while time.time() < end:
    bsl.write(bytes([0x7F]))
    count += 1
    time.sleep(0.003)
    r = bsl.read(1)
    if r:
        got.append(r[0])
        if r[0] in (0x79, 0x1F):
            sync = r[0]
            break
        if r[0] == 0x7F:
            r2 = bsl.read(1)
            if r2 and r2[0] in (0x79, 0x1F):
                sync = r2[0]
                break

bsl.close()

if sync == 0x79:
    print("ACK")
elif sync == 0x1F:
    print("NACK")
else:
    print(f"FAIL count={count} bytes={' '.join(f'{b:02X}' for b in got[:30])}")
PYEOF
}

read_flash() {
    python3 << PYEOF
import serial, time, sys

PORT = "$STM8_PORT"
BAUD = $BAUD
FLASH_START = $FLASH_START
FLASH_END = $FLASH_END
CHUNK = $CHUNK
DUMP = "$DUMP_FILE"

bsl = serial.Serial(PORT, BAUD, timeout=0.05,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_EVEN,
                    stopbits=serial.STOPBITS_ONE)

def xor(data):
    x = 0
    for b in data: x ^= b
    return x & 0xFF

def send_cmd(code):
    bsl.reset_input_buffer()
    bsl.write(bytes([code, code ^ 0xFF]))
    time.sleep(0.02)
    ack = bsl.read(1)
    return ack and ack[0] == 0x79

def read_memory(addr, length):
    if not send_cmd(0x11): return b''
    addr_b = addr.to_bytes(4, 'big')
    bsl.reset_input_buffer()
    bsl.write(addr_b + bytes([xor(addr_b)]))
    time.sleep(0.02)
    ack = bsl.read(1)
    if not ack or ack[0] != 0x79: return b''
    n = length - 1
    bsl.reset_input_buffer()
    bsl.write(bytes([n, n ^ 0xFF]))
    time.sleep(0.05)
    data = bsl.read(length + 1)
    if not data or len(data) != length + 1: return b''
    payload = data[:length]
    checksum = data[length]
    if checksum != xor(payload): return b''
    return payload

def re_sync():
    end = time.time() + 2
    bsl.reset_input_buffer()
    while time.time() < end:
        bsl.write(bytes([0x7F]))
        time.sleep(0.003)
        r = bsl.read(1)
        if r and r[0] == 0x79: return True
    return False

total = FLASH_END - FLASH_START + 1
flash = bytearray()
addr = FLASH_START
i = 0
errs = 0
while addr <= FLASH_END and i < (total + CHUNK - 1) // CHUNK:
    want = min(CHUNK, FLASH_END - addr + 1)
    data = read_memory(addr, want)
    if len(data) == want:
        flash.extend(data)
        errs = 0
        pct = 100 * len(flash) / total
        print(f"\r  0x{addr:04X}: {len(flash):5d}/{total} bytes ({pct:5.1f}%)", end='', flush=True)
        addr += want
        i += 1
    else:
        errs += 1
        print(f"\n  [err] 0x{addr:04X}, retry {errs}/3")
        if errs >= 3: break
        time.sleep(0.3)
        if not re_sync():
            print("  [err] Re-Sync fehlgeschlagen")
            break

print()
if len(flash) > 0:
    with open(DUMP, 'wb') as f:
        f.write(flash)
    print(f"DUMP_OK file={DUMP} bytes={len(flash)}")
else:
    print("DUMP_FAIL")
    sys.exit(1)
bsl.close()
PYEOF
}

# ============================================================================
# HAUPTPROGRAMM
# ============================================================================
clear
echo -e "${BOLD}${BLUE}"
cat << "EOF"
╔══════════════════════════════════════════════════════════════╗
║                                                              ║
║     STM8L BSL Flash-Dump (vollautomatisch)                   ║
║                                                              ║
║     Liest den Flash des STM8L151G über UART-Bootloader       ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
EOF
echo -e "${RESET}"

info "Verkabelung (bitte prüfen):"
echo "    ESP32 GPIO25 → Dongle TP2 (SWIM/BS)"
echo "    ESP32 GPIO26 → Dongle TP1 (NRST)"
echo "    ESP32 GND    → Dongle TP3 (GND)"
echo

# === Schritt 1: ESP32 prüfen ===
header "Schritt 1: ESP32 prüfen"
if ! port_exists "$ESP_PORT"; then
    err "ESP32 nicht gefunden an $ESP_PORT"
    info "Bitte ESP32 einstecken und Skript neu starten."
    exit 1
fi
ok "ESP32 gefunden"
info "Reset ESP32..."
reset_esp32
ok "ESP32 bereit (SWIM=BS=HIGH garantiert)"

# === Schritt 2: Auf Dongle-Trennung warten (oder direkt weiter) ===
header "Schritt 2: Auf Dongle-Trennung warten (falls noch eingesteckt)"
if port_exists "$STM8_PORT"; then
    warn "Dongle ist eingesteckt. Bitte JETZT ausstecken!"
    info "Warte auf Trennung..."
    wait_port_gone "$STM8_PORT"
    ok "Dongle getrennt"
else
    ok "Dongle ist bereits getrennt"
fi

# === Schritt 3: Auf Wiedereinsteecken mit gedrücktem Reset warten ===
header "Schritt 3: Stecke Dongle mit GEDRÜCKTEM Reset-Knopf (K1) wieder ein"
echo
echo "    1. Reset-Knopf K1 am Dongle GEDRÜCKT halten"
echo "    2. USB-Kabel einstecken (Knopf weiter halten!)"
echo "    3. 1-2s warten bis Power-LEDs leuchten"
echo "    4. Reset-Knopf loslassen"
echo
info "Warte auf Dongle..."

LAST_STATUS=""
while ! port_exists "$STM8_PORT"; do
    sleep 0.2
done
ok "Dongle erkannt!"
sleep 0.5

# === Schritt 4: BSL-Sync versuchen (mit vielen Retries) ===
header "Schritt 4: BSL-Sync (max $MAX_RETRIES Versuche)"

SUCCESS=0
for attempt in $(seq 1 $MAX_RETRIES); do
    echo
    info "--- Versuch $attempt/$MAX_RETRIES ---"

    # 1. NRST-Pulse
    trigger_nrst_pulse
    sleep 0.1

    # 2. Sofort BSL-Sync
    info "Hämmere 0x7F (8E1, $BAUD baud) für 5s..."
    SYNC_RESULT=$(try_bsl_sync "$STM8_PORT" "$BAUD" 2>&1)
    SYNC_RESULT=$(echo "$SYNC_RESULT" | tail -1)  # nur letzte Zeile

    case "$SYNC_RESULT" in
        ACK)
            ok "✓ BSL aktiv! (ACK empfangen)"
            SUCCESS=1
            break
            ;;
        NACK)
            warn "NACK - BSL aktiv, aber ROP (Read-Out-Protection) gesetzt"
            warn "Versuche ROP zu löschen..."
            python3 << 'PYEOF' 2>&1 | tail -1
import serial, time
bsl = serial.Serial('/dev/ttyUSB1', 9600, timeout=0.05,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_EVEN,
                    stopbits=serial.STOPBITS_ONE)
bsl.reset_input_buffer()
bsl.write(bytes([0x31, 0xCE]))
time.sleep(0.05)
r = bsl.read(1)
bsl.close()
print(f"ROP-Clear Antwort: {r.hex() if r else 'nichts'}")
PYEOF
            info "Re-Sync..."
            SYNC_RESULT=$(try_bsl_sync "$STM8_PORT" "$BAUD" 2>&1 | tail -1)
            if [ "$SYNC_RESULT" = "ACK" ]; then
                ok "✓ BSL aktiv nach ROP-Clear"
                SUCCESS=1
                break
            fi
            ;;
        FAIL*)
            err "Sync fehlgeschlagen: $SYNC_RESULT"
            if [ $attempt -lt $MAX_RETRIES ]; then
                echo
                warn "Mache automatisch nächsten Versuch in 2s..."
                warn "Falls Reset-Knopf-Timing das Problem war:"
                warn "  - Dongle ausstecken"
                warn "  - Reset-Knopf K1 gedrückt halten"
                warn "  - Dongle einstecken, 1-2s warten, Knopf loslassen"
                sleep 2
            fi
            ;;
    esac
done

if [ $SUCCESS -ne 1 ]; then
    err "BSL konnte nicht aktiviert werden nach $MAX_RETRIES Versuchen."
    echo
    echo -e "${BOLD}${YELLOW}Mögliche Ursachen:${RESET}"
    echo "  1. BSL ist per OPT-Byte deaktiviert → nur SWIM-Hardware hilft"
    echo "  2. Kabel ESP32 ↔ Dongle prüfen (Durchgang mit Multimeter)"
    echo "  3. ESP32-Firmware läuft nicht → Reset-Knopf am ESP32 drücken"
    echo "  4. Reset-Knopf-Timing am Dongle variieren (1-3s gedrückt halten)"
    echo
    echo "Wenn alle Stricke reißen: 3-polige Stiftleiste auf SWIM-Pads löten,"
    echo "dann ST-Link V2 oder ESP32-SWIM-Tool benutzen."
    exit 1
fi

# === Schritt 5: Flash lesen ===
header "Schritt 5: Flash auslesen"
echo
info "Lese 0x$FLASH_START - 0x$FLASH_END in ${CHUNK}-Byte-Chunks..."

read_flash
READ_EXIT=$?

echo
if [ $READ_EXIT -eq 0 ] && [ -f "$DUMP_FILE" ]; then
    echo
    echo -e "${BOLD}${GREEN}╔═══════════════════════════════════════════════════════════╗"
    echo -e "║                                                           ║"
    echo -e "║     ✓ ERFOLG: Flash-Dump gespeichert!                     ║"
    echo -e "║                                                           ║"
    echo -e "╚═══════════════════════════════════════════════════════════╝${RESET}"
    echo
    ok "Datei: $DUMP_FILE"
    info "Größe: $(stat -c%s "$DUMP_FILE") bytes"
    info "Erste 32 Bytes (Hex):"
    xxd -l 32 "$DUMP_FILE" | head -2
    info "Reset-Vektor @ 0x8000: 0x$(xxd -l 4 -p "$DUMP_FILE")"
    echo
    info "Analyse-Tipps:"
    echo "  - strings $DUMP_FILE | head -30         (sichtbare Strings)"
    echo "  - xxd $DUMP_FILE | head -50             (Hex-Dump)"
else
    err "Flash-Lesen fehlgeschlagen (exit $READ_EXIT)"
    exit 1
fi
