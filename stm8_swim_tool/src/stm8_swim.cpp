/*
 * stm8_swim.cpp — SWIM master for ESP32
 *
 * Ported from esp-stlink (rumpeltux). Original: ESP8266 @ 80 MHz.
 * This port: ESP32 @ 240 MHz (timing ×3).
 *
 * SWIM protocol per UM0470 / RM0016:
 *   1 bit = 22 HSI cycles = 1.375 µs @ 16 MHz HSI
 *   '1' =  2 cycles LOW + 20 HIGH
 *   '0' = 20 cycles LOW +  2 HIGH
 *
 * Frames (open-drain, MSB first):
 *   Command: 4 bits (3-bit opcode + 1 reserved), mask = BIT3
 *   Data:    9 bits (1 header + 8 data),     mask = BIT8
 *   Each frame: [N data bits] [parity] [ACK/NACK from target]
 *
 * All timing-critical sections run with interrupts disabled (RSIL r,15)
 * and use the CCOUNT cycle counter.
 *
 * NO Serial.print in timing-critical paths — UART IRQs break 8 MHz bit-rate.
 */

#include <Arduino.h>
#include <esp32/rom/ets_sys.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_struct.h>
#include <driver/gpio.h>
#include <cstring>
#include "stm8_swim.h"

// ---------------------------------------------------------------------------
// Pin configuration
// ---------------------------------------------------------------------------
#ifndef SWIM_PIN
#define SWIM_PIN     25
#endif
#ifndef SWIM_NRST_PIN
#define SWIM_NRST_PIN 26
#endif

#define SWIM_BIT   (1U << SWIM_PIN)
#define NRST_BIT   (1U << SWIM_NRST_PIN)

// ---------------------------------------------------------------------------
// ESP32 GPIO direct register access (TRM §4.10)
// ---------------------------------------------------------------------------
#define GPIO_OUT_W1TS    (0x3FF44008)
#define GPIO_OUT_W1TC    (0x3FF4400C)
#define GPIO_ENABLE_W1TS (0x3FF44024)
#define GPIO_ENABLE_W1TC (0x3FF44028)
// GPIO_IN_REG is already defined in soc/gpio_reg.h from the ESP32 SDK

#define SET_PIN_HIGH(pin)  (*(volatile uint32_t*)GPIO_OUT_W1TS = (pin))
#define SET_PIN_LOW(pin)   (*(volatile uint32_t*)GPIO_OUT_W1TC = (pin))
#define READ_PIN(pin)      ((*(volatile uint32_t*)GPIO_IN_REG) & (pin))

// ---------------------------------------------------------------------------
// GPIO helpers
// ---------------------------------------------------------------------------
static inline void pin_as_input_no_pull(uint8_t pin_num, uint32_t pin_mask) {
    *(volatile uint32_t*)GPIO_ENABLE_W1TC = pin_mask;
    volatile uint32_t *gpio_pin = (volatile uint32_t*)(0x3FF44000 + 0x04 + (pin_num * 4));
    *gpio_pin = (*gpio_pin & ~0x28) | 0x02;  // IE=1, PU=0, PD=0
}

static inline void pin_as_input_pullup(uint8_t pin_num, uint32_t pin_mask) {
    *(volatile uint32_t*)GPIO_ENABLE_W1TC = pin_mask;
    volatile uint32_t *gpio_pin = (volatile uint32_t*)(0x3FF44000 + 0x04 + (pin_num * 4));
    *gpio_pin = (*gpio_pin & ~0x0D) | 0x05;  // IE=1, PU=1, no PD
}

#define PIN_AS_INPUT(pin)  pin_as_input_pullup(SWIM_PIN, (pin))
#define PIN_AS_OUTPUT(pin) do { SET_PIN_HIGH(pin); *(volatile uint32_t*)GPIO_ENABLE_W1TS = (pin); } while (0)

// ---------------------------------------------------------------------------
// SWIM timing constants (cycles @ 240 MHz)
// ---------------------------------------------------------------------------
#define SHORT_PERIOD_LENGTH    81     // 27 * 3  (low/high phase of a '1' bit)
#define SWIM_CLOCK             30     // 10 * 3  (1 HSI cycle)
#define BIT_HALF_TIME          (9 * SWIM_CLOCK)
#define BIT_TOTAL_PERIOD_LENGTH (22 * SWIM_CLOCK)
#define MICROS_TO_CYCLES(x)    ((x) * 240)

// ---------------------------------------------------------------------------
// Xtensa helpers
// ---------------------------------------------------------------------------
static inline uint32_t get_ccount(void) {
    uint32_t ccount;
    __asm__ __volatile__("rsr %0,ccount" : "=a"(ccount));
    return ccount;
}

