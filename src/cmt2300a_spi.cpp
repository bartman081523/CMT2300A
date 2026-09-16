/*
 * cmt2300a_spi.cpp
 *
 * Low-level SPI access and state-machine helpers for the CMT2300A.
 *
 * The chip uses a 4-wire SPI bus (SCK, SDIO, CSB, FCSB) where CSB selects
 * the register bank and FCSB selects the FIFO. Both are active-low and
 * must never be asserted at the same time. The protocol is:
 *
 *   1. Pull CSB low.
 *   2. Wait > 0.5 SCLK cycles (the chip samples CSB on a falling edge).
 *   3. Send one R/W bit (1 = read, 0 = write), then 7 address bits,
 *      MSB first.
 *   4. For reads the SDIO line must be released to output mode by the
 *      ESP32 BEFORE the falling edge of the 8th SCLK (Figure 9 in the
 *      datasheet). The Arduino SPI library handles this for us as long
 *      as we hand it the full transfer word up front.
 *   5. Transfer one byte per data byte, MSB first.
 *   6. Pull CSB high. Wait > 0.5 SCLK cycles before the next access.
 *
 * Max SCLK frequency is 5 MHz (Table 11). We use 4 MHz to leave margin
 * for the wiring.
 */

#include "cmt2300a.h"
#include "cmt2300a_pins.h"
#include <SPI.h>

CMT2300A::CMT2300A(uint8_t pinCsb, uint8_t pinFcsb, uint8_t pinInt1, uint8_t pinInt2)
    : _csb(pinCsb), _fcsb(pinFcsb), _int1(pinInt1), _int2(pinInt2),
      _spiSettings(4000000, MSBFIRST, SPI_MODE0),
      _inRx(false), _packetReady(false), _rxHandler(nullptr),
      _baseFreqHz(0), _currentFreqHz(0)
{
}

// ----- low-level register access ---------------------------------------

// Joined-SDIO wiring (CMT_PIN_MISO == CMT_PIN_MOSI): bit-bang the 4-wire
// bus instead of hardware SPI. The shared line needs a per-bit direction
// switch (drive out, then release to input and sample), which the SPI
// peripheral cannot express - on the ESP32-S3 the first hardware SPI
// transfer never completes and the boot hangs.
// CMT_SWSPI is defined in cmt2300a_pins.h so every driver TU sees it.

#ifdef CMT_SWSPI
void CMT2300A::swSpiInit() {
    pinMode(CMT_PIN_SCK, OUTPUT);
    digitalWrite(CMT_PIN_SCK, LOW);
    pinMode(CMT_PIN_MOSI, OUTPUT);
    digitalWrite(CMT_PIN_MOSI, LOW);
}

// One byte, MSB first. Drives each bit out, releases the SDIO line, then
// samples it while SCK is high. For pure writes the sampled value is the
// line's echo and is ignored by the caller.
uint8_t CMT2300A::swSpiTransferByte(uint8_t b) {
    uint8_t r = 0;
    for (int i = 7; i >= 0; i--) {
        pinMode(CMT_PIN_MOSI, OUTPUT);
        digitalWrite(CMT_PIN_MOSI, (b >> i) & 0x01);
        delayMicroseconds(1);
        digitalWrite(CMT_PIN_SCK, HIGH);
        delayMicroseconds(1);
        pinMode(CMT_PIN_MISO, INPUT);
        if (digitalRead(CMT_PIN_MISO)) r |= (1 << i);
        delayMicroseconds(1);
        digitalWrite(CMT_PIN_SCK, LOW);
        delayMicroseconds(1);
    }
    return r;
}

// FIFO-Read-Strategiezyklus (Diagnostik): 0 = per-byte FCSB (E49x), 1 = ein
// langer FCSB-Frame, 2 = per-byte mit invertierter Phase.
static uint8_t s_rmode = 0;
static uint8_t s_mode_next_val() { uint8_t m = s_rmode % 3; s_rmode++; return m; }

