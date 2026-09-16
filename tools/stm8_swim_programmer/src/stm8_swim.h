/*
 * stm8_swim.h
 *
 * Minimal SWIM master for ESP32. Single-byte read/write only.
 * For a full read/write of a flash page you would normally use the
 * WOTF/ROTF 8-byte variants, but those are not implemented here -
 * this class is the lowest common denominator that lets us verify
 * the SWIM wiring is correct before we trust the chip with writes.
 */
#ifndef STM8_SWIM_H
#define STM8_SWIM_H

#include <Arduino.h>

class stm8_swim {
public:
    stm8_swim();

    // Configure GPIO and pull-ups. Call once from setup().
    bool begin();

    // Run the SWIM entry sequence. Returns true if the target
    // acknowledged (SWIM line driven high). The target is now halted
    // with SWIM active.
    bool connect();

    bool isConnected() const;

    // Issue a soft reset. The target re-enters execution; SWIM stays
    // active.
    bool softReset();

    // Read one byte at the 32-bit address. Returns 0xFF on error.
    uint8_t readByte(uint32_t addr);

    // Write one byte. Returns true on ACK.
    bool writeByte(uint32_t addr, uint8_t value);

    // Block variants (single-byte only in this minimal version).
    bool readBlock(uint32_t addr, uint8_t *buf, uint16_t len);
    bool writeBlock(uint32_t addr, const uint8_t *buf, uint16_t len);

    // Read the SWIM status register.
    uint8_t readStatus();

private:
    // Run the SWIM entry sequence. Returns true if the target
    // acknowledged (SWIM line driven high).
    bool entry();

private:
    bool _ok;
};

#endif // STM8_SWIM_H
