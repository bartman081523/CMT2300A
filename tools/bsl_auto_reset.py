#!/usr/bin/env python3
"""
STM8L BSL Auto-Reset + Read (AN3155).

Probiert verschiedene DTR/RTS-Kombinationen um den STM8-Reset automatisch
auszulösen. CH340X hat DTR+RTS als GPIO. Typisch:
  - DTR  -> STM8 NRST
  - RTS  -> STM8 NRST
  - beide (active-low oder active-high)

Funktioniert nur, wenn der BSL-Eintritts-Pin (BOOT) fest auf VCC liegt
oder das Flash leer ist. Auf dem E49-Dongle ist das nicht garantiert -
deshalb probieren wir mehrere Polaritäten.
"""
import serial, time, sys, os
import array, fcntl, termios

PORT = '/dev/ttyUSB0'
BAUD = 9600
DUMP_FILE = '/run/media/julian/ML4/CMT2300A/tools/stm8_flash_dump.bin'
FLASH_START = 0x8000
FLASH_END   = 0xFFFF
CHUNK       = 128

SYNCH = 0x7F
ACK   = 0x79
NACK  = 0x1F


def xor(data: bytes) -> int:
    x = 0
    for b in data:
        x ^= b
    return x & 0xFF


class BSL:
    def __init__(self, port, baud):
        self.s = serial.Serial(
            port, baud, timeout=0.1,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_EVEN,
            stopbits=serial.STOPBITS_ONE,
            exclusive=True
        )
        self.s.reset_input_buffer()
        self.s.reset_output_buffer()

    def close(self):
        self.s.close()

    def set_lines(self, dtr, rts):
        """Setzt DTR/RTS. None = unverändert."""
        # pyserial setdtr/setrts ist 1.5+ API. Wir nutzen fcntl TIOCMSET für
        # Robustheit.
        if dtr is None and rts is None:
            return
        buf = array.array('I', [0])
        # aktuelle bits lesen
        fcntl.ioctl(self.s.fd, termios.TIOCMGET, buf, 1)
        bits = buf[0]
        if dtr is not None:
            if dtr: bits |=  termios.TIOCM_DTR
            else:   bits &= ~termios.TIOCM_DTR
        if rts is not None:
            if rts: bits |=  termios.TIOCM_RTS
            else:   bits &= ~termios.TIOCM_RTS
        buf[0] = bits
        fcntl.ioctl(self.s.fd, termios.TIOCMSET, buf, 1)

    def pulse_reset(self, dtr=None, rts=None, low_ms=50, high_ms=50):
        """Zieht die gewählte(n) Leitungen kurz auf 0, dann wieder auf 1."""
        if dtr is not None or rts is not None:
            self.set_lines(dtr=0 if dtr is not None else None,
                          rts=0 if rts is not None else None)
            time.sleep(low_ms / 1000.0)
            self.set_lines(dtr=1 if dtr is not None else None,
                          rts=1 if rts is not None else None)
            time.sleep(high_ms / 1000.0)
        else:
            time.sleep(low_ms / 1000.0)

    def sync(self, timeout_s=8) -> bool:
        """BSL-Sync. Akzeptiert ACK (0x79) oder NACK (0x1F). 0x7F = Echo."""
        self.s.reset_input_buffer()
        start = time.time()
        count = 0
        while time.time() - start < timeout_s and count < 200:
            self.s.write(bytes([SYNCH]))
            count += 1
            time.sleep(0.01)
            r = self.s.read(1)
            if not r:
                continue
            if r[0] == SYNCH:
                r2 = self.s.read(1)
                if r2 and (r2[0] == ACK or r2[0] == NACK):
                    print(f"  [BSL sync ok nach {count}x, Antwort 0x{r2[0]:02X}]")
                    return True
                continue
            if r[0] == ACK or r[0] == NACK:
                print(f"  [BSL sync ok nach {count}x, Antwort 0x{r[0]:02X}]")
                return True
        return False

    def cmd(self, code: int) -> bool:
        self.s.reset_input_buffer()
        self.s.write(bytes([code, code ^ 0xFF]))
        time.sleep(0.02)
        ack = self.s.read(1)
        if not ack or ack[0] != ACK:
            return False
        return True

    def read_memory(self, addr: int, length: int) -> bytes:
        if length < 1 or length > 128:
            return b''
        if not self.cmd(0x11):
            return b''
        addr_b = addr.to_bytes(4, 'big')
        self.s.reset_input_buffer()
        self.s.write(addr_b + bytes([xor(addr_b)]))
        time.sleep(0.02)
        ack = self.s.read(1)
        if not ack or ack[0] != ACK:
            return b''
        n = length - 1
        self.s.reset_input_buffer()
        self.s.write(bytes([n, n ^ 0xFF]))
        time.sleep(0.05)
        data = self.s.read(length + 1)
        if not data or len(data) != length + 1:
            return b''
        payload = data[:length]
        checksum = data[length]
        if checksum != xor(payload):
            return b''
        return payload