// Receive-only byte for FIFO reads (E49x_HalfSpiReceive): the SDIO line is
// released the whole time (chip drives it), we only clock SCK and sample.
// sampleFalling=true: startet mit SCK HIGH und taket low-high je Bit
// (invertierte Phase, Abfrage vor dem L-Puls).
uint8_t CMT2300A::swSpiReceiveByte(bool sampleFalling) {
    uint8_t r = 0;
    pinMode(CMT_PIN_MISO, INPUT_PULLUP);
    if (sampleFalling) digitalWrite(CMT_PIN_SCK, HIGH);
    for (int i = 7; i >= 0; i--) {
        if (sampleFalling) {
            delayMicroseconds(2);
            if (digitalRead(CMT_PIN_MISO)) r |= (1 << i);
            digitalWrite(CMT_PIN_SCK, LOW);   // fallende Flanke = Takt
            delayMicroseconds(5);
            digitalWrite(CMT_PIN_SCK, HIGH);
            delayMicroseconds(2);
        } else {
            digitalWrite(CMT_PIN_SCK, HIGH);
            // Runde 48: 5/2/5 -> 2/1/1 us (12 -> 4 us/Bit). Die 100-kbps-
            // Luft schreibt mit 10 us/B; mit 12 us/Bit + 110 us FCSB-Rahmung
            // kostete ein Drain ~210 us/B - 21x zu langsam, der FIFO lief
            // bei Byte 64 voll (der beobachtete PKT_DONE-Latch).
            delayMicroseconds(2);
            if (digitalRead(CMT_PIN_MISO)) r |= (1 << i);
            delayMicroseconds(1);
            digitalWrite(CMT_PIN_SCK, LOW);
            delayMicroseconds(1);
        }
    }
    return r;
}
#endif // CMT_SWSPI

void CMT2300A::writeReg(uint8_t addr, uint8_t val) {
#ifdef CMT_SWSPI
    csbLow();
    delayMicroseconds(5);
    swSpiTransferByte(addr & 0x7F);
    swSpiTransferByte(val);
    csbHigh();
    delayMicroseconds(5);
#else
    SPI.beginTransaction(_spiSettings);
    csbLow();
    // Address byte: R/W=0, addr<6:0>
    SPI.transfer(addr & 0x7F);
    SPI.transfer(val);
    csbHigh();
    SPI.endTransaction();
#endif
}

uint8_t CMT2300A::readReg(uint8_t addr) {
#ifdef CMT_SWSPI
    csbLow();
    delayMicroseconds(5);
    swSpiTransferByte(addr | 0x80);   // R/W=1
    uint8_t v = swSpiTransferByte(0x00);
    csbHigh();
    delayMicroseconds(5);
    return v;
#else
    SPI.beginTransaction(_spiSettings);
    csbLow();
    SPI.transfer(addr | 0x80);   // R/W=1
    uint8_t v = SPI.transfer(0x00);
    csbHigh();
    SPI.endTransaction();
    return v;
#endif
}

void CMT2300A::writeFifo(const uint8_t *data, uint8_t len) {
    if (!data || !len) return;
    // E49x_GoFIFO(FIFO_WRITE): 0x69 bit0 = FIFO access direction
    // (1 = SPI writes the FIFO), bit2 = FIFO_RX_TX_SEL (1 = TX half in
    // merged mode).  Without this the packet handler never sees any
    // data - chip TXs an unmodulated carrier (SDR-verified 2026-08-30).
    writeReg(CMT_REG_CTL1_IO_SEL, readReg(CMT_REG_CTL1_IO_SEL) | 0x05);
#ifdef CMT_SWSPI
    // EBYTE E49x_SetFIFO flow: EVERY byte gets its own FCSB frame with
    // generous settle delays - a single long FCSB frame does not work.
    for (uint8_t i = 0; i < len; i++) {
        fcsbLow();
        delayMicroseconds(20);
        swSpiTransferByte(data[i]);
        digitalWrite(CMT_PIN_SCK, LOW);
        delayMicroseconds(30);
        fcsbHigh();
        delayMicroseconds(60);
    }
#else
    SPI.beginTransaction(_spiSettings);
    fcsbLow();
    // Per datasheet 5.2.1: pull FCSB low 1 SCLK cycle, then raise,
    // wait > 2 us, then write. The library cannot easily reproduce
    // that without a slow path; we just satisfy the > 2 us gap by
    // leaving the chip select low - the chip interprets the next
    // SCLK as the first data clock. The "FCSB must be pulled high
    // for at least 4 us" rule applies between bytes, so we chunk.
    for (uint8_t i = 0; i < len; i++) {
        SPI.transfer(data[i]);
        if ((i & 0x0F) == 0x0F && i + 1 < len) {
            // give the FIFO a chance to absorb the burst
            fcsbHigh();
            delayMicroseconds(5);
            fcsbLow();
        }
    }
    fcsbHigh();
    SPI.endTransaction();
#endif
}

