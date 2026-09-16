#!/bin/bash
# Runde 79 (2026-09-07): lokale wmbusmeters-User-Instanz aus dem Repo.
#
#   tools/wmbus-local.sh [PORT] [SEKUNDEN]
#
# - OHNE --useconfig/--daemon: laedt KEINE /etc/wmbusmeters.conf
#   (safe — kein rtl_sdr-Spawn, kein Konflikt mit dem Root-Daemon/Dongle #0).
# - Quelle: unser Dongle via dem r74 offline bewiesenen Pfad stdin:rtlwmbus
#   (rtlwmbus:/dev/ttyX geht NICHT — detectRTLWMBUS=assert(0)).
#   tee speichert den Rohlog parallel nach Log/r79_chip.txt.
# - Meter als INLINE-Specs (4 Tokens name driver id key), generiert aus den
#   Fleet-Files /etc/wmbusmeters/wmbusmeters.d/*.meter. Meter-FILES als
#   Positional-Args lehnt der Build ab ("You can only specify one stdin or
#   one file or one command!" — r74-CLI-Falle: immer 4 Tokens, auch leerer
#   Key '').
# - Kein MQTT-shell — JSON nur nach stdout, keine Duplicate-Receipts am
#   Broker (das Root-Orakel wmbusmeters/$METER_NAME bleibt allein).
PORT=${1:-/dev/ttyACM0}
WIN=${2:-600}
METERS=()
for f in /etc/wmbusmeters/wmbusmeters.d/*.meter; do
    name=$(grep '^name='   "$f" | head -1 | cut -d= -f2-)
    drv=$(grep  '^driver=' "$f" | head -1 | cut -d= -f2-)
    id=$(grep   '^id='     "$f" | head -1 | cut -d= -f2-)
    METERS+=("$name" "$drv" "$id" "")
done
mkdir -p Log
# Runde 79: der tty kann in einem stuck-throttle-Zustand landen (Reader-Wchsel,
# ESP32-Serial-TX-Backpressure: Arduino TX-Ring 256 B, kein Reader -> write blockt
# -> Main-Loop steht -> RX tot). tcsetattr (SET_LINE_CODING) ent-stallt bewiesen
# (r79-Matrix cat4: 0 B -> 3323 B/20s nach stty).
stty -F "$PORT" 115200 raw -echo -crtscts 2>/dev/null
cat "$PORT" | tee "Log/r79_chip.txt" | \
    timeout "$WIN" wmbusmeters --silent --format=json --listento=any \
        stdin:rtlwmbus "${METERS[@]}" \
        > "Log/r79_wmbus.json"
rc=$?
echo "wmbus-local: $WIN s beendet, wmbusmeters rc=$rc; JSON in Log/r79_wmbus.json, Rohlog in Log/r79_chip.txt"