static inline void sync_ccount(uint32_t next) {
    while ((int32_t)(get_ccount() - (int32_t)next) < 0) ;
}

static inline uint32_t enter_critical(void) {
    uint32_t state;
    __asm__ __volatile__("rsil %0,15 ; esync" : "=a"(state));
    return state;
}

static inline void leave_critical(uint32_t state) {
    __asm__ __volatile__("wsr %0,ps ; esync" :: "a"(state) : "memory");
}

static const uint32_t TIMEOUT = 0x7FFFF;  // ~335 ms @ 240 MHz

// ===========================================================================
// LOW-LEVEL BIT FUNCTIONS
// ===========================================================================

static inline void finish_sync(uint32_t last) {
    sync_ccount(last + BIT_TOTAL_PERIOD_LENGTH - SHORT_PERIOD_LENGTH);
    SET_PIN_HIGH(SWIM_BIT);
}

static int read_bit(uint32_t *start) {
    uint32_t timeout = TIMEOUT;
    while (READ_PIN(SWIM_BIT) && --timeout) ;
    if (!timeout) return -1;  // READ_BIT_TIMEOUT
    *start = get_ccount();
    sync_ccount(*start + BIT_HALF_TIME);
    return READ_PIN(SWIM_BIT) ? 1 : 0;
}

static void write_bit_sync(uint32_t next, uint32_t prev_bit, uint32_t current_bit) {
    if (!prev_bit) {
        sync_ccount(next - SHORT_PERIOD_LENGTH);
        SET_PIN_HIGH(SWIM_BIT);
    }
    sync_ccount(next);
    SET_PIN_LOW(SWIM_BIT);
    if (current_bit) {
        sync_ccount(next + SHORT_PERIOD_LENGTH);
        SET_PIN_HIGH(SWIM_BIT);
    }
}

// Send bits MSB-first from mask down to bit 0, then parity, then read ACK.
// Returns 1 on ACK, 0 on NACK, negative on error.
static int write_byte(uint32_t *pnext, uint32_t data, uint32_t mask) {
    uint32_t parity = 0;
    uint32_t next = *pnext + 600;  // 200 * 3 = 600 cycles pre-roll
    while (mask) {
        uint32_t bit = data & mask;
        uint32_t prev_bit = (mask > 1) ? (data & (mask >> 1)) : 0;
        write_bit_sync(next, prev_bit, bit);
        mask >>= 1;
        parity ^= !!bit;
        next += BIT_TOTAL_PERIOD_LENGTH;
    }
    // parity bit
    write_bit_sync(next, 0, parity);
    finish_sync(next);
    PIN_AS_INPUT(SWIM_BIT);
    return read_bit(pnext);
}

static int read_byte(void) {
    uint32_t next;
    int status;
    if ((status = read_bit(&next)) != 1)
        return status < 0 ? status : -2;  // INVALID_TARGET_ID
    uint32_t result = 0;
    uint32_t parity = 0;
    for (uint32_t i = 0; i < 9; i++) {
        sync_ccount(next + 18 * SWIM_CLOCK);
        int bit = read_bit(&next);
        if (bit < 0) return -i - 20;
        result = result << 1 | (bit ? 1 : 0);
        parity ^= result;
    }
    next += BIT_TOTAL_PERIOD_LENGTH;
    PIN_AS_OUTPUT(SWIM_BIT);
    write_bit_sync(next, 0, !(parity & 1));
    finish_sync(next);
    PIN_AS_INPUT(SWIM_BIT);
    if (parity & 1) return -3;  // PARITY
    return (int)(result >> 1);
}

// Send a SWIM command + N data bytes.
// cmd: 3-bit opcode in bits 0-2, sent with mask=BIT3 (4 bits + parity)
// data bytes: sent with mask=BIT8 (9 bits + parity)
static int send_command(uint32_t cmd, size_t len, const uint8_t *data) {
    PIN_AS_OUTPUT(SWIM_BIT);
    uint32_t next = get_ccount() + 120;  // 40 * 3
    int status = write_byte(&next, cmd, BIT3);
    if (status == 1) {
        // ACK
    } else if (status == 0) {
        return -4;  // NACK
    } else {
        return status;  // error
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t now = get_ccount();
        next = (next + BIT_TOTAL_PERIOD_LENGTH > now + 120)
             ? next + BIT_TOTAL_PERIOD_LENGTH
             : now + 120;
        PIN_AS_OUTPUT(SWIM_BIT);
        int s = write_byte(&next, data[i], BIT8);
        if (s == 1) continue;
        if (s == 0) { i--; continue; }  // NACK, retry
        return s;
    }
    return 0;
}