void CMT2300A::readFifo(uint8_t *data, uint8_t len) {
    if (!data || !len) return;
    // Lesestrategien-Zyklus (Diagnostik): V0 = E49x per-byte FCSB (Sample an
    // steigender Flanke), V1 = EIN langer FCSB-Frame über alle Bytes,
    // V2 = per-byte mit fallender Flanke (SCK idle HIGH). RX-Hälfte (bit2=0,
    // bit0=0) — der Roundtrip-Test im Heartbeat zeigt, welche Varianten die
    // Bytes liefern.
    writeReg(CMT_REG_CTL1_IO_SEL, readReg(CMT_REG_CTL1_IO_SEL) & ~0x05);
#ifdef CMT_SWSPI
    // Bewiesene Variante V0 (RTRIP-Test 2026-08-31, byte-exakt): pro Byte
    // ein FCSB-Rahmen, SDIO als INPUT_PULLUP (nie treiben), Sample an
    // steigender Flanke.
    pinMode(CMT_PIN_MISO, INPUT_PULLUP);
    for (uint8_t i = 0; i < len; i++) {
        fcsbLow();
        // Runde 50: Rahmung 10/10/30 -> 5/5/15 us (~82 -> ~62 us/B).
        // Runde 48 (10/10/30) brachte den Durchbruch (Peak 41 < 64),
        // aber die Tail-Bloecke failen noch in ~40 % der starken Frames
        // (single-bit im letzten CRC-Feld / Byte-Slip) - Kandidaten:
        // (a) Timing-Race am Rahmen-Ende, (b) Luft-Demod (AGC/CDR).
        // Diese Runde eliminiert (a); bleibt das Fail-Muster byte-identisch,
        // ist es Luft und die AGC/Gain-Register (0x24/0x25/0x26/0x30) dran.
        delayMicroseconds(5);
        data[i] = swSpiReceiveByte();
        digitalWrite(CMT_PIN_SCK, LOW);
        delayMicroseconds(5);
        fcsbHigh();
        delayMicroseconds(15);
    }
#else
    SPI.beginTransaction(_spiSettings);
    fcsbLow();
    for (uint8_t i = 0; i < len; i++) {
        data[i] = SPI.transfer(0x00);
    }
    fcsbHigh();
    SPI.endTransaction();
#endif
}

// ----- state machine ---------------------------------------------------

bool CMT2300A::softReset() {
    // EBYTE reference: write 0xFF to register 0x7F (the dedicated soft
    // reset register), then wait >= 20 ms.  After reset the chip
    // settles in STBY (state code 0x02 in 0x61<3:0>).
    // On the S3/dongle wiring the chip can take longer than 200 ms to
    // leave SLEEP after the reset, so poll for up to 1 s.
    writeReg(CMT_SOFT_RST_ALT_REG, CMT_SOFT_RST);
    delay(20);
    // The reset lands in SLEEP (0x01) on the dongle wiring, not directly
    // in STBY - begin()'s goStandby() performs the transition. Accept both
    // as proof that the reset executed.
    uint32_t start = millis();
    while (true) {
        uint8_t st = readReg(CMT_REG_CTL1_MODE_STA) & 0x0F;
        if (st == CMT_STATE_STBY || st == CMT_STATE_SLEEP) return true;
        if (millis() - start > 1000) return false;
    }
}

bool CMT2300A::goSleep() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_SLEEP);
    waitForState(0x01);
    return true;
}

