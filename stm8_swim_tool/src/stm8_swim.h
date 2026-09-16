/*
 * stm8_swim.h — Minimal SWIM master for ESP32
 *
 * Single-byte and block read/write for STM8L flash access.
 * Based on esp-stlink (rumpeltux), ported to ESP32.
 *
 * Wiring:
 *   GPIO 25 → SWIM (Dongle TP2 / P2 Pin 2)
 *   GPIO 26 → NRST (Dongle TP1 / P2 Pin 1)
 *   GND     → GND  (Dongle TP3 / P2 Pin 3)
 */
#ifndef STM8_SWIM_H
#define STM8_SWIM_H

#include <Arduino.h>

class Stm8Swim {
public:
    Stm8Swim();

    // Configure GPIO. Call once from setup().
    bool begin();

    // Full SWIM entry: NRST pulse + entry sequence + SRST + CSR init.
    // Returns true if the target acknowledged.
    bool connect();

    bool isConnected() const;

    // Soft reset (SRST command). Target re-enters execution; SWIM stays active.
    bool softReset();

    // Read one byte at 32-bit address. Returns 0xFF on error.
    uint8_t readByte(uint32_t addr);

    // Write one byte. Returns true on ACK.
    bool writeByte(uint32_t addr, uint8_t value);

    // Block read (max 255 bytes per call).
    bool readBlock(uint32_t addr, uint8_t *buf, uint16_t len);

    // Block write (max 255 bytes per call).
    bool writeBlock(uint32_t addr, const uint8_t *buf, uint16_t len);

    // Read SWIM status register (0x7F80).
    uint8_t readStatus();

private:
    bool _ok;
};

#endif // STM8_SWIM_H
