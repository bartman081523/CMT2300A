#!/usr/bin/env python3
"""
STM8L BSL Flash-Reader (AN3155-konform, stm8gal-kompatibel).

Protokoll (stm8gal/src/bootloader.c, AN3155):
  Sync:  0x7F   bis ACK (0x79) ODER NACK (0x1F) kommt
         (0x7F-Echo ignorieren, 1-Wire-Read nochmal versuchen)
  Cmd:   0x11 0xEE    -> ACK
  Addr:  4B BE + 1B XOR -> ACK
  N:     N-1  ~N      -> N Datenbytes + 1B XOR

Verwendung:
  python3 bsl_read_flash.py
  -> drücke Reset-Knopf wenn 'PRESS RESET NOW' erscheint
"""
import serial, time, sys, os

PORT = '/dev/ttyUSB0'
BAUD = 9600
DUMP_FILE = '/run/media/julian/ML4/CMT2300A/tools/stm8_flash_dump.bin'
FLASH_START = 0x8000
FLASH_END   = 0xFFFF
CHUNK       = 128   # BSL max 128 Bytes pro Read

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

    def sync(self, timeout_s=15) -> bool:
        """BSL-Sync wie stm8gal: sendet 0x7F bis ACK(0x79) oder NACK(0x1F).
        Ein 0x7F-Echo (1-Wire) wird verworfen."""
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
                # 1-Wire Echo: nochmal lesen
                r2 = self.s.read(1)
                if r2 and (r2[0] == ACK or r2[0] == NACK):
                    print(f"  SYNC ok nach {count} Versuchen (Antwort 0x{r2[0]:02X})")
                    return True
                continue
            if r[0] == ACK or r[0] == NACK:
                print(f"  SYNC ok nach {count} Versuchen (Antwort 0x{r[0]:02X})")
                return True
        return False

    def cmd(self, code: int) -> bool:
        """Sendet ein 2-Byte-Kommando und wartet auf ACK (0x79)."""
        self.s.reset_input_buffer()
        self.s.write(bytes([code, code ^ 0xFF]))
        time.sleep(0.02)
        ack = self.s.read(1)
        if not ack:
            return False
        if ack[0] != ACK:
            print(f"  CMD 0x{code:02X} NACK 0x{ack[0]:02X}", file=sys.stderr)
            return False
        return True

    def read_memory(self, addr: int, length: int) -> bytes:
        """Liest `length` Bytes ab `addr` via READ-Befehl (0x11)."""
        if length < 1 or length > 128:
            return b''

        # 1. READ-Command
        if not self.cmd(0x11):
            return b''

        # 2. Adresse (4 Bytes BE + 1 XOR)
        addr_b = addr.to_bytes(4, 'big')
        self.s.reset_input_buffer()
        self.s.write(addr_b + bytes([xor(addr_b)]))
        time.sleep(0.02)
        ack = self.s.read(1)
        if not ack or ack[0] != ACK:
            return b''

        # 3. Anzahl Bytes
        n = length - 1
        self.s.reset_input_buffer()
        self.s.write(bytes([n, n ^ 0xFF]))
        time.sleep(0.05)
        # Antwort: N Datenbytes + 1 XOR-Checksum
        data = self.s.read(length + 1)
        if not data or len(data) != length + 1:
            return b''
        payload = data[:length]
        checksum = data[length]
        if checksum != xor(payload):
            print(f"  XOR mismatch at 0x{addr:04X}", file=sys.stderr)
            return b''
        return payload


def main():
    print("=" * 64)
    print("STM8L BSL Flash-Reader (AN3155, stm8gal-kompatibel)")
    print("=" * 64)
    print(f"Port: {PORT} @ {BAUD} 8E1")
    print(f"Bereich: 0x{FLASH_START:04X} - 0x{FLASH_END:04X}")
    print(f"Dump: {DUMP_FILE}")
    print()
    print(">>> Wenn 'PRESS RESET NOW' erscheint: Reset-Knopf drücken <<<")
    print()
    print("Start in 3s...")
    time.sleep(3)

    bsl = BSL(PORT, BAUD)
    print("\n--- Phase 1: BSL-Sync ---")
    print(">>> PRESS RESET NOW <<<")
    if not bsl.sync(timeout_s=15):
        print("!!! Kein Sync !!!")
        bsl.close()
        sys.exit(1)
    print(">>> SYNC OK <<<")

    print("\n--- Phase 2: Flash lesen ---")
    total = FLASH_END - FLASH_START + 1
    print(f"Lese {total} Bytes in {total // CHUNK} Chunks à {CHUNK}...")

    flash = bytearray()
    addr = FLASH_START
    chunk_count = (total + CHUNK - 1) // CHUNK
    i = 0
    consecutive_errors = 0
    while addr <= FLASH_END and i < chunk_count:
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
            time.sleep(0.5)
            # Re-sync
            if not bsl.sync(timeout_s=3):
                print("!!! Re-Sync fehlgeschlagen !!!")
                break

    bsl.close()

    if len(flash) == total:
        with open(DUMP_FILE, 'wb') as f:
            f.write(flash)
        print(f"\n>>> DUMP ERFOLGREICH: {DUMP_FILE} ({len(flash)} bytes) <<<")
        print(f"Erste 32 Bytes: {flash[:32].hex(' ')}")
        print(f"Letzte 32 Bytes: {flash[-32:].hex(' ')}")
        # Reset-Vektor auswerten
        rst_vec = int.from_bytes(flash[0:4], 'little')  # STM8 ist little-endian
        print(f"Reset-Vektor @ 0x8000: 0x{rst_vec:08X}")
    else:
        print(f"\n!!! DUMP UNVOLLSTÄNDIG: nur {len(flash)}/{total} bytes !!!")
        if flash:
            partial = DUMP_FILE + '.partial'
            with open(partial, 'wb') as f:
                f.write(flash)
            print(f"Teil-Dump: {partial}")


if __name__ == '__main__':
    main()
