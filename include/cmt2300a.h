/*
 * cmt2300a.h
 *
 * HopeRF (CMOSTEK) CMT2300A sub-GHz transceiver driver.
 *
 * The chip is configured through 113 user registers (0x00 - 0x71) that
 * the manufacturer partitions into the following banks (Table 22):
 *
 *   0x00 - 0x0B   CMT Bank         (factory, do not touch)
 *   0x0C - 0x17   System Bank      (low-power / clock settings)
 *   0x18 - 0x1F   Frequency Bank   (base frequency)
 *   0x20 - 0x37   Data Rate Bank   (DR, Fdev, BW, modulation)
 *   0x38 - 0x54   Baseband Bank    (packet format, FIFO, codecs)
 *   0x55 - 0x5F   TX Bank          (TX power, deviation ramp)
 *   0x60 - 0x6A   Control Bank 1   (state machine, GPIO, INT)
 *   0x6B - 0x71   Control Bank 2   (FIFO control, RSSI read-back)
 *
 * The driver loads a register map exported from the CMOSTEK RFPDK tool
 * (see config.h) and only changes the few Control Bank fields that
 * depend on the chosen operating mode. Anything that does not change
 * per packet (frequencies, filters, codecs) is the RFPDK's job.
 *
 * The interrupt lines INT1 / INT2 share a set of internal sources
 * selected by INT1_SEL<4:0> (Table 18) and routed to a chosen GPIO via
 * GPIO1_SEL / GPIO2_SEL / GPIO3_SEL.
 *
 * Reference: CMT2300A datasheet Rev 1.7 (Jul 2023).
 */

#ifndef CMT2300A_H
#define CMT2300A_H

#include <Arduino.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// Register addresses (Table 21 in the datasheet)
// ---------------------------------------------------------------------------

// CMT Bank - read-only factory calibration
#define CMT_REG_CMT_CAL           0x00
#define CMT_REG_CMT_MODE          0x01
#define CMT_REG_CMT_FREQ          0x02
// System Bank
#define CMT_REG_SYSTEM_HSI        0x0C
#define CMT_REG_SYSTEM_FREQ       0x0D
#define CMT_REG_SYSTEM_RX         0x0E
#define CMT_REG_SYSTEM_TX         0x0F
#define CMT_REG_SYSTEM_XO         0x10
#define CMT_REG_SYSTEM_LP         0x11
#define CMT_REG_SYSTEM_RSSI       0x12
#define CMT_REG_SYSTEM_CAL        0x13
#define CMT_REG_SYSTEM_AFC        0x14
#define CMT_REG_SYSTEM_DUTY       0x15
#define CMT_REG_SYSTEM_TIMING     0x16
#define CMT_REG_SYSTEM_RXDUTY     0x17
// Frequency Bank
#define CMT_REG_FREQ_BASE_H       0x18
#define CMT_REG_FREQ_BASE_L       0x19
#define CMT_REG_FREQ_OFFSET       0x1A
#define CMT_REG_FREQ_CHANNEL      0x1B
// Data Rate Bank
#define CMT_REG_DR_BW             0x20
#define CMT_REG_DR_AFC            0x21
// ... 0x22-0x37 follow (see config.h)
// Baseband Bank 0x38-0x54
// TX Bank 0x55-0x5F
// Control Bank 1
#define CMT_REG_CTL1_MODE         0x60    // State machine (RST, go_*)
#define CMT_REG_CTL1_MODE_STA     0x61    // Current state <3:0>, EBYTE-confirmed
#define CMT_REG_CTL1_MODE_CTL    0x62    // Misc control (bit 5 = PLL_FREQ_RELOCK)
#define CMT_REG_CTL1_INT_EN       0x64
#define CMT_REG_CTL1_INT_CLR      0x65
#define CMT_REG_CTL1_GPIO1_SEL    0x66
#define CMT_REG_CTL1_GPIO2_SEL    0x67
#define CMT_REG_CTL1_GPIO3_SEL    0x68
#define CMT_REG_CTL1_IO_SEL       0x69
#define CMT_REG_CTL1_CUS_INT      0x6A
// Control Bank 2
// Per the EBYTE e49x reference driver, the chip's interrupt status is
// split between two registers we have to read and OR together:
//   0x6A  low IRQ flags  (TX_DONE=bit3, RX_TMO=bit4, SL_TMO=bit5)
//   0x6D  high IRQ flags (PKT_OK=bit0, CRC_OK=bit1, NODE_OK=bit2,
//                         SYNC_OK=bit3, PREAM_OK=bit4, PKT_ERR=bit5,
//                         COL_ERR=bit6, LBD=bit7)
#define CMT_REG_CTL2_INT_FLAG_LO  0x6A    // IRQ low  (TX done, timeouts)
#define CMT_REG_CTL2_INT_FLAG_HI  0x6D    // IRQ high (PKT_OK, CRC_OK, ...)
#define CMT_REG_CTL2_INT_FLAG     0x6E    // Combined read (E49x_GetIRQ)
#define CMT_REG_CTL2_FIFO_FLAG    0x6C
#define CMT_REG_CTL2_RSSI_CODE    0x6E
#define CMT_REG_CTL2_RSSI_DBM     0x6F
#define CMT_REG_CTL2_LBD_VALUE    0x6F
#define CMT_REG_CTL2_FIFO         0x70    // FIFO read / write
#define CMT_REG_CTL2_PKT          0x71    // packet config read/write