bool CMT2300A::goStandby() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_STBY);
    waitForState(0x02);
    return true;
}

bool CMT2300A::goTfs() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_TFS);
    waitForState(0x03);   // FS state code (chip reports 0x3 after TFS)
    return true;
}

bool CMT2300A::goRfs() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_RFS);
    waitForState(0x05);   // FS/RX share state code 0x05
    return true;
}

bool CMT2300A::goRx() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_RX);
    waitForState(0x05);   // RX state code (EBYTE-verified)
    _inRx = true;
    return true;
}

bool CMT2300A::goTx() {
    writeReg(CMT_REG_CTL1_MODE, CMT_GO_TX);
    waitForState(0x06);   // TX state code (EBYTE-verified)
    _inRx = false;
    return true;
}

void CMT2300A::waitForState(uint8_t stateCode, uint32_t timeoutMs) {
    // The current state lives in CHIP_MODE_STA<3:0> at register 0x61
    // (EBYTE reference driver, ebyte_e49x.c::E49x_SetStby).  The
    // datasheet is vague on this register - some revisions list
    // 0x6B, but EBYTE has confirmed 0x61 against real silicon.  We
    // poll with a 100 us / 600 us cadence for up to timeoutMs.
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if ((readReg(CMT_REG_CTL1_MODE_STA) & 0x0F) == stateCode) return;
        delayMicroseconds(100);
    }
    // Fall through: timeout - just leave the chip where it is.
}

// ----- high-level configuration ----------------------------------------

bool CMT2300A::setChannel(uint8_t ch) {
    // FH_CHANNEL<7:0> at 0x1B; base frequency stays where RFPDK put it,
    // FH_OFFSET in 0x1A is the 2.5 kHz * 256 channel spacing.
    writeReg(CMT_REG_FREQ_CHANNEL, ch);
    return true;
}

// ---------------------------------------------------------------------------
// Arbitrary frequency tuning
// ---------------------------------------------------------------------------
//
// We pick the smallest VCO_BANK / DIVX_CODE combination that puts the
// requested carrier in range, then write N (integer) and K (20-bit
// fractional) into the eight frequency bank registers. This is the
// approach documented in the CMOSTEK AN199 frequency calculator
// (http://www.cmostek.com/download/AN199-CMT2300A-CMT2119B-CMT2219B
//  frequency calculation tool.pdf).
//
// Note: the chip is silicon-capable of 127-1020 MHz (Table 7). The
// matching network on the PCB narrows the *usable* range. There is no
// register bit that returns "is the antenna matched?", so we just
// program the requested value and let the user see the signal-strength
// results.

struct BandDesc {
    uint8_t  vcoBank;
    uint8_t  divxCode;
    uint32_t divider;       // 2, 4, 6, 8, 12
    uint32_t loMinHz;
    uint32_t loMaxHz;
};

