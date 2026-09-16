/*
 * cmt2300a_pins.h
 *
 * Default pin mapping for the ESP32 <-> CMT2300A wiring. Override any
 * of the macros from your platformio.ini build_flags (e.g.
 * -DCMT_PIN_SCK=18) to suit your board.
 */
#ifndef CMT2300A_PINS_H
#define CMT2300A_PINS_H

#ifndef CMT_PIN_SCK
#define CMT_PIN_SCK  18
#endif
#ifndef CMT_PIN_MISO
#define CMT_PIN_MISO 19
#endif
#ifndef CMT_PIN_MOSI
#define CMT_PIN_MOSI 23
#endif
#ifndef CMT_PIN_CSB
#define CMT_PIN_CSB  5
#endif
#ifndef CMT_PIN_FCSB
#define CMT_PIN_FCSB 17
#endif
#ifndef CMT_PIN_INT1
#define CMT_PIN_INT1 4
#endif
#ifndef CMT_PIN_INT2
#define CMT_PIN_INT2 16
#endif

// Joined-SDIO wiring (MISO == MOSI, e.g. the E49 dongle with its single
// bidirectional SDIO line): hardware SPI cannot switch the line direction
// per bit, so the driver bit-bangs the bus instead. Must be visible to
// every translation unit of the driver.
#if defined(CMT_PIN_MISO) && defined(CMT_PIN_MOSI) && (CMT_PIN_MISO == CMT_PIN_MOSI)
#define CMT_SWSPI 1
#endif

#endif // CMT2300A_PINS_H