// IRQ bit positions in the EBYTE-confirmed 0x6A/0x6D layout
#define CMT_IRQ_PKT_OK_LO         0x01    // in 0x6D: bit 0
#define CMT_IRQ_CRC_OK_LO         0x02    // in 0x6D: bit 1
#define CMT_IRQ_TX_DONE_LO        0x08    // in 0x6A: bit 3
#define CMT_IRQ_PKT_DONE_LO       0x40    // in 0x6D: bit 6 = COL/PKT error mask
#define CMT_IRQ_PREAM_OK_LO       0x10    // in 0x6D: bit 4

// IRQ clear registers (1 to clear)
#define CMT_REG_CTL1_INT_CLR_LO   0x6A    // write 1 to clear bit
#define CMT_REG_CTL1_INT_CLR_HI   0x6B    // write 1 to clear bit

// ---------------------------------------------------------------------------
// State commands (CHIP_MODE_SWT<7:0> at 0x60 - confirmed against EBYTE
// E49-900M20S reference driver ebyte_e49x.c).
//
// Note: the CMT2300A datasheet (Table 16) lists the values as a packed
// command code that must be written to 0x60.  EBYTE uses the same register
// but with the following bit-patterns.  We use the EBYTE numbers
// because they have been verified to actually drive the chip.
// ---------------------------------------------------------------------------
#define CMT_GO_SLEEP              0x10
#define CMT_GO_STBY               0x02
#define CMT_GO_TFS                0x04
#define CMT_GO_RFS                0x08
#define CMT_GO_TX                 0x40
#define CMT_GO_RX                 0x08
#define CMT_SOFT_RST              0xFF
// Some sources (and the EBYTE driver) also use register 0x7F for the
// reset command.  Writing 0xFF there is equivalent.
#define CMT_SOFT_RST_ALT_REG      0x7F

// ---------------------------------------------------------------------------
// State readback (0x61<3:0>, EBYTE confirmed).
// ---------------------------------------------------------------------------
#define CMT_STATE_SLEEP           0x01
#define CMT_STATE_STBY            0x02
#define CMT_STATE_TFS             0x04
#define CMT_STATE_RFS             0x05
#define CMT_STATE_RX              0x05
#define CMT_STATE_TX              0x06

// ---------------------------------------------------------------------------
// Interrupt sources (Table 18) - written to INT1_SEL / INT2_SEL<4:0>
// ---------------------------------------------------------------------------
enum CmtIntSource {
    CMT_INT_RX_ACTIVE   = 0,
    CMT_INT_TX_ACTIVE   = 1,
    CMT_INT_RSSI_VLD    = 2,
    CMT_INT_PREAM_OK    = 3,
    CMT_INT_SYNC_OK     = 4,
    CMT_INT_NODE_OK     = 5,
    CMT_INT_CRC_OK      = 6,
    CMT_INT_PKT_OK      = 7,
    CMT_INT_SL_TMO      = 8,
    CMT_INT_RX_TMO      = 9,
    CMT_INT_TX_DONE     = 10,
    CMT_INT_RX_FIFO_NMTY= 11,
    CMT_INT_RX_FIFO_TH  = 12,
    CMT_INT_RX_FIFO_FULL= 13,
    CMT_INT_RX_FIFO_WBYTE=14,
    CMT_INT_RX_FIFO_OVF = 15,
    CMT_INT_TX_FIFO_NMTY= 16,
    CMT_INT_TX_FIFO_TH  = 17,
    CMT_INT_TX_FIFO_FULL= 18,
    CMT_INT_STATE_IS_STBY= 19,
    CMT_INT_STATE_IS_FS = 20,
    CMT_INT_STATE_IS_RX = 21,
    CMT_INT_STATE_IS_TX = 22,
    CMT_INT_LBD         = 23,
    CMT_INT_TRX_ACTIVE  = 24,
    CMT_INT_PKT_DONE    = 25,
};