// Band table reconstructed from EBYTE E49 reference profiles plus
// CMOSTEK AN199 (Cmt2300A/Cmt2119B/Cmt2219B frequency calculation
// tool).
//
// The CMT2300A uses a Fractional-N PLL with a post-VCO prescaler.
// The hardware does:
//
//     VCO = (N + K/2^20) * 26 MHz        // sigma-delta modulator
//     LO  = VCO / divider                // 1 of 7 prescaler steps
//
// The VCO is broadband and covers ~1.27 - 2.04 GHz.  Any LO in
// 127 - 1020 MHz can be generated by picking the highest divider
// that keeps the VCO inside its tuning range.  Higher divider = more
// phase-noise margin.
//
// The seven prescaler steps and the (VCO_BANK, DIVX_CODE) pair the
// chip uses for each were reverse-engineered from the EBYTE STM8
// demo's E49-900M20S and E49-400M20S register exports:
//
//     divider  DIVX_CODE   LO range (MHz)    EBYTE profile
//     --------  ---------   -------------    -------------
//        2      0b000       635 - 1020       E49-900M20S @ 850 MHz
//        3      0b001       423 -  680
//        4      0b011       318 -  510       E49-400M20S @ 410 MHz
//        6      0b101       212 -  340
//        8      0b010       159 -  255
//       12      0b110       106 -  170
//       16      0b100        79 -  128
//
// Adjacent bands overlap; pickBand() chooses the highest divider
// (best phase noise) for the requested LO.  Coverage of 127-1020
// MHz is continuous once the VCO range is this wide.
//
// VCO_BANK is always 0b000 except in the 850-1020 MHz range where
// EBYTE uses 0b100.  We do the same so a profile that the E49
// module was qualified for stays bit-identical.
static const BandDesc kBands[] = {
    // {vcoBank, divxCode, divider, loMin, loMax}
    { 0b100, 0b000,  2,   635000000UL, 1020000000UL },  // 635-1020 (E49-900M20S anchor)
    { 0b000, 0b001,  3,   423000000UL,  680000000UL },
    { 0b000, 0b011,  4,   318000000UL,  510000000UL },  // (E49-400M20S anchor)
    { 0b000, 0b101,  6,   212000000UL,  340000000UL },
    { 0b000, 0b010,  8,   159000000UL,  255000000UL },
    { 0b000, 0b110, 12,   127000000UL,  170000000UL },
    { 0b000, 0b100, 16,    80000000UL,  128000000UL },  // 80-128, partly below silicon min
};
static const size_t kNumBands = sizeof(kBands) / sizeof(kBands[0]);

bool CMT2300A::isFrequencySupported(uint32_t freqHz) {
    return freqHz >= kMinFreqHz && freqHz <= kMaxFreqHz;
}

uint32_t CMT2300A::roundToChipResolution(uint32_t freqHz) {
    // Per AN199, the PLL word is N.K = FREQ_LO * DIVIDER / 26 MHz,
    // and the fractional part K is 20 bits. Frequency resolution in
    // Hz is therefore 26 MHz / (DIVIDER * 2^20). The chip datasheet
    // advertises 25 Hz which corresponds to DIVIDER = 1 (synthetic);
    // with the real dividers the worst case is 12 * 2^20 / 26e6
    // = 0.45 Hz, but for stability we round to the nearest 1 Hz
    // (well below the channel filter bandwidth).
    if (freqHz < kMinFreqHz) return kMinFreqHz;
    if (freqHz > kMaxFreqHz) return kMaxFreqHz;
    return freqHz;
}

static const BandDesc *pickBand(uint32_t freqHz) {
    // Bands overlap; pick the highest divider that covers the target.
    // Higher divider means a lower VCO frequency, which is quieter
    // (better phase noise) for a given LO.
    const BandDesc *best = nullptr;
    for (size_t i = 0; i < kNumBands; i++) {
        if (freqHz >= kBands[i].loMinHz && freqHz <= kBands[i].loMaxHz) {
            if (!best || kBands[i].divider > best->divider) {
                best = &kBands[i];
            }
        }
    }
    return best;
}

