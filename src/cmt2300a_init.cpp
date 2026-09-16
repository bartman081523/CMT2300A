/*
 * cmt2300a_init.cpp
 *
 * Power-on reset, RFPDK register map load, and packet-mode configuration
 * for the CMT2300A.
 *
 *   - begin() drives the power-on timing from datasheet Figure 15
 *     (POR release <= 1 ms, XTAL up to settle <= 2.48 ms, block
 *     calibrations <= 6.5 ms).
 *   - The user supplies a register map produced by the CMOSTEK RFPDK
 *     tool. The map covers 0x00 - 0x5F. Control Bank 1 / 2 are
 *     re-asserted on every call to begin() to make sure the chip is
 *     in a known state.
 *   - For packet mode we wire up the three interrupt sources we care
 *     about: PKT_DONE on INT1, RX_FIFO_WBYTE as a pulse for fast
 *     draining, and TX_DONE for the TX helper.
 */

#include "cmt2300a.h"
#include "cmt2300a_pins.h"

bool CMT2300A::begin(const uint8_t *rfpdkRegMap, size_t mapSize) {
    if (!rfpdkRegMap || mapSize == 0) return false;

    // ----- GPIO --------------------------------------------------------
    pinMode(_csb, OUTPUT);
    pinMode(_fcsb, OUTPUT);
    digitalWrite(_csb, HIGH);
    digitalWrite(_fcsb, HIGH);

#ifdef CMT2300A_DEBUG
    Serial.println(F("CMT2300A: begin: GPIO"));
#endif
    if (_int1 != 0xFF) {
        pinMode(_int1, INPUT);
#ifdef CMT_USE_INT_RX
        // Only for interrupt-driven RX (startRx()/isrPending). The polled
        // rxPacket() path does not need it - and a floating/toggling INT1
        // line (e.g. dongle wiring with the chip unconfigured) would raise
        // an ISR storm that starves the whole system.
        attachInterruptArg(digitalPinToInterrupt(_int1), isrThunk, this, RISING);
#endif
    }
    if (_int2 != 0xFF) {
        pinMode(_int2, INPUT);
    }
#ifdef CMT2300A_DEBUG
    Serial.println(F("CMT2300A: begin: INT attached"));
#endif

    // ----- SPI ---------------------------------------------------------
#ifdef CMT_SWSPI
    swSpiInit();
#else
    SPI.begin(CMT_PIN_SCK, CMT_PIN_MISO, CMT_PIN_MOSI, -1);
#endif
#ifdef CMT2300A_DEBUG
    Serial.println(F("CMT2300A: begin: SPI up"));
#endif

    // ----- Power-on timing (Figure 15) --------------------------------
    delay(10);                     // generous POR delay

    // ----- Soft reset -------------------------------------------------
    if (!softReset()) {
#ifdef CMT2300A_DEBUG
        Serial.println(F("CMT2300A: soft reset failed"));
#endif
        return false;
    }
#ifdef CMT2300A_DEBUG
    Serial.println(F("CMT2300A: begin: soft reset OK"));
#endif

    // ----- Load RFPDK register map (0x00 - 0x5F) ----------------------
    // Skip CMT Bank (0x00-0x0B) which holds factory calibration that
    // must not be overwritten. The rest is fair game.
    for (size_t i = 0; i < mapSize; i++) {
        uint8_t addr = 0x0C + i;
        if (addr > 0x5F) break;
        writeReg(addr, rfpdkRegMap[i]);
    }

    // ----- Runde 64 (2026-09-06): RX-FIFO-Threshold senken -------------
    // Datasheet V1.7 Table 21 (S. 47): 0x54 = CUS_PKT29, Bit7 = FIFO_AUTO_
    // RES_EN, Bits[6:0] = FIFO_TH - EIN Threshold fuer RX und TX (Figures
    // 13/14: "FIFO_TH = 16"). Das RFPDK-Map liefert 16.
    // Grund: L=62-Frames (73 B on-FIFO) verlieren die letzten 9 Bytes, weil
    // PKT_DONE bei Byte 64 latcht (fixed-64-Payload) und der SW-Drain
    // (62 us/B vs. 10 us/B Luft) zu spaet Platz schafft - der Chip droppt
    // bei Level 64. TH=2 feuert den RX_FIFO_TH-Flag schon bei Level 3 ->
    // Drain startet frueher -> Peak ~62 < 64 (Python-Sim Log/r64).
    {
        uint8_t pkt29 = readReg(0x54);
        pkt29 = (pkt29 & 0x80) | 0x02;   // FIFO_TH[6:0] = 2
        writeReg(0x54, pkt29);
    }

    // ----- Control Bank 1 (0x60 - 0x69) -------------------------------
    // State command is written separately on every state transition.
    // Set up the INT routing and GPIO mapping here so the chip knows
    // what to do once we leave STBY.
    //
    // EBYTE reference (ebyte_e49x.c::E49x_Config):
    //   0x61 bit 4 = CONFIG_RETAIN   (keep CMT-bank regs through reset)
    //   0x61 bit 5 = ?  (EBYTE clears this bit)
    //   0x62 bit 5 = PLL_FREQ_RELOCK (set after every N/K change)
    //   0x69 bit 1 = FIFO_MERGE (two 32-byte FIFOs -> one 64-byte FIFO)
    //
    // INT routing per the E49x reference (ebyte_e49x.c), corrected 2026-08-30:
    //   0x65 = GPIO mapping, THREE 2-bit fields:
    //            GPIO1[1:0]: 0=DOUT 1=IRQ1 2=IRQ2 3=DCLK
    //            GPIO2[3:2]: 0=IRQ1 1=IRQ2 2=DOUT 3=DCLK
    //            GPIO3[5:4]: 0=CLKO 1=DOUT 2=IRQ2 3=DCLK
    //          (the old code wrote 0x66/0x67/0x68 as "GPIOx_SEL" - those are
    //           actually INT1 source / INT2 source / IRQ-enable register!)
    //   0x66 = INT1 source select <4:0>  (IRQ codes, e.g. PREAM_OK=0x03)
    //   0x67 = INT2 source select <4:0>  (IRQ_SYNC_OK=0x04)
    //   0x68 = IRQ ENABLE gate (E49x_GoIRQ!): bit0 PKT_DONE, bit1 CRC_OK,
    //          bit2 NODE_OK, bit3 SYNC_OK, bit4 PREAM_OK, bit5 TX_DONE,
    //          bit6 RX_TMO, bit7 SL_TMO.  WITHOUT these enables the
    //          packet-handler flags never latch - this was the RX-deaf root
    //          cause candidate: the old code wrote "INT_EN" to 0x64 instead.
    {
        uint8_t ctl = readReg(CMT_REG_CTL1_MODE_STA);
        ctl |= 0x10;        // CONFIG_RETAIN
        ctl &= ~0x20;       // clear the unknown bit
        writeReg(CMT_REG_CTL1_MODE_STA, ctl);
    }
    {
        uint8_t ctl = readReg(CMT_REG_CTL1_MODE_CTL);
        ctl |= 0x20;        // PLL_FREQ_RELOCK
        writeReg(CMT_REG_CTL1_MODE_CTL, ctl);
    }
    {
        uint8_t io = readReg(CMT_REG_CTL1_IO_SEL);
        io |= 0x02;         // FIFO_MERGE -> 64-byte single FIFO
        writeReg(CMT_REG_CTL1_IO_SEL, io);
    }
    writeReg(0x65, 0x05);                    // GPIO1 = IRQ1, GPIO2 = IRQ2
    writeReg(CMT_REG_CTL1_GPIO1_SEL, 0x03);  // INT1 src = PREAM_OK (diag tick)
    writeReg(CMT_REG_CTL1_GPIO2_SEL, 0x04);  // INT2 src = SYNC_OK
    writeReg(CMT_REG_CTL1_GPIO3_SEL, 0x1F);  // IRQ enables: PKT|CRC|NODE|SYNC|PREAM
    clearIntFlagHi(0xFF);                       // clear all 0x6B flags
    clearIntFlagLo(0xFF);                       // clear all 0x6A flags

    // ----- Control Bank 2 (0x6A - 0x71) -------------------------------
    // 0x6A and 0x6B are interrupt CLEAR registers (write-1-to-clear),
    // not status.  0x6C bits[1:0] are FIFO clear (bit0 = TX, bit1 = RX,
    // E49x_ClearFIFO) - the old "FIFO_TH = 16 bytes" comment here was a
    // misreading; the real FIFO threshold lives in 0x54[6:0] (see above).
    // Bit4 semantics unconfirmed; keep the historical 0x10 write since
    // TX/RX were validated with it.  0x6D is IRQ high status (read-only).
    writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x10);
    // 0x70 / 0x71 are read-only FIFO data / packet-config registers.
    // Leave them alone.

    // ----- Enter STBY and re-calibrate --------------------------------
    goStandby();
    delay(2);

    _inRx = false;
    _packetReady = false;
    _baseFreqHz = 0;
    _currentFreqHz = 0;

#ifdef CMT2300A_DEBUG
    Serial.println(F("CMT2300A: begin() complete"));
    Serial.print(F("  Chip mode = 0x"));
    Serial.println(readReg(CMT_REG_CTL1_MODE), HEX);
#endif
    return true;
}

// ISR stub - the actual work is done by the main loop after the flag
// is set, so we only touch a flag here.
void IRAM_ATTR CMT2300A::isrThunk(void *arg) {
    CMT2300A *self = (CMT2300A *)arg;
    self->_packetReady = true;
}

void CMT2300A::attachRxPacketHandler(void (*handler)(const uint8_t *, uint8_t, int)) {
    _rxHandler = handler;
}