// ---------------------------------------------------------------------------
// GPIO routing (Table 17)
// ---------------------------------------------------------------------------
enum CmtGpioSel {
    CMT_GPIO1_DOUT      = 0x00,
    CMT_GPIO1_DIN       = 0x20,
    CMT_GPIO1_INT1      = 0x40,
    CMT_GPIO1_INT2      = 0x60,
    CMT_GPIO1_DCLK_TRXR = 0x80,
    CMT_GPIO1_RF_SWT    = 0xC0,
};

enum CmtModulation {
    CMT_MOD_OOK         = 0,
    CMT_MOD_FSK         = 1,
    CMT_MOD_GFSK        = 2,
    CMT_MOD_MSK         = 3,
    CMT_MOD_GMSK        = 4,
};

// ---------------------------------------------------------------------------
// Three frequency bands defined by the matching network (Table 7).
// The 868 / 915 board (E49-900MBL-01) uses the 760-1020 MHz band.
// ---------------------------------------------------------------------------
enum CmtBand {
    CMT_BAND_433  = 0,   // 380-510 MHz
    CMT_BAND_868  = 1,   // 760-1020 MHz, base 868 MHz
    CMT_BAND_915  = 2,   // 760-1020 MHz, base 915 MHz
};

// Packet result codes
enum CmtRxResult {
    CMT_RX_OK            = 0,
    CMT_RX_TIMEOUT       = 1,
    CMT_RX_ERR_CRC       = 2,
    CMT_RX_ERR_COLLISION = 3,
};

// ---------------------------------------------------------------------------
// Driver class
// ---------------------------------------------------------------------------
class CMT2300A {
public:
    CMT2300A(uint8_t pinCsb, uint8_t pinFcsb, uint8_t pinInt1 = 0xFF,
             uint8_t pinInt2 = 0xFF);

    // Initialise SPI bus, GPIO pins, perform the power-on sequence
    // (POR -> soft reset -> RFPDK register map -> STBY).
    // Returns true on success.
    bool begin(const uint8_t *rfpdkRegMap, size_t mapSize);

    // --- State machine ----------------------------------------------------
    bool goSleep();
    bool goStandby();
    bool goTfs();      // TX frequency synthesise
    bool goRfs();      // RX frequency synthesise
    bool goRx();
    bool goTx();
    bool softReset();

    // --- Frequency / channel ---------------------------------------------
    // The RFPDK config sets the base frequency. setChannel() can then
    // hop +/- 2.5 kHz * 256 = 640 kHz around that base via FH_OFFSET
    // (set in config.h) and FH_CHANNEL (0x1B).
    bool setChannel(uint8_t ch);

    // --- Arbitrary frequency tuning -------------------------------------
    // Sets the carrier frequency in Hz anywhere in the 127-1020 MHz
    // chip range (Table 7). The driver automatically picks the right
    // VCO_BANK and DIVX_CODE for the band, computes the integer (N)
    // and 20-bit fractional (K) PLL words from the 26 MHz reference
    // and writes registers 0x18 - 0x1F. Resolution is 26 MHz /
    // (DIVIDER * 2^20) which is about 25 Hz in the 868/915 band.
    //
    // Returns true if the frequency could be set. Outside-of-band
    // requests (i.e. outside 127-1020 MHz) return false and leave
    // the chip in its previous state. Note: the matching network on
    // the attached PCB defines a narrower usable range than the
    // silicon can reach.
    bool setFrequencyHz(uint32_t freqHz);

    // Reads the currently programmed carrier frequency in Hz. This
    // is the requested value, not the actual on-air frequency (the
    // crystal tolerance and AFC offset apply).
    uint32_t getFrequencyHz() const { return _currentFreqHz; }

    // Convenience: returns the chip's nearest supported frequency
    // to a requested value, after rounding to the PLL resolution.
    static uint32_t roundToChipResolution(uint32_t freqHz);

    // Returns true if the given frequency is supported by the silicon
    // (independent of board matching network).
    static bool isFrequencySupported(uint32_t freqHz);

    // Reads back the four PLL control registers (0x1B..0x1F) and
    // returns the current band / divider / channel selection.
    // Useful for diagnosing what is actually programmed.
    struct BandInfo {
        uint8_t  vcoBank;       // 0..7
        uint8_t  divxCode;      // 0..7
        uint32_t baseFreqHz;    // base = RFPDK value (not retuned)
    };
    BandInfo getBandInfo();

    // --- Power ------------------------------------------------------------
    // Power is signed in dBm, range -20 .. +20. Board hardware (matching
    // network) determines the actual achievable output.
    bool setTxPower(int8_t dBm);

    // --- Modulation / packet format --------------------------------------
    // Most of these are baked into the RFPDK map. We only expose a small
    // set of helper mutations.
    bool setModulation(CmtModulation mod);