bool CMT2300A::setFrequencyHz(uint32_t freqHz) {
    if (!isFrequencySupported(freqHz)) return false;
    const BandDesc *band = pickBand(freqHz);
    if (!band) return false;

    // RX uses FREQ_RX_N/K, TX uses FREQ_TX_N/K. We write both so a
    // subsequent goRx or goTx is correct without re-tuning.
    //
    // N.K = FREQ_LO * DIVIDER / 26 MHz
    // For RX, the datasheet applies an IF offset of 26 MHz / 92.
    // We use FREQ_LO = freqHz for TX and freqHz + 26e6/92 for RX.

    auto program = [&](uint32_t loHz, bool isTx) {
        // Fixed-point with 20-bit fractional part.
        // N = floor(LO * DIVIDER / 26e6)
        // K = ((LO * DIVIDER) % 26e6) * 2^20 / 26e6, rounded.
        const uint64_t num   = (uint64_t)loHz * band->divider;
        const uint64_t n     = num / kXtalHz;
        const uint64_t rem   = num % kXtalHz;
        const uint64_t kVal  = (rem * (1ULL << kFractBits) + kXtalHz / 2) / kXtalHz;
        const uint32_t N     = (uint32_t)n;
        const uint32_t K     = (uint32_t)(kVal & 0xFFFFF);

        // Pack K into three bytes (low 8, mid 8, high 4 in upper nibble)
        const uint8_t kLow  = (uint8_t)(K & 0xFF);
        const uint8_t kMid  = (uint8_t)((K >> 8) & 0xFF);
        const uint8_t kHigh = (uint8_t)((K >> 16) & 0x0F);

        if (isTx) {
            // 0x18 = FREQ_TX_N
            // 0x19 = FREQ_TX_K[7:0]
            // 0x1A = FREQ_TX_K[15:8]
            // 0x1B = { VCO_BANK[2:0], FREQ_TX_K[19:16] }
            writeReg(0x18, (uint8_t)(N & 0xFF));
            writeReg(0x19, kLow);
            writeReg(0x1A, kMid);
            writeReg(0x1B, (uint8_t)((band->vcoBank << 5) | kHigh));
        } else {
            // 0x1C = FREQ_RX_N
            // 0x1D = FREQ_RX_K[7:0]
            // 0x1E = FREQ_RX_K[15:8]
            // 0x1F = { DIVX_CODE[2:0], FREQ_RX_K[19:16] }
            writeReg(0x1C, (uint8_t)(N & 0xFF));
            writeReg(0x1D, kLow);
            writeReg(0x1E, kMid);
            writeReg(0x1F, (uint8_t)((band->divxCode << 5) | kHigh));
        }
    };

    // Standby before touching the frequency bank; the chip must not
    // be in RFS/TFS/RX/TX when N/K is rewritten.
    const bool wasActive = (_inRx ||
        (readReg(CMT_REG_CTL1_MODE_STA) & 0x0F) != CMT_STATE_STBY);
    if (wasActive) {
        goStandby();
        delay(1);
    }

    // Word roles (RFPDK-map- + on-air-verified 2026-08-30): the map puts the
    // +IF word at 0x18..0x1B (vcoBank nibble) and the plain-carrier word at
    // 0x1C..0x1F (divx nibble).  Beacons TXed on the plain word (@0x1C) at
    // 868.95 MHz -> chip TX = 0x1C..0x1F, and RX needs LO = carrier + IF
    // (high-side) to land the signal at the 26 MHz/92 IF -> RX = 0x18..0x1B.
    // The driver had these swapped, which left the RX demod dead (signal at
    // IF=0; RSSI spikes but PREAM/SYNC never fired - SDR-verified).
    program(freqHz + (kXtalHz / 92), /*isTx=*/true);   // -> 0x18..0x1B (RX word)
    program(freqHz, /*isTx=*/false);                   // -> 0x1C..0x1F (TX word)

    // EBYTE: after writing N/K we must pulse bit 5 of 0x62 to make
    // the PLL re-lock.  Without this the chip carries the old
    // frequency on the next goRfs/goRx.
    {
        uint8_t ctl = readReg(CMT_REG_CTL1_MODE_CTL);
        writeReg(CMT_REG_CTL1_MODE_CTL, ctl | 0x20);
        writeReg(CMT_REG_CTL1_MODE_CTL, ctl & ~0x20);
    }

    _currentFreqHz = freqHz;
    return true;
}

CMT2300A::BandInfo CMT2300A::getBandInfo() {
    BandInfo info;
    info.vcoBank  = (readReg(0x1B) >> 5) & 0x07;
    info.divxCode = (readReg(0x1F) >> 5) & 0x07;
    info.baseFreqHz = _currentFreqHz;
    return info;
}

bool CMT2300A::setTxPower(int8_t dBm) {
    if (dBm < -20) dBm = -20;
    if (dBm >  20) dBm =  20;
    // TX power is 5 bits, 0 = -20 dBm, 40 = +20 dBm
    // mapped through register 0x59 (TX Power) and 0x5A
    // (Power Ramp). We only adjust the magnitude.
    uint8_t mag = (uint8_t)(dBm + 20);
    uint8_t txPowerReg = readReg(0x59) & 0xC0;
    writeReg(0x59, txPowerReg | (mag & 0x3F));
    return true;
}

