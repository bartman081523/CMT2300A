/*
 * main.cpp — STM8 SWIM Flash Tool for ESP32
 *
 * Reads/writes STM8L151G6 flash via SWIM (single-wire debug interface).
 *
 * Serial commands (115200 baud, send newline to execute):
 *
 *   R       Read full flash (32 KB) — binary dump to serial
 *   H       Read full flash — hex dump (human readable)
 *   I       Read chip info (option bytes, SWIM status)
 *   W       Write flash (NOT YET IMPLEMENTED — needs user confirmation)
 *   S       SWIM status check
 *
 * Wiring:
 *   GPIO 25 → SWIM (Dongle TP2)
 *   GPIO 26 → NRST (Dongle TP1)
 *   GND     → GND  (Dongle TP3)
 *
 * STM8L151G6 memory map:
 *   0x001000 - 0x0013FF   EEPROM (1 KB)
 *   0x004800 - 0x00487F   Option bytes (128 B)
 *   0x008000 - 0x00FFFF   Flash program (32 KB)
 */

#include <Arduino.h>
#include "stm8_swim.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define FLASH_START   0x008000UL
#define FLASH_SIZE    0x8000UL     // 32 KB for STM8L151G6
#define BLOCK_SIZE    255          // Max ROTF block size
#define EEPROM_START  0x001000UL
#define EEPROM_SIZE   0x0400UL     // 1 KB
#define OPTION_START  0x004800UL
#define OPTION_SIZE   128

static Stm8Swim swim;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read a range of memory in 255-byte blocks, output as raw binary.
// Format: 4-byte length (big-endian) + data + "OK\n"
static void dumpFlashBinary(uint32_t start, uint32_t size) {
    uint8_t buf[256];
    uint32_t remaining = size;
    uint32_t addr = start;

    // Send length header
    uint8_t lenBytes[4] = {
        (uint8_t)(size >> 24),
        (uint8_t)(size >> 16),
        (uint8_t)(size >> 8),
        (uint8_t)(size)
    };
    Serial.write(lenBytes, 4);

    unsigned long lastProgress = millis();

    while (remaining > 0) {
        uint16_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : (uint16_t)remaining;

        if (!swim.readBlock(addr, buf, chunk)) {
            Serial.print("\nERROR: readBlock failed at 0x");
            Serial.println(addr, HEX);
            return;
        }

        Serial.write(buf, chunk);
        addr += chunk;
        remaining -= chunk;

        // Progress every second
        if (millis() - lastProgress > 1000) {
            lastProgress = millis();
            uint32_t done = size - remaining;
            Serial.print("\nPROGRESS:");
            Serial.print(done);
            Serial.print("/");
            Serial.print(size);
            Serial.print(" (");
            Serial.print(done * 100 / size);
            Serial.println("%)");
        }
    }

    Serial.println("\nOK");
}

// Read a range and output as hex with address markers.
// Format:
//   ===HEX_START===
//   :008000:HHHHHHHH...
//   :008100:HHHHHHHH...
//   ===HEX_END:32768===
static void dumpFlashHex(uint32_t start, uint32_t size) {
    uint8_t buf[256];
    uint32_t remaining = size;
    uint32_t addr = start;
    const int BYTES_PER_LINE = 64;

    Serial.println("===HEX_START===");

    while (remaining > 0) {
        uint16_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : (uint16_t)remaining;

        if (!swim.readBlock(addr, buf, chunk)) {
            Serial.print("ERROR: readBlock failed at 0x");
            Serial.println(addr, HEX);
            return;
        }

        // Output in BYTES_PER_LINE-byte lines
        for (uint16_t i = 0; i < chunk; i += BYTES_PER_LINE) {
            uint16_t lineLen = min((uint16_t)BYTES_PER_LINE, (uint16_t)(chunk - i));
            Serial.print(":");
            Serial.print((addr + i), HEX);
            Serial.print(":");
            for (uint16_t j = 0; j < lineLen; j++) {
                if (buf[i + j] < 0x10) Serial.print('0');
                Serial.print(buf[i + j], HEX);
            }
            Serial.println();
        }

        addr += chunk;
        remaining -= chunk;
    }

    Serial.print("===HEX_END:");
    Serial.print(size);
    Serial.println("===");
}