def try_combo(bsl, dtr_state, rts_state, label):
    """Eine (DTR, RTS) Konfiguration für Reset probieren.
    dtr_state/rts_state: True=HIGH, False=LOW, None=offen"""
    print(f"\n--- Versuche {label} (DTR={'H' if dtr_state else 'L' if dtr_state is not None else '-'}, "
          f"RTS={'H' if rts_state else 'L' if rts_state is not None else '-'}) ---")
    bsl.set_lines(dtr_state, rts_state)
    time.sleep(0.1)
    bsl.pulse_reset(dtr_state, rts_state, low_ms=100, high_ms=100)
    # Sofort nach dem Reset-Puls versuchen zu syncen
    return bsl.sync(timeout_s=4)


def read_full_flash(bsl):
    """Wenn BSL synchronisiert, lies 0x8000-0xFFFF."""
    total = FLASH_END - FLASH_START + 1
    print(f"\n--- Lese {total} Bytes in {total // CHUNK} Chunks ---")
    flash = bytearray()
    addr = FLASH_START
    i = 0
    consecutive_errors = 0
    while addr <= FLASH_END and i < (total + CHUNK - 1) // CHUNK:
        want = min(CHUNK, FLASH_END - addr + 1)
        data = bsl.read_memory(addr, want)
        if len(data) == want:
            flash.extend(data)
            consecutive_errors = 0
            pct = 100 * len(flash) / total
            print(f"  0x{addr:04X}: {len(flash):5d}/{total} bytes ({pct:5.1f}%)", flush=True)
            addr += want
            i += 1
        else:
            consecutive_errors += 1
            print(f"  Fehler bei 0x{addr:04X}, retry {consecutive_errors}/3")
            if consecutive_errors >= 3:
                print("!!! Zu viele Fehler, breche ab !!!")
                break
            time.sleep(0.3)
            if not bsl.sync(timeout_s=3):
                print("!!! Re-Sync fehlgeschlagen !!!")
                break
    return flash


def main():
    print("=" * 64)
    print("STM8L BSL Auto-Reset + Read")
    print("=" * 64)
    print(f"Port: {PORT} @ {BAUD} 8E1")
    print(f"Dump: {DUMP_FILE}")
    print()

    bsl = BSL(PORT, BAUD)

    # Strategie: BSL verlangt, dass beim Reset der BOOT-Pin HIGH ist (oder
    # Flash leer). Wir probieren systematisch alle DTR/RTS-Kombinationen
    # + Reset-Pulse und versuchen nach jedem Pulse zu syncen.

    combos = [
        # (dtr, rts, label) - Endzustand und Resetpuls
        (True,  True,  "DTR+RTS HIGH  (Default)"),
        (True,  False, "DTR HIGH, RTS LOW"),
        (False, True,  "DTR LOW,  RTS HIGH"),
        (False, False, "DTR+RTS LOW"),
        (None,  True,  "DTR offen, RTS HIGH"),
        (None,  False, "DTR offen, RTS LOW"),
        (True,  None,  "DTR HIGH, RTS offen"),
        (False, None,  "DTR LOW,  RTS offen"),
    ]

    for attempt in range(3):
        for dtr, rts, label in combos:
            if try_combo(bsl, dtr, rts, label):
                # Sync erfolgreich → versuchen zu lesen
                flash = read_full_flash(bsl)
                if len(flash) > 0:
                    partial = DUMP_FILE + f'.attempt{attempt}'
                    with open(partial, 'wb') as f:
                        f.write(flash)
                    print(f"\n>>> TEIL-DUMP: {partial} ({len(flash)} bytes) <<<")
                    if len(flash) == (FLASH_END - FLASH_START + 1):
                        with open(DUMP_FILE, 'wb') as f:
                            f.write(flash)
                        print(f">>> VOLL-DUMP: {DUMP_FILE} ({len(flash)} bytes) <<<")
                        print(f"Erste 32 Bytes: {flash[:32].hex(' ')}")
                        print(f"Letzte 32 Bytes: {flash[-32:].hex(' ')}")
                        rst_vec = int.from_bytes(flash[0:4], 'little')
                        print(f"Reset-Vektor @ 0x8000: 0x{rst_vec:08X}")
                        bsl.close()
                        return 0
                # Wenn wir hier landen: sync ok aber read ging nicht —
                # kurz warten und nochmal probieren mit anderem Combo
                time.sleep(1)
        print(f"\n--- Runde {attempt+1} beendet, nochmal probieren ---")
        time.sleep(2)

    print("\n!!! Kein BSL-Sync mit keiner Auto-Reset-Kombination !!!")
    print("Mögliche Ursachen:")
    print("  - BOOT-Pin (BSL-Eintrag) ist nicht auf VCC")
    print("  - Hardware-Reset ist nicht an DTR/RTS angeschlossen")
    print("  - Taster ist nicht der echte Reset, sondern ein Application-Button")
    print("  - Versuche es manuell mit dem Taster (zeitlich genau treffen)")
    bsl.close()
    return 1


if __name__ == '__main__':
    sys.exit(main())