static void generate_len_and_address_spec(uint8_t *dest, size_t len, uint32_t addr) {
    dest[0] = (uint8_t)len;
    dest[1] = (uint8_t)(addr >> 16);
    dest[2] = (uint8_t)(addr >> 8);
    dest[3] = (uint8_t)addr;
}

static int rotf(const uint8_t *len_and_address_spec, uint8_t *dest) {
    uint32_t state = enter_critical();
    int status = send_command(1, 4, len_and_address_spec);
    if (status < 0) { leave_critical(state); return status; }
    for (int i = 0; i < len_and_address_spec[0]; i++) {
        int result = read_byte();
        if (result < 0) { leave_critical(state); return result; }
        dest[i] = (uint8_t)result;
    }
    leave_critical(state);
    return 0;
}

static int wotf(const uint8_t *data) {
    uint32_t state = enter_critical();
    int result = send_command(2, 4 + data[0], data);
    leave_critical(state);
    return result;
}

// SRST command: drive SWIM low to start, then send SRST command with proper bus init
static int srst_cmd(void) {
    // Before sending, ensure bus is in known state
    PIN_AS_OUTPUT(SWIM_BIT);
    SET_PIN_HIGH(SWIM_BIT);
    delayMicroseconds(10);

    uint32_t state = enter_critical();
    int result = send_command(0, 0, NULL);
    leave_critical(state);
    return result;
}

// ===========================================================================
// SWIM ENTRY SEQUENCE
// ===========================================================================
// Per UM0470 §3.1 and original esp-stlink (rumpeltux):
//   1. Drive SWIM low for 16µs
//   2. 16 toggle pulses: 8 @ 1 kHz (500µs per half-cycle), 8 @ 2 kHz (250µs)
//   3. Release SWIM (input with pullup)
//   4. Wait for target sync (pull low, then release high)
//
// Returns:
//   >= 0: measured sync duration in CCOUNT cycles
//   -5: SYNC_TIMEOUT_1 (target never drove SWIM low)
//   -6: SYNC_TIMEOUT_2 (target never released SWIM high)
static int swim_entry_impl(void) {
    Serial.println("    [DBG] swim_entry: enter");

    // Check NRST state for debugging
    uint32_t nrst_in = (*(volatile uint32_t*)GPIO_IN_REG) & NRST_BIT;
    uint32_t nrst_out = (*(volatile uint32_t*)(0x3FF44004)) & NRST_BIT;
    uint32_t nrst_en = (*(volatile uint32_t*)(0x3FF44020)) & NRST_BIT;
    Serial.print("    [DBG] NRST: IN=");
    Serial.print(nrst_in ? "HIGH" : "LOW");
    Serial.print(" OUT=");
    Serial.print(nrst_out ? "HIGH" : "LOW");
    Serial.print(" EN=");
    Serial.println(nrst_en ? "OUT" : "IN");

    // Set SWIM as output, start with HIGH (matching original esp-stlink)
    SET_PIN_HIGH(SWIM_BIT);
    *(volatile uint32_t*)GPIO_ENABLE_W1TS = SWIM_BIT;
    uint32_t counter = get_ccount();

    // Initial 16µs LOW
    SET_PIN_LOW(SWIM_BIT);
    counter += 240 * 16;  // 16µs @ 240 MHz
    sync_ccount(counter);
    Serial.println("    [DBG] after 16us LOW");

    // 16 toggle pulses: 8 @ 1 kHz, 8 @ 2 kHz
    // Use 3x longer pulses for debugging — STM8L entry sequence is forgiving
    // Try 2x first
    for (int i = 0; i < 16; i++) {
        if (i & 1) SET_PIN_LOW(SWIM_BIT);
        else       SET_PIN_HIGH(SWIM_BIT);
        for (int j = 0; j < 100; j++) {  // 2x longer than original 50
            counter += (i < 8) ? MICROS_TO_CYCLES(10) : MICROS_TO_CYCLES(5);
            sync_ccount(counter);
        }
        uint32_t pin_now = (*(volatile uint32_t*)GPIO_IN_REG) & SWIM_BIT;
        Serial.print(pin_now ? "H" : "L");
    }
    Serial.println();

    // Critical section: release bus, wait for target sync
    uint32_t state = enter_critical();
    SET_PIN_HIGH(SWIM_BIT);
    // Switch to input with pullup (matching original esp-stlink PIN_AS_INPUT)
    *(volatile uint32_t*)GPIO_ENABLE_W1TC = SWIM_BIT;
    volatile uint32_t *gpio_pin = (volatile uint32_t*)(0x3FF44000 + 0x04 + (SWIM_PIN * 4));
    *gpio_pin = (*gpio_pin & ~0x0D) | 0x05;  // IE=1, PU=1, PD=0

    // Wait for target to pull SWIM LOW (sync response 1)
    // Original uses ~5µs timeout; we use 100µs to be safe
    uint32_t timeout = MICROS_TO_CYCLES(100);
    while (READ_PIN(SWIM_BIT) && --timeout) ;
    counter = get_ccount();
    int sync_status = 0;
    if (!timeout) {
        sync_status = -5;  // SYNC_TIMEOUT_1
    } else {
        // Wait for target to release SWIM HIGH (sync response 2)
        timeout = MICROS_TO_CYCLES(100);
        while (!READ_PIN(SWIM_BIT) && --timeout) ;
        int duration = (int)(get_ccount() - counter);
        if (!timeout) {
            sync_status = -6;  // SYNC_TIMEOUT_2
        } else {
            counter += duration + 72;  // >= 300 ns settle
            sync_status = duration;
        }
    }
    leave_critical(state);
    if (sync_status >= 0) {
        sync_ccount(counter);
    }

    Serial.print("    [DBG] swim_entry: status=");
    Serial.println(sync_status);
    return sync_status;
}