// Read option bytes
static void dumpOptionBytes() {
    uint8_t buf[128];
    Serial.println("===OPTION_BYTES===");
    for (uint32_t addr = OPTION_START; addr < OPTION_START + OPTION_SIZE; addr += BLOCK_SIZE) {
        uint16_t chunk = min((uint16_t)BLOCK_SIZE, (uint16_t)(OPTION_START + OPTION_SIZE - addr));
        if (swim.readBlock(addr, buf, chunk)) {
            Serial.print(":");
            Serial.print(addr, HEX);
            Serial.print(":");
            for (uint16_t i = 0; i < chunk; i++) {
                if (buf[i] < 0x10) Serial.print('0');
                Serial.print(buf[i], HEX);
            }
            Serial.println();
        }
    }
    Serial.println("===OPTION_END===");
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void cmdRead() {
    Serial.println("Reading 32 KB flash (0x008000 - 0x00FFFF)...");
    unsigned long start = millis();
    dumpFlashBinary(FLASH_START, FLASH_SIZE);
    unsigned long elapsed = millis() - start;
    Serial.print("Done in ");
    Serial.print(elapsed / 1000.0, 1);
    Serial.println(" s");
}

static void cmdHex() {
    Serial.println("Reading 32 KB flash as hex...");
    unsigned long start = millis();
    dumpFlashHex(FLASH_START, FLASH_SIZE);
    unsigned long elapsed = millis() - start;
    Serial.print("Done in ");
    Serial.print(elapsed / 1000.0, 1);
    Serial.println(" s");
}

static void cmdInfo() {
    Serial.println("===CHIP_INFO===");

    // SWIM status
    uint8_t status = swim.readStatus();
    Serial.print("SWIM_CSR (0x7F80): 0x");
    Serial.println(status, HEX);

    // Read a few bytes from flash to verify
    uint8_t buf[16];
    Serial.println("Flash @ 0x8000 (first 16 bytes):");
    if (swim.readBlock(0x8000, buf, 16)) {
        Serial.print("  ");
        for (int i = 0; i < 16; i++) {
            if (buf[i] < 0x10) Serial.print('0');
            Serial.print(buf[i], HEX);
            Serial.print(' ');
        }
        Serial.println();
    }

    // Option bytes
    dumpOptionBytes();

    Serial.println("===INFO_END===");
}

static void cmdStatus() {
    if (swim.isConnected()) {
        uint8_t status = swim.readStatus();
        Serial.print("SWIM connected. CSR=0x");
        Serial.println(status, HEX);
    } else {
        Serial.println("SWIM not connected. Send 'C' to connect.");
    }
}

static void cmdConnect() {
    Serial.println("Connecting via SWIM...");
    if (swim.connect()) {
        Serial.println("OK - SWIM active, target halted.");
    } else {
        Serial.println("FAILED - check wiring and target power.");
    }
}

// BSL-Entry-Pulse: NRST LOW 100ms, dann release, SWIM bleibt HIGH.
// Nützlich wenn die Application per UART läuft aber BSL gebraucht wird.
static void cmdBslEntry() {
    Serial.println("BSL-Entry: NRST LOW 100ms, then release (SWIM=HIGH kept)");
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);   // NRST low
    delay(100);
    pinMode(26, INPUT_PULLUP);  // release NRST
    delay(10);
    Serial.println("NRST released. STM8 should be in BSL now.");
}

static void cmdWrite() {
    Serial.println("WRITE NOT YET IMPLEMENTED.");
    Serial.println("Flash writing is dangerous - confirm with user first.");
    Serial.println("Use the Python script 'swim_flash.py' for write operations.");
}