bool CMT2300A::setModulation(CmtModulation mod) {
    // Modulation is split between Data Rate Bank (0x27) and TX Bank
    // (0x58) - the exact bit field is RFPDK-version dependent. We
    // assume the RFPDK has already configured a sensible default and
    // only tweak the modulation selector byte that appears in 0x27
    // (Table 21 shows MODE<2:0> at bits 4..6 of 0x27 for FSK/OOK/MSK).
    uint8_t r = readReg(0x27) & 0x8F;
    writeReg(0x27, r | ((mod & 0x07) << 4));
    return true;
}

int CMT2300A::readRssiDbm() {
    if (!_inRx) return -127;
    return (int8_t)readReg(CMT_REG_CTL2_RSSI_DBM);
}

// ----- TX / RX packet helpers (packet mode) ----------------------------

bool CMT2300A::txPacket(const uint8_t *data, uint8_t len, uint32_t timeoutMs) {
    if (len > 64) return false;   // FIFO limit
    goStandby();
    goTfs();
    // Clear TX FIFO (0x6C bit0, self-clearing - EBYTE E49x demo line 434)
    writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x01);   // 0x6C = 0x01
    delay(1);
    // Fill FIFO
    writeFifo(data, len);
    // Arm and go
    goTx();
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        // TX_DONE = bit 3 of 0x6A (EBYTE layout)
        if (readReg(CMT_REG_CTL2_INT_FLAG_LO) & CMT_IRQ_TX_DONE_LO) {
            clearIntFlagLo(CMT_IRQ_TX_DONE_LO);
            goStandby();
            return true;
        }
    }
    goStandby();
    return false;
}

CmtRxResult CMT2300A::rxPacket(uint8_t *buf, uint8_t bufLen, uint8_t *rxLen,
                               int *rssiDbm, uint32_t timeoutMs)
{
    *rxLen = 0;
    if (rssiDbm) *rssiDbm = -127;

    // E49x_GoReceive-Sequenz EXAKT: ClearIRQ → GoFIFO(FIFO_READ) →
    // ClearFIFO(READ) → SetReceive. Das GoFIFO(READ) (0x69 bit0=0, bit2=0)
    // VOR dem Empfang fehlte — 0x69 stand nach dem letzten writeFifo auf
    // WRITE-Modus, und der Paket-Handler schrieb dann NICHTS in den FIFO
    // (PKT_DONE mit leerem FIFO = die 00/FF-Frames!).
    writeReg(CMT_REG_CTL1_IO_SEL, readReg(CMT_REG_CTL1_IO_SEL) & ~0x05);  // GoFIFO READ
    writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);   // 0x6C = 0x02 RX-FIFO clear
    delay(1);

    goRfs();
    goRx();

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        // EBYTE layout: PKT_OK = bit 0 of 0x6D
        uint8_t hi = readReg(CMT_REG_CTL2_INT_FLAG_HI);
        if (hi & CMT_IRQ_PKT_OK_LO) {
            clearIntFlagHi(CMT_IRQ_PKT_OK_LO);
            // FIFO-Bytes per readFifo (Lesestrategie zykliert - der
            // RTRIP-Test im Heartbeat identifiziert die funktionierende
            // Variante; hier dieselbe Statik).
            uint8_t n = bufLen < 31 ? bufLen : 31;   // EXAKT die Frame-Länge (0x46=0x1F) — überlesen verschiebt den Lesezeiger dauerhaft
            readFifo(buf, n);
            *rxLen = n;
            if (rssiDbm) *rssiDbm = readRssiDbm();
            goStandby();
            return CMT_RX_OK;
        }
        // CRC error: COL_ERR = bit 6 of 0x6D
        if (hi & 0x40) {
            clearIntFlagHi(0x40);
            goStandby();
            return CMT_RX_ERR_CRC;
        }
    }
    goStandby();
    return CMT_RX_TIMEOUT;
}

void CMT2300A::startRx() {
    writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);   // RX FIFO clear @ 0x6C bit1
    delay(1);
    goRfs();
    goRx();
    _inRx = true;
}

void CMT2300A::stopRx() {
    goStandby();
    _inRx = false;
}