// ===========================================================================
// NRST CONTROL
// ===========================================================================
static void reset_pin(int on) {
    if (on == 0xFF) {
        // Release: input with pullup
        pin_as_input_pullup(SWIM_NRST_PIN, NRST_BIT);
    } else {
        if (on) SET_PIN_LOW(NRST_BIT);
        else    SET_PIN_HIGH(NRST_BIT);
        *(volatile uint32_t*)GPIO_ENABLE_W1TS = NRST_BIT;
    }
}

// ===========================================================================
// PUBLIC API
// ===========================================================================

Stm8Swim::Stm8Swim() : _ok(false) {}

bool Stm8Swim::begin() {
    PIN_AS_OUTPUT(NRST_BIT);
    SET_PIN_HIGH(NRST_BIT);
    PIN_AS_INPUT(SWIM_BIT);
    return true;
}

bool Stm8Swim::connect() {
    _ok = false;

    // 1. Hold target in reset
    reset_pin(1);
    delay(10);

    // 2. SWIM entry sequence
    int r = swim_entry_impl();
    if (r < 0) {
        Serial.print("SWIM entry failed: ");
        Serial.println(r);
        return false;
    }
    Serial.print("SWIM entry OK (sync=");
    Serial.print(r);
    Serial.println(" cycles)");
    delay(10);  // Let target stabilize after entry

    // 3. SRST command
    int sr = srst_cmd();
    if (sr < 0) {
        Serial.print("SRST failed: ");
        Serial.println(sr);
        return false;
    }
    delay(1);

    // 4. Write 0xA0 to SWIM_CSR (0x7F80) - activates SWIM module
    if (!writeByte(0x7F80, 0xA0)) {
        Serial.println("SWIM_CSR init failed");
        return false;
    }

    // 5. Release NRST
    reset_pin(0xFF);
    delay(10);

    _ok = true;
    Serial.println("SWIM connected.");
    return true;
}

bool Stm8Swim::isConnected() const { return _ok; }

uint8_t Stm8Swim::readByte(uint32_t addr) {
    if (!_ok) return 0xFF;
    uint8_t spec[4];
    generate_len_and_address_spec(spec, 1, addr);
    uint8_t buf[1];
    int r = rotf(spec, buf);
    if (r < 0) return 0xFF;
    return buf[0];
}

bool Stm8Swim::writeByte(uint32_t addr, uint8_t value) {
    if (!_ok) return false;
    uint8_t buf[5];
    generate_len_and_address_spec(buf, 1, addr);
    buf[4] = value;
    int r = wotf(buf);
    if (r < 0) return false;
    return true;
}

uint8_t Stm8Swim::readStatus() {
    return readByte(0x7F80);
}

bool Stm8Swim::softReset() {
    int r = srst_cmd();
    if (r < 0) return false;
    return true;
}

bool Stm8Swim::readBlock(uint32_t addr, uint8_t *out, uint16_t len) {
    if (!_ok || len == 0) return false;
    if (len > 255) len = 255;
    uint8_t spec[4];
    generate_len_and_address_spec(spec, len, addr);
    int r = rotf(spec, out);
    if (r < 0) return false;
    return true;
}

bool Stm8Swim::writeBlock(uint32_t addr, const uint8_t *data, uint16_t len) {
    if (!_ok || len == 0) return false;
    if (len > 255) len = 255;
    uint8_t buf[4 + 255];
    generate_len_and_address_spec(buf, len, addr);
    memcpy(&buf[4], data, len);
    int r = wotf(buf);
    if (r < 0) return false;
    return true;
}