static void cmdDiagnostic() {
    Serial.println("=== PIN DIAGNOSTIC ===");

    // --- Test 1: Read SWIM and NRST as input with pullup ---
    pinMode(25, INPUT_PULLUP);
    delayMicroseconds(10);
    int swim_raw = digitalRead(25);
    Serial.print("SWIM (GPIO25) raw: ");
    Serial.println(swim_raw ? "HIGH" : "LOW");

    pinMode(26, INPUT_PULLUP);
    delayMicroseconds(10);
    int nrst_raw = digitalRead(26);
    Serial.print("NRST (GPIO26) raw: ");
    Serial.println(nrst_raw ? "HIGH" : "LOW");

    // --- Test 2: Drive NRST LOW, check SWIM ---
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);
    delay(10);
    pinMode(25, INPUT_PULLUP);
    delayMicroseconds(10);
    swim_raw = digitalRead(25);
    Serial.print("SWIM with NRST=LOW: ");
    Serial.println(swim_raw ? "HIGH" : "LOW");

    // --- Test 3: Drive NRST HIGH, check SWIM ---
    digitalWrite(26, HIGH);
    delay(10);
    pinMode(25, INPUT_PULLUP);
    delayMicroseconds(10);
    swim_raw = digitalRead(25);
    Serial.print("SWIM with NRST=HIGH: ");
    Serial.println(swim_raw ? "HIGH" : "LOW");

    // --- Test 4: Drive SWIM HIGH as OUTPUT, read back ---
    // Use direct register access (same as SWIM code)
    *(volatile uint32_t*)0x3FF4400C = (1U << 25);  // GPIO_OUT_W1TC: set LOW first
    *(volatile uint32_t*)0x3FF44024 = (1U << 25);  // GPIO_ENABLE_W1TS: enable output
    *(volatile uint32_t*)0x3FF44008 = (1U << 25);  // GPIO_OUT_W1TS: drive HIGH
    delayMicroseconds(10);
    uint32_t in_reg = (*(volatile uint32_t*)0x3FF4403C) & (1U << 25);
    Serial.print("SWIM driven HIGH (direct reg): IN_REG=");
    Serial.println(in_reg ? "HIGH" : "LOW");

    // --- Test 5: Drive SWIM LOW as OUTPUT, read back ---
    *(volatile uint32_t*)0x3FF4400C = (1U << 25);  // GPIO_OUT_W1TC: drive LOW
    delayMicroseconds(10);
    in_reg = (*(volatile uint32_t*)0x3FF4403C) & (1U << 25);
    Serial.print("SWIM driven LOW (direct reg):  IN_REG=");
    Serial.println(in_reg ? "HIGH" : "LOW");

    // --- Test 6: Check OUT register ---
    uint32_t out_reg = (*(volatile uint32_t*)0x3FF44004) & (1U << 25);
    uint32_t en_reg = (*(volatile uint32_t*)0x3FF44020) & (1U << 25);
    Serial.print("GPIO25 OUT reg: ");
    Serial.print(out_reg ? "HIGH" : "LOW");
    Serial.print(", ENABLE reg: ");
    Serial.println(en_reg ? "OUTPUT" : "INPUT");

    // --- Test 7: Drive SWIM HIGH with Arduino API, read back ---
    pinMode(25, OUTPUT);
    digitalWrite(25, HIGH);
    delayMicroseconds(10);
    swim_raw = digitalRead(25);
    Serial.print("SWIM HIGH (Arduino API): ");
    Serial.println(swim_raw ? "HIGH" : "LOW");

    digitalWrite(25, LOW);
    delayMicroseconds(10);
    swim_raw = digitalRead(25);
    Serial.print("SWIM LOW (Arduino API):  ");
    Serial.println(swim_raw ? "HIGH" : "LOW");

    // Restore GPIO state for SWIM
    swim.begin();
    Serial.println("=== DIAG END ===");
}

static void cmdHelp() {
    Serial.println("\n=== STM8 SWIM Flash Tool ===");
    Serial.println("Commands (send via serial @ 115200):");
    Serial.println("  C  - Connect to target via SWIM");
    Serial.println("  B  - BSL-Entry: NRST pulse (100ms) for bootloader via UART");
    Serial.println("  D  - Pin diagnostic (check wiring)");
    Serial.println("  R  - Read full flash (32 KB binary)");
    Serial.println("  H  - Read full flash (hex dump)");
    Serial.println("  I  - Chip info + option bytes");
    Serial.println("  S  - SWIM status");
    Serial.println("  W  - Write flash (disabled, use Python script)");
    Serial.println("  ?  - This help");
    Serial.println("=============================\n");
}

// ---------------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== STM8 SWIM Flash Tool ===");
    Serial.println("GPIO 25=SWIM, GPIO 26=NRST");

    swim.begin();
    Serial.println("GPIO initialized. Send 'C' to connect, '?' for help.");
}

void loop() {
    if (Serial.available()) {
        char cmd = Serial.read();
        // Consume any trailing whitespace/newline
        while (Serial.available() && Serial.peek() <= ' ') {
            Serial.read();
        }

        switch (cmd) {
            case 'C': case 'c': cmdConnect();    break;
            case 'B': case 'b': cmdBslEntry();   break;
            case 'D': case 'd': cmdDiagnostic(); break;
            case 'R': case 'r': cmdRead();       break;
            case 'H': case 'h': cmdHex();        break;
            case 'I': case 'i': cmdInfo();       break;
            case 'S': case 's': cmdStatus();     break;
            case 'W': case 'w': cmdWrite();      break;
            case '?':            cmdHelp();      break;
            default:
                Serial.print("Unknown: '");
                Serial.print(cmd);
                Serial.println("'. Send '?' for help.");
                break;
        }
    }
    delay(10);
}
