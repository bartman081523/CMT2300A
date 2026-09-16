#!/usr/bin/env python3
"""
STM8L BSL Flash-Reader mit manuellem Reset-Trigger.

Anders als die vorherigen Skripts: Wir öffnen den Port mit DTR/RTS
explizit vor dem Sync, halten den Port 'warm' und bieten 3 Strategien:

  1. AUTO:    Bei Start sofort Reset-Puls auf DTR/RTS + Sync
  2. MANUAL:  Wartet auf Enter-Druck und hämmert dann 0x7F
  3. MIXED:   Auto-Pulse + dann manuelles Synchen mit Taster

Funktioniert mit /dev/ttyUSB0 (CH340X).
"""
import serial, time, sys, os, select
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
            port, baud, timeout=0.05,
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
        """Setzt DTR/RTS via TIOCMSET."""
        buf = array.array('I', [0])
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

    def pulse_reset(self, dtr_high=True, rts_high=True, low_ms=50, high_ms=200):
        """Reset-Puls: Zieht die Leitungen kurz runter, lässt sie dann oben."""
        # Auf LOW
        buf = array.array('I', [0])
        fcntl.ioctl(self.s.fd, termios.TIOCMGET, buf, 1)
        bits = buf[0]
        bits &= ~termios.TIOCM_DTR
        bits &= ~termios.TIOCM_RTS
        buf[0] = bits
        fcntl.ioctl(self.s.fd, termios.TIOCMSET, buf, 1)
        time.sleep(low_ms / 1000.0)
        # Auf HIGH
        bits |= termios.TIOCM_DTR
        bits |= termios.TIOCM_RTS
        buf[0] = bits
        fcntl.ioctl(self.s.fd, termios.TIOCMSET, buf, 1)
        time.sleep(high_ms / 1000.0)
        print(f"  [Reset-Puls: LOW {low_ms}ms → HIGH {high_ms}ms]")

    def sync_hammer(self, duration_s=10) -> int:
        """Hämmert 0x7F bis ACK oder NACK kommt. Returns 0x79, 0x1F oder 0."""
        self.s.reset_input_buffer()
        end = time.time() + duration_s
        count = 0
        last_print = time.time()
        while time.time() < end:
            self.s.write(bytes([SYNCH]))
            count += 1
            time.sleep(0.005)
            r = self.s.read(1)
            if r:
                if r[0] == SYNCH:
                    r2 = self.s.read(1)
                    if r2 and (r2[0] == ACK or r2[0] == NACK):
                        return r2[0]
                elif r[0] == ACK or r[0] == NACK:
                    return r[0]
            if time.time() - last_print > 2:
                print(f"  ...noch kein Sync ({count}x 0x7F gesendet)")
                last_print = time.time()
        return 0

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


def read_full_flash(bsl):
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
                break
            time.sleep(0.3)
            bsl.pulse_reset()
            sync = bsl.sync_hammer(duration_s=3)
            if sync != ACK and sync != NACK:
                print("!!! Re-Sync fehlgeschlagen !!!")
                break
    return flash


def main():
    print("=" * 64)
    print("STM8L BSL mit mehreren Reset-Strategien")
    print("=" * 64)
    print(f"Port: {PORT} @ {BAUD} 8E1")
    print(f"Dump: {DUMP_FILE}")
    print()

    bsl = BSL(PORT, BAUD)

    # === Strategie 1: Auto-Reset mit DTR/RTS-Puls ===
    print("\n>>> Strategie 1: Auto-Reset (DTR/RTS-Puls) <<<")
    print(">>> 3 Versuche mit DTR/RTS-Puls <<<")
    for attempt in range(3):
        bsl.pulse_reset(low_ms=50, high_ms=200)
        sync = bsl.sync_hammer(duration_s=3)
        if sync == ACK:
            print(f"  *** ACK nach Puls {attempt+1}! ***")
            flash = read_full_flash(bsl)
            if len(flash) > 0:
                with open(DUMP_FILE, 'wb') as f:
                    f.write(flash)
                print(f"\n>>> DUMP GESPEICHERT: {DUMP_FILE} ({len(flash)} bytes) <<<")
                print(f"Erste 32 Bytes: {flash[:32].hex(' ')}")
                print(f"Letzte 32 Bytes: {flash[-32:].hex(' ')}")
                rst_vec = int.from_bytes(flash[0:4], 'little')
                print(f"Reset-Vektor @ 0x8000: 0x{rst_vec:08X}")
                bsl.close()
                return 0
        elif sync == NACK:
            print(f"  *** NACK nach Puls {attempt+1} - BSL aktiv aber ROP gesetzt ***")
            print("  Versuche ROP zu löschen...")
            # WRITE 0x31 0xCE
            bsl.s.reset_input_buffer()
            bsl.s.write(bytes([0x31, 0xCE]))
            time.sleep(0.05)
            ack = bsl.s.read(1)
            print(f"  WRITE 0x31 Antwort: {ack.hex() if ack else 'nichts'}")
            # dann nochmal versuchen
            sync = bsl.sync_hammer(duration_s=3)
            if sync == ACK:
                flash = read_full_flash(bsl)
                if len(flash) > 0:
                    with open(DUMP_FILE, 'wb') as f:
                        f.write(flash)
                    print(f"\n>>> DUMP GESPEICHERT: {DUMP_FILE} ({len(flash)} bytes) <<<")
                    bsl.close()
                    return 0

    # === Strategie 2: Manueller Reset, getriggert durch Enter ===
    print("\n>>> Strategie 2: Manueller Reset (Taster) <<<")
    print(">>> Drücke den Reset-Knopf JETZT und halte ihn <<<")
    print(">>> Dann Enter drücken <<<")
    input("  ...Enter wenn bereit...")
    print(">>> Hämmere jetzt 0x7F - lass den Knopf alle 2-3s kurz los und drück wieder <<<")
    sync = bsl.sync_hammer(duration_s=20)
    if sync == ACK:
        print(f"  *** ACK! ***")
        flash = read_full_flash(bsl)
        if len(flash) > 0:
            with open(DUMP_FILE, 'wb') as f:
                f.write(flash)
            print(f"\n>>> DUMP GESPEICHERT: {DUMP_FILE} ({len(flash)} bytes) <<<")
            print(f"Erste 32 Bytes: {flash[:32].hex(' ')}")
            rst_vec = int.from_bytes(flash[0:4], 'little')
            print(f"Reset-Vektor @ 0x8000: 0x{rst_vec:08X}")
            bsl.close()
            return 0
    elif sync == NACK:
        print("  *** NACK - ROP wahrscheinlich gesetzt, kein automatisches Umgehen ***")
    else:
        print("  Kein Sync - Taster erreicht BSL nicht (BOOT-Pin / Verdrahtung)")

    bsl.close()
    print("\n!!! Nichts hat funktioniert !!!")
    print("Empfehlung: SWIM-Pins einlöten + ST-Link V2 verwenden")
    return 1


if __name__ == '__main__':
    sys.exit(main())