    // --- FIFO -------------------------------------------------------------
    // Blocking TX of one packet. Returns true if it was sent.
    // Packet mode requires the RFPDK config to define preamble, sync,
    // length and CRC.
    bool txPacket(const uint8_t *data, uint8_t len, uint32_t timeoutMs = 2000);

    // Blocking RX of one packet. Writes payload to `buf` (max `bufLen`),
    // payload length to `*rxLen`, RSSI to `*rssiDbm`.
    CmtRxResult rxPacket(uint8_t *buf, uint8_t bufLen, uint8_t *rxLen,
                         int *rssiDbm, uint32_t timeoutMs = 2000);

    // --- Interrupt-driven RX ---------------------------------------------
    // Start continuous RX in the background. The packet handler fills
    // the 32-byte FIFO; call fetchPacket() to drain it.
    void startRx();
    void stopRx();

    // EE-FCSB-Scan: FCSB-Pin zur Laufzeit wechseln (Pin-Mapping-Scan)
    void setFcsbPin(uint8_t pin) { _fcsb = pin; }
    uint8_t getFcsbPin() const { return _fcsb; }

    // Low-level register access (also used by the WebUI to dump state).
    void writeReg(uint8_t addr, uint8_t val);
    uint8_t readReg(uint8_t addr);
    void writeFifo(const uint8_t *data, uint8_t len);
    void readFifo(uint8_t *data, uint8_t len);

    // Reads the RSSI in dBm. Requires the chip to be in RX.
    int readRssiDbm();

    // Direct access to interrupt flags. Useful for status queries.
    // Returns a combined 16-bit word: low byte = 0x6A, high byte = 0x6D.
    uint16_t readIntFlag() { return ((uint16_t)readReg(CMT_REG_CTL2_INT_FLAG_HI) << 8)
                                   |  readReg(CMT_REG_CTL2_INT_FLAG_LO); }
    uint8_t readFifoFlag() { return readReg(CMT_REG_CTL2_FIFO_FLAG); }

    // Clear one or more IRQ flags by writing 1-bits to the EBYTE
    // confirmed clear registers (0x6A for low, 0x6B for high).
    void clearIntFlagLo(uint8_t mask) { writeReg(CMT_REG_CTL1_INT_CLR_LO, mask); }
    void clearIntFlagHi(uint8_t mask) { writeReg(CMT_REG_CTL1_INT_CLR_HI, mask); }

    // GPIO-based IRQ handler support
    void attachRxPacketHandler(void (*handler)(const uint8_t *data, uint8_t len, int rssiDbm));
    bool isrPending() const { return _packetReady; }
    void clearIsr() { _packetReady = false; }

private:
    uint8_t _csb, _fcsb, _int1, _int2;
    uint8_t _lastReadMode = 0;   // Diagnostik: zuletzt genutzter FIFO-Read-Modus
    SPISettings _spiSettings;
    bool _inRx;
    volatile bool _packetReady;
    void (*_rxHandler)(const uint8_t *, uint8_t, int);

    // Frequency tracking. _baseFreqHz is the value the RFPDK wrote
    // to 0x18/0x19; _currentFreqHz is the actual carrier after the
    // last setFrequencyHz() call.
    uint32_t _baseFreqHz;
    uint32_t _currentFreqHz;

    static constexpr uint32_t kXtalHz      = 26000000UL;
    static constexpr uint32_t kMinFreqHz   = 127000000UL;  // Table 7
    static constexpr uint32_t kMaxFreqHz   = 1020000000UL;
    static constexpr uint8_t  kFractBits   = 20;

    // Helpers
    void csbLow()  { digitalWrite(_csb, LOW);  }
    void csbHigh() { digitalWrite(_csb, HIGH); }
    void fcsbLow()  { digitalWrite(_fcsb, LOW);  }
    void fcsbHigh() { digitalWrite(_fcsb, HIGH); }

    // Software-SPI for the joined-SDIO wiring (CMT_PIN_MISO == CMT_PIN_MOSI):
    // the E49 dongle routes the CMT2300A's bidirectional SDIO out as one
    // line, so the bus direction must switch per bit - hardware SPI cannot
    // do that and hangs. Timing mirrors the validated
    // scratches/esp32s3-dongle bit-bang implementation.
    void swSpiInit();
    uint8_t swSpiTransferByte(uint8_t b);
    uint8_t swSpiReceiveByte(bool sampleFalling = false);

public:
    // Diagnostik: zuletzt genutzter FIFO-Read-Modus (0/1/2-Variantenzyklus)
    uint8_t lastReadMode() const { return _lastReadMode; }

private:
    void waitForState(uint8_t stateCode, uint32_t timeoutMs = 1000);
    void interruptSetup();
    static void isrThunk(void *arg);
};

#endif // CMT2300A_H
