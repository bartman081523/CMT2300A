/*
 * main.cpp
 *
 * ESP32 + CMT2300A "arbitrary frequency TX/RX" demo.
 *
 *  - Default profile: 868 MHz, GFSK, 10 kbps, 10 kHz deviation.
 *  - Boots into WiFi AP mode (or STA if CMT_WIFI_SSID is defined).
 *  - Serves a small web page that lets you type a hex payload, pick a
 *    channel offset, and trigger a transmission; the same page shows
 *    the last packet received, RSSI and a hex dump.
 *  - The packet handler is polled from the loop(). Interrupt-driven
 *    reception can be enabled by wiring INT1 in hardware.
 *
 * Wiring (default in platformio.ini):
 *
 *   ESP32 GPIO  ->  CMT2300A
 *     18 SCK     ->  9  SCLK
 *     19 MISO    ->  10 SDIO
 *     23 MOSI    ->  10 SDIO
 *      5 CSB     ->  11 CSB   (active low)
 *     17 FCSB    ->  12 FCSB  (active low)
 *      4 INT1    ->  15 GPIO1 (or any INT-mapped GPIO)
 *     16 INT2    ->  8  GPIO3
 *
 *  The board must be powered from a clean 3V3 rail; the E49-900MBL-01
 *  draws 80 mA in TX at +20 dBm, so a linear regulator from USB is
 *  usually insufficient. Use a switching regulator or feed the
 *  board from a bench supply.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>        // Runde 79: rtlwmbus-Feed-Timestamps (localtime_r)
#include <sys/time.h>    // settimeofday (Boot-Anker fuer den Feed-ts)
#include "cmt2300a.h"
#include "cmt2300a_config.h"

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
#ifdef CMT_PIN_FCSB_ALT
// EE_4: The FCSB pin mapping (g5/PB1) is the only never-verified wire of the
// dongle header. Try the alternative pin (AUX, g17) as FCSB.
#undef CMT_PIN_FCSB
#define CMT_PIN_FCSB CMT_PIN_FCSB_ALT
#endif

// AP credentials. Leave CMT_WIFI_SSID empty to use AP mode with the
// default SSID.
#ifndef CMT_WIFI_SSID
#define CMT_WIFI_SSID  ""
#define CMT_WIFI_SSID_EMPTY 1   // Preprozessor-Marker: Fallback = leer
#endif
#ifndef CMT_WIFI_PASS
#define CMT_WIFI_PASS  ""
#endif

// OMS Wireless M-Bus T1 recording mode (868 MHz, GFSK, 32.8 kbps, 50 kHz
// dev, Manchester ON, sync 0x2DD4). Loads RFPDK_REG_MAP_T1 and runs a
// continuous RX logger instead of the demo TX/poll loop.
#ifndef CMT_OMS_T1
#define CMT_OMS_T1 0
#endif
// Passive pin sniffer: identify the dongle header pin functions by watching
// the STM8's own SPI traffic. Set -DCMT_SNIFF=1 in platformio.ini.
#ifndef CMT_SNIFF
#define CMT_SNIFF 0
#endif
// 1 = variable-length framing via the M-Bus L byte (0x45=0x05),
// 0 = fixed 32-byte payload fallback (0x45=0x00).
#ifndef CMT_T1_VARLEN
#define CMT_T1_VARLEN 1
#endif

// Default AP SSID
#define CMT_AP_SSID    "CMT2300A-Demo"
#define CMT_AP_PASS    "cmt2300a"

CMT2300A radio(CMT_PIN_CSB, CMT_PIN_FCSB, CMT_PIN_INT1, CMT_PIN_INT2);
WebServer server(80);

// Last received packet, kept for the web page
struct RxRecord {
    uint32_t seq;
    uint32_t ms;
    int rssi;
    uint8_t len;
    uint8_t data[64];
    bool crcOk;
};
static volatile uint32_t g_rxSeq = 0;
static RxRecord g_lastRx = {0, 0, -127, 0, {0}, false};
static portMUX_TYPE g_rxMux = portMUX_INITIALIZER_UNLOCKED;

#if CMT_OMS_T1
// Ring buffer of the last received T1 packets for the /status page.
#define RX_HIST_N 6
static RxRecord g_hist[RX_HIST_N];
static volatile uint32_t g_pktCount = 0;
static volatile uint32_t g_errCount  = 0;
// Runde 62 (2026-09-06): TX-Sequenzzaehler - das V-Feld des Beacons wird
// pro Versuch variiert, damit JEDES Telegramm einzigartig ist und der
// wmbusmeters-Dedup (identische Telegramme im Dedup-Fenster) nicht
// frisst -> MQTT-Receipt-Kadenz wird zum per-TX-RF-Orakel.
static volatile uint8_t g_beaconSeq = 0;
// RSSI min/max hold since the last heartbeat (raw 0x6F code).
static volatile int g_rssiMin = 999;
static volatile int g_rssiMax = -999;
#endif

// ----- HTML page --------------------------------------------------------

static const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html><head><title>CMT2300A demo</title>
<style>
body{font-family:system-ui;margin:24px;max-width:720px;background:#0d1117;color:#e6edf3}
h1{font-size:1.4em;margin:0 0 .4em}
fieldset{border:1px solid #30363d;border-radius:6px;padding:12px 16px;margin:12px 0}
input[type=text],input[type=number]{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:4px;padding:6px 8px;width:100%;box-sizing:border-box}
input[type=submit]{background:#238636;color:#fff;border:0;border-radius:4px;padding:8px 16px;cursor:pointer}
pre{background:#161b22;border:1px solid #30363d;border-radius:4px;padding:8px;overflow:auto}
label{display:block;margin:6px 0 2px;font-size:.9em;color:#8b949e}
.row{display:flex;gap:8px}
.row > *{flex:1}
</style></head>
<body>
<h1>CMT2300A - sub-GHz test bed</h1>
<form method="POST" action="/tx">
<fieldset><legend>Transmit</legend>
<label>Payload hex (max 64 byte, optional "0x" prefix)</label>
<input name="hex" value="DE AD BE EF">
<label>Channel offset (0..255, step 2.5 kHz x FH_OFFSET)</label>
<input name="ch" type="number" min="0" max="255" value="0">
<div class="row">
<div><label>Frequency (Hz, 127M..1020M, step 25)</label>
<input name="freq" type="number" min="127000000" max="1020000000" step="25" value="868000000"></div>
</div>
<label>TX power (dBm, -20..+20)</label>
<input name="pwr" type="number" min="-20" max="20" value="10">
<label>Channel offset (0..255, fast 2.5 kHz step)</label>
<input name="ch" type="number" min="0" max="255" value="0">
<input type="submit" value="Send packet">
</fieldset>
</form>
<form method="GET" action="/status">
<fieldset><legend>Status</legend>
<div id="status">click "Refresh" to poll</div>
<input type="submit" value="Refresh">
</fieldset>
</form>
</body></html>
)HTML";

static String renderStatus() {
    String s;
    s.reserve(2048);
#if CMT_OMS_T1
    s += "OMS T1 recorder (868 MHz, 32.8 kbps, Manchester)\n";
    s += "packets = " + String((unsigned long)g_pktCount) +
         ", errors = " + String((unsigned long)g_errCount) + "\n\n";
    s += "Last packets:\n";
    for (uint8_t i = 0; i < RX_HIST_N; i++) {
        const RxRecord &r = g_hist[i];
        if (r.ms == 0) continue;
        char line[16];
        snprintf(line, sizeof(line), "#%lu", (unsigned long)r.seq);
        s += "  " + String(line) + " " + String((millis() - r.ms) / 1000.0, 1) +
             "s ago " + String(r.rssi) + "dBm len=" + String(r.len) + " hex=";
        char buf[4];
        for (uint8_t j = 0; j < r.len && j < 32; j++) {
            snprintf(buf, sizeof(buf), "%02X", r.data[j]);
            s += buf;
        }
        if (r.len > 32) s += "..";
        s += "\n";
    }
    s += "\n";
#endif
    s += "Last RX:\n";
    if (g_lastRx.ms == 0) {
        s += "  (none)\n";
    } else {
        s += "  seq   = " + String(g_lastRx.seq) + "\n";
        s += "  age   = " + String((millis() - g_lastRx.ms) / 1000.0, 1) + " s\n";
        s += "  rssi  = " + String(g_lastRx.rssi) + " dBm\n";
        s += "  len   = " + String(g_lastRx.len) + "\n";
        s += "  hex   = ";
        char buf[8];
        for (uint8_t i = 0; i < g_lastRx.len; i++) {
            snprintf(buf, sizeof(buf), "%02X ", g_lastRx.data[i]);
            s += buf;
        }
        s += "\n";
    }
    s += "Chip state = 0x" + String(radio.readReg(CMT_REG_CTL1_MODE), HEX) + "\n";
    {
        uint16_t f = radio.readIntFlag();
        s += "INT flag   = 0x" + String((unsigned)f, HEX) + " (0x6A:0x6D)\n";
    }
    s += "FIFO flag  = 0x" + String(radio.readFifoFlag(), HEX) + "\n";
    s += "Frequency  = " + String((unsigned long)radio.getFrequencyHz()) + " Hz\n";
    auto band = radio.getBandInfo();
    s += "VCO bank   = 0x" + String(band.vcoBank, HEX) + "\n";
    s += "DIVX code  = 0x" + String(band.divxCode, HEX) + "\n";
    return s;
}

// ----- HTTP handlers ---------------------------------------------------

static void handleRoot() {
    server.send(200, "text/html", kIndexHtml);
}

static void handleStatus() {
    server.send(200, "text/plain", renderStatus());
}

static int hexToNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parseHex(const String &s, uint8_t *out, int maxLen) {
    int n = 0;
    int hi = -1;
    for (size_t i = 0; i < s.length() && n < maxLen; i++) {
        char c = s[i];
        if (c == ' ' || c == ',' || c == ':' || c == '-') continue;
        int v = hexToNibble(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

static void handleTx() {
    String hex   = server.arg("hex");
    uint32_t freq = server.arg("freq").toInt();
    int     ch   = server.arg("ch").toInt();
    int     pwr  = server.arg("pwr").toInt();

    uint8_t buf[64];
    int n = parseHex(hex, buf, sizeof(buf));

    radio.goStandby();
    bool okFreq = radio.setFrequencyHz(freq);
    radio.setTxPower((int8_t)pwr);
    radio.setChannel((uint8_t)ch);
    bool ok = radio.txPacket(buf, (uint8_t)n);

    String s = "TX ";
    s += ok ? "OK" : "TIMEOUT";
    s += " - " + String(n) + " bytes on " + String((unsigned long)freq) + " Hz";
    if (!okFreq) s += " (FREQUENCY OUT OF RANGE - not applied)";
    s += " at " + String(pwr) + " dBm\n";
    s += renderStatus();
    server.send(200, "text/plain", s);
}

// ----- wM-Bus Mode C/T decode helpers -----------------------------------
// EN 13757-4: everything after the sync word is 3-of-6 coded - each 4-bit
// nibble becomes a 6-bit symbol with exactly three ones. The CMT2300A
// delivers the raw (undecoded) bytes after sync; we decode here.

// Nibble -> 6-bit symbol table (EN 13757-4 / OMS Volume 2).
// Mock-Test-gefunden 2026-08-30: die alte Tabelle war eine FREMDE Auswahl
// (0x38/0x07/0x15/0x2A statt 0x0B/0x0D/0x0E/0x1C ... ) - falsch.
static const uint8_t k3of6[16] = {
    0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
    0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29
};

// wM-Bus CRC-16 (EN 13757): poly 0x3D65, init 0x0000, xorout 0xFFFF.
static uint16_t wmbusCrc(const uint8_t *d, uint8_t n) {
    uint16_t crc = 0;
    for (uint8_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x3D65) : (uint16_t)(crc << 1);
    }
    return crc ^ 0xFFFF;
}

#if defined(CMT_WMBUS_REPAIR) && CMT_WMBUS_REPAIR
// Runde 80 (2026-09-07): Block-CRC-Kette ab dem ersten FAIL-Block. tail =
// Frame-Bytes ab dem FAIL-Block (pos0), rem = restliche Datenbytes (EN-13757-
// Blockstruktur wie im CRC-Scan: je 16 Daten + 2 CRC, letzter Block kuerzer).
// Return 1 = alle Block-CRCs bis zum Frame-Ende passen. Bricht am ersten
// Fehlblock -> die Repair-Suche kostet pro Kandidat meist nur einen CRC.
static uint8_t cmtTailBlocksOk(const uint8_t *tail, uint16_t tlen, uint16_t rem)
{
    uint16_t pos = 0;
    while (rem) {
        uint8_t blk = rem > 16 ? 16 : (uint8_t)rem;
        if ((uint16_t)(pos + blk + 2) > tlen) return 0;
        uint16_t cb = wmbusCrc(tail + pos, blk);
        uint16_t rb = (uint16_t)(tail[pos + blk] << 8) | tail[pos + blk + 1];
        if (cb != rb) return 0;
        pos += blk + 2; rem -= blk;
    }
    return 1;
}
#endif

#if defined(CMT_SWFRAM) && CMT_SWFRAM
// ---- Cross-Burst-Assembly (Runde 31) ----
// Die Meter wiederholen das Telegramm 3x pro Burst. CRC-gepruefte Captures
// mit identischem Block1 (= gleiche Telegramm-Generation) werden byte-weis
// verschmolzen; Kontroverse Positionen (z.B. das wandernde XX an Telegramm-
// Byte 13) entscheidet ein CRC2-Bruteforce ueber die Kandidatenwerte.
// Nebenbefund: variiert XX ZWISCHEN Wiederholungen desselben Telegramms,
// ist es ein Demod-Bitfehler und nie echte Daten.
struct AsmCap {
    bool valid;
    uint32_t ms;
    uint8_t L, off;
    uint8_t buf[64];
    const char *var;
};
static AsmCap s_asm[8];
static uint8_t s_asmI = 0, s_asmN = 0;

static bool asmMerge(const uint8_t *t, const uint8_t *f, uint8_t L, uint8_t off,
                     const char *var, uint32_t now) {
    AsmCap &e = s_asm[s_asmI];
    s_asmI = (uint8_t)((s_asmI + 1) & 7);
    if (s_asmN < 8) s_asmN++;
    e.valid = true; e.ms = now; e.L = L; e.off = off; e.var = var;
    memcpy(e.buf, f, 64);

    // Gruppe: identischer Block1 (10 Bytes, je CRC1-geprueft) + gleiches L,
    // juenger als 10 min.
    AsmCap *grp[8]; uint8_t nG = 0;
    for (uint8_t k = 0; k < 8; k++) {
        AsmCap &a = s_asm[k];
        if (!a.valid || now - a.ms > 600000UL) continue;
        if (a.L != L || memcmp(a.buf + a.off, t, 10) != 0) continue;
        grp[nG++] = &a;
    }
    if (nG < 2) return false;

    const uint8_t TL = (uint8_t)(L + 5);   // Telegrammlaenge = Block1+Block2
    uint8_t merged[70], cand[70];
    uint8_t cval[70][4], ccnt[70][4], cN[70];
    uint8_t confPos[16], nConf = 0;
    uint16_t nCover = 0;
    memset(merged, 0, sizeof(merged));
    memset(cN, 0, sizeof(cN));
    for (uint8_t j = 0; j < TL; j++) {
        for (uint8_t k = 0; k < nG; k++) {
            const AsmCap &a = *grp[k];
            if ((uint16_t)(a.off + j) >= 64) continue;
            uint8_t b = a.buf[a.off + j];
            uint8_t u;
            for (u = 0; u < cN[j]; u++) if (cval[j][u] == b) break;
            if (u < cN[j]) ccnt[j][u]++;
            else if (cN[j] < 4) { cval[j][u] = b; ccnt[j][u] = 1; cN[j]++; }
            nCover++;
        }
        if (!cN[j]) continue;             // Position von keinem Capture erfasst
        uint8_t bi = 0;                   // Mehrheits-Kandidat
        for (uint8_t u = 1; u < cN[j]; u++) if (ccnt[j][u] > ccnt[j][bi]) bi = u;
        merged[j] = cval[j][bi];
        if (cN[j] > 1 && nConf < 16) confPos[nConf++] = j;
    }

    // Kompaktbericht: Konfliktstellen = Demod-Bitfehler-Landkarte
    Serial.printf("[ASM g=%u %s L=%u cov=%u/%u conf=%u", (unsigned)nG, var,
                  L, (unsigned)nCover, (unsigned)TL, (unsigned)nConf);
    for (uint8_t u = 0; u < nConf; u++) {
        uint8_t j = confPos[u];
        Serial.printf(" p%u=", j);
        for (uint8_t v = 0; v < cN[j]; v++)
            Serial.printf("%02X%s", cval[j][v], (v + 1 < cN[j]) ? "|" : "");
    }
    if (L >= 14 && cN[13])
        Serial.printf(" xx=%02X%s", cval[13][0], (cN[13] > 1) ? "VAR" : "cst");
    Serial.println();

    // CRC2-Bruteforce ueber die Konflikt-Kandidaten, beide L-Deutungen:
    // (a) Block2-Daten = L-9 (beacon-verifiziert), (b) = L-13.
    if (L < 14) return false;
    for (uint8_t fi = 0; fi < 2; fi++) {
        int16_t NB = (fi == 0) ? (int16_t)L - 9 : (int16_t)L - 13;
        if (NB < 1) break;
        if (!cN[12 + NB] || !cN[13 + NB]) continue;   // CRC2 nicht erfasst
        uint32_t prod = 1;
        bool fit = true;
        for (uint8_t u = 0; u < nConf; u++) {
            prod *= cN[confPos[u]];
            if (prod > 4096) { fit = false; break; }
        }
        if (!fit) continue;
        for (uint32_t combo = 0; combo < prod; combo++) {
            memcpy(cand, merged, sizeof(cand));
            uint32_t c = combo;
            for (uint8_t u = 0; u < nConf; u++) {
                uint8_t j = confPos[u];
                cand[j] = cval[j][c % cN[j]];
                c /= cN[j];
            }
            uint16_t cc = wmbusCrc(cand + 12, (uint8_t)NB);
            uint16_t rr = (uint16_t)(cand[12 + NB] << 8) | cand[13 + NB];
            if (cc == rr) {
                Serial.printf("[B2OK-ASM %s fix=%lu g=%u] L=%u:", (fi ? "b" : "a"),
                              (unsigned long)combo, (unsigned)nG, L);
                for (uint8_t j = 0; j < TL; j++) Serial.printf(" %02X", cand[j]);
                Serial.println();
                return true;
            }
        }
    }
    return false;
}
#endif  // CMT_SWFRAM

// Decode the 3-of-6 bit stream; returns data bytes written, -1 on an
// invalid symbol (framing lost).
static int wmbus3of6Decode(const uint8_t *air, uint8_t airLen, uint8_t *out, uint8_t outMax) {
    int outLen = 0;
    uint8_t hi = 0, haveHi = 0, sym = 0;
    int symBits = 0;
    for (uint8_t i = 0; i < airLen; i++) {
        for (int b = 7; b >= 0; b--) {
            sym = (sym << 1) | ((air[i] >> b) & 1);
            if (++symBits < 6) continue;
            symBits = 0;
            int idx = -1;
            for (int k = 0; k < 16; k++)
                if (k3of6[k] == sym) { idx = k; break; }
            if (idx < 0) return -1;
            if (haveHi) {
                if (outLen < outMax) out[outLen++] = (uint8_t)((hi << 4) | idx);
                haveHi = 0;
            } else {
                hi = (uint8_t)idx;
                haveHi = 1;
            }
        }
    }
    return outLen;
}

// ----- RX worker -------------------------------------------------------

// Mikro-Probe aus dem Treiber (rxPacket): FIFO-Zaehler + erste Bytes.
void cmtFifoProbe(uint8_t flags, uint8_t w70, uint8_t p71,
                  const uint8_t *head, uint8_t n, int rssi) {
    static uint32_t cnt = 0;
    if (++cnt % 5 != 1) return;    // nur jedes 5. Event drucken
    Serial.printf("[FIFO-PROBE] 0x6C=%02X 0x70=%02X 0x71=%02X "
                  "head=%02X %02X %02X %02X n=%u rssi=%d\n",
                  flags, w70, p71, head[0], head[1], head[2], head[3], n, rssi);
}

static void pollRx() {
#if CMT_OMS_T1
    // Continuous recording: re-arm RX immediately after every packet.
    // rxPacket() does FIFO clear -> goRfs -> goRx -> wait PKT_DONE ->
    // drain -> STBY, so the dead gap between reception windows is only
    // the state transition (a few ms). M-Bus T1 telegrams are periodic
    // (seconds apart), so no packet is lost to a polling throttle.
    uint8_t buf[64];
    uint8_t rxLen = 0;
    int rssi = -127;
    CmtRxResult r = radio.rxPacket(buf, sizeof(buf), &rxLen, &rssi, 150);
#if CMT_OMS_T1
    // Continuous RSSI sampling while the loop cycles RX windows: min/max
    // raw RSSI-code since the last heartbeat. Telegrams show up as spikes.
    int now = (int8_t)radio.readReg(CMT_REG_CTL2_RSSI_DBM);
    if (now < g_rssiMin) g_rssiMin = now;
    if (now > g_rssiMax) g_rssiMax = now;
#endif
    if (r == CMT_RX_OK) {
        portENTER_CRITICAL(&g_rxMux);
        g_rxSeq++;
        g_pktCount++;
        g_lastRx.seq = g_rxSeq;
        g_lastRx.ms = millis();
        g_lastRx.rssi = rssi;
        g_lastRx.len = rxLen;
        g_lastRx.crcOk = true;
        memcpy(g_lastRx.data, buf, rxLen);
        g_hist[g_rxSeq % RX_HIST_N] = g_lastRx;
        portEXIT_CRITICAL(&g_rxMux);

        // Mode C/T framing per rtl_433 m_bus.c: after the sync {0x54,0x3D}
        // the next byte 0x54 marks Mode C, followed by the format byte
        // (0xCD = A / 0x3D = B) and the RAW telegram. Mode T (anything
        // else) is 3-of-6 coded.
        const uint8_t *t = buf + 2;
        if (rxLen >= 14 && buf[0] == 0x54 && (buf[1] == 0xCD || buf[1] == 0x3D)) {
            uint16_t crc = wmbusCrc(t, 10);
            // wM-Bus CRC auf der Leitung ist BIG-endian (rtl_433-Quelltext +
            // Mock-Test 2026-08-30): erstes Byte = High-Byte.
            bool ok = (crc == (uint16_t)((t[10] << 8) | t[11]));
            uint32_t id = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                          ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
            Serial.printf("[wM-C %c] L=%u C=0x%02X M=%02X%02X ID=%08lu V=%02X T=%02X CRC=%s RSSI=%d raw=",
                          ok ? 'O' : 'X', t[0], t[1], t[3], t[2],
                          (unsigned long)id, t[8], t[9], ok ? "OK" : "BAD", rssi);
        } else {
            uint8_t dec[24];
            int decLen = wmbus3of6Decode(buf, rxLen, dec, sizeof(dec));
            if (decLen >= 12) {
                uint16_t crc = wmbusCrc(dec, 10);
                bool ok = (crc == (uint16_t)((dec[10] << 8) | dec[11]));
                uint32_t id = (uint32_t)dec[4] | ((uint32_t)dec[5] << 8) |
                              ((uint32_t)dec[6] << 16) | ((uint32_t)dec[7] << 24);
                Serial.printf("[wM-T %c] L=%u C=0x%02X M=%02X%02X ID=%08lu V=%02X T=%02X CRC=%s RSSI=%d raw=",
                              ok ? 'O' : 'X', dec[0], dec[1], dec[3], dec[2],
                              (unsigned long)id, dec[8], dec[9], ok ? "OK" : "BAD", rssi);
            } else {
                Serial.printf("[RX %lu] %u bytes (3of6 %s), %d dBm:", (unsigned long)g_rxSeq,
                              rxLen, decLen < 0 ? "invalid sym" : "short", rssi);
            }
        }
        for (uint8_t i = 0; i < rxLen; i++) Serial.printf(" %02X", buf[i]);
        Serial.println();
    } else if (r == CMT_RX_ERR_CRC || r == CMT_RX_ERR_COLLISION) {
        g_errCount++;
        Serial.printf("[RX-ERR %lu] code=%d\n", (unsigned long)g_errCount, (int)r);
    }
#else
    // Fire a single-packet RX every 200 ms. In a real app you would
    // use startRx() + INT1-driven FIFO draining; this is the smallest
    // reliable loop without tight timing requirements.
    //
    // EBYTE layout: PKT_OK = bit 0 of 0x6D.  readIntFlag() packs the
    // 0x6D byte into the high byte of the returned 16-bit word.
    if (radio.readIntFlag() & 0x0100) {   // PKT_DONE in 0x6D
        radio.clearIntFlagHi(0x01);
    }
    static uint32_t lastTry = 0;
    if (millis() - lastTry < 200) return;
    lastTry = millis();

    uint8_t buf[64];
    uint8_t rxLen = 0;
    int rssi = -127;
    CmtRxResult r = radio.rxPacket(buf, sizeof(buf), &rxLen, &rssi, 50);
    if (r == CMT_RX_OK) {
        portENTER_CRITICAL(&g_rxMux);
        g_rxSeq++;
        g_lastRx.seq = g_rxSeq;
        g_lastRx.ms = millis();
        g_lastRx.rssi = rssi;
        g_lastRx.len = rxLen;
        g_lastRx.crcOk = true;
        memcpy(g_lastRx.data, buf, rxLen);
        portEXIT_CRITICAL(&g_rxMux);
    }
#endif
}

// ----- Setup / loop ----------------------------------------------------

void setup() {
#if defined(CMT_WMBUS_FEED) && CMT_WMBUS_FEED
    // Runde 79b: TX-Ring vergroessern (vor begin() — Default 256 B). Ohne
    // Reader am Port fuellt der Rueckstau den Ring, Serial.write blockt,
    // der Main-Loop (und damit die RX-Engine) steht. 2048 B ≈ 12 s Puffer
    // bei ~170 B/s; zusammen mit dem guarded Feed-Write (drop statt block).
    Serial.setTxBufferSize(2048);
#endif
    Serial.begin(115200);
    delay(200);
#if defined(CMT_WMBUS_FEED) && CMT_WMBUS_FEED
    // Runde 79: der rtlwmbus-Feed braucht einen strptime-faehigen ts
    // (%Y-%m-%d %H:%M:%S). ESP32 ohne RTC/NTP startet bei 1970 — Anker
    // auf die Build-Basis 2026-09-07 00:00:00 UTC, dann laeuft die
    // Systemzeit mit der uptime weiter (wmbusmeters parst nur das
    // Format; NTP im STA-Modus wuerde spaeter korrigieren).
    {
        struct timeval tv = { 1788739200, 0 };   // 2026-09-07 00:00:00 UTC
        settimeofday(&tv, nullptr);
    }
#endif
#if defined(CMT_STM8_ONLY) && CMT_STM8_ONLY
    // Reiner STM8-Beobachter: ESP32 hält komplett die Finger vom SPI-Bus
    // (CSB/FCSB nur idle-HIGH), NRST losgelassen -> Stock-Firmware steuert
    // den CMT2300A. Wir loggen nur die UART.
    Serial.println(F("STM8-ONLY Modus: Stock-Firmware besitzt den Bus"));
    {
        const int hiZ[] = { CMT_PIN_CSB, CMT_PIN_FCSB, CMT_PIN_SCK,
                            CMT_PIN_MISO, CMT_PIN_MOSI };
        for (unsigned i = 0; i < sizeof(hiZ)/sizeof(hiZ[0]); i++)
            pinMode(hiZ[i], INPUT);
    }
    pinMode(CMT_PIN_NRST, OUTPUT);
    digitalWrite(CMT_PIN_NRST, HIGH);   // STM8-NRST definieren (nicht halten!)
    // UART-Beine: Header-7 "TXD" = Host->STM8 (ESP32-TX), Header-8 "RXD" =
    // STM8->Host (ESP32-RX) — Labels sind Host-seitig.
    Serial2.begin(9600, SERIAL_8N1, /*rx*/ CMT_PIN_INT2, /*tx*/ CMT_PIN_INT1);
    Serial.println(F("STM8 bridge listener on Serial2 (9600 8N1, rx=g16 tx=g15)"));
#else
    Serial.println();
    Serial.println(F("CMT2300A demo booting"));

#if defined(CMT_STM8_BRIDGE_PROBE) && CMT_STM8_BRIDGE_PROBE
    // STM8-UART-Brücke (Header 7/8): RX=g15 (STM8-TXD), TX=g16 (STM8-RXD).
    // EBYTE-AT-Firmware-Default: 9600 8N1.
    Serial2.begin(9600, SERIAL_8N1, /*rx*/ CMT_PIN_INT1, /*tx*/ CMT_PIN_INT2);
    Serial.println(F("STM8 bridge probe on Serial2 (9600 8N1)"));
#endif

    // WiFi
#if defined(CMT_WIFI_OFF) && CMT_WIFI_OFF
    // Diagnose: eigenes 2.4-GHz-Radio aus — trennt RSSI-Spikes des AP von
    // echten 868-MHz-Fleet-Telegrammen (Korrelation gegen rtl_433).
    WiFi.mode(WIFI_MODE_NULL);
    Serial.println(F("WiFi OFF (CMT_WIFI_OFF)"));
#elif defined(CMT_WIFI_SSID) && defined(CMT_WIFI_PASS) && !CMT_WIFI_SSID_EMPTY
    WiFi.begin(CMT_WIFI_SSID, CMT_WIFI_PASS);
    Serial.print(F("Connecting to "));
    Serial.println(CMT_WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("STA IP "));
        Serial.println(WiFi.localIP());
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(CMT_AP_SSID, CMT_AP_PASS);
        Serial.print(F("AP "));
        Serial.print(CMT_AP_SSID);
        Serial.print(F(" IP "));
        Serial.println(WiFi.softAPIP());
    }
#else
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CMT_AP_SSID, CMT_AP_PASS);
    Serial.print(F("AP "));
    Serial.print(CMT_AP_SSID);
    Serial.print(F(" IP "));
    Serial.println(WiFi.softAPIP());
#endif

    // Radio
#if CMT_SNIFF
    // ---- Passive sniffer: watch the header pins while the dongle's own
    // STM8 runs. Do NOT touch NRST - the STM8 must boot. -------------------
    Serial.println(F("SNIFF mode: passive monitoring, all pins as input"));
    Serial.println(F("Power dongle (J1) + STM8 (J2) now - I watch the wires."));
    const int sniffPins[] = {4, 5, 6, 7, 15, 16, 17, 8};
    const char *sniffNames[] = {"g4/PB0", "g5/PB1", "g6/PB2", "g7/PD1",
                                "g15/TXD", "g16/RXD", "g17/AUX", "g8/NRST"};
    const uint8_t N = sizeof(sniffPins) / sizeof(sniffPins[0]);
    for (uint8_t i = 0; i < N; i++) pinMode(sniffPins[i], INPUT);
    uint32_t counts[8] = {0};
    int prev[8];
    for (uint8_t i = 0; i < N; i++) prev[i] = digitalRead(sniffPins[i]);
    uint32_t lastReport = millis();
    uint32_t windowCounts[8] = {0};
    while (true) {
        for (uint8_t i = 0; i < N; i++) {
            int v = digitalRead(sniffPins[i]);
            if (v != prev[i]) {
                prev[i] = v;
                counts[i]++;
                windowCounts[i]++;
            }
        }
        if (millis() - lastReport > 5000) {
            lastReport = millis();
            Serial.print(F("SNIFF 5s: "));
            for (uint8_t i = 0; i < N; i++) {
                Serial.printf("%s=%lu/%lu ", sniffNames[i],
                              (unsigned long)windowCounts[i],
                              (unsigned long)counts[i]);
                windowCounts[i] = 0;
            }
            Serial.println();
        }
        yield();
    }
#endif
#ifdef CMT_PIN_NRST
#if defined(CMT_NRST_HOLD) && CMT_NRST_HOLD
    // E49 dongle: hold the STM8 bridge in reset so it releases the SPI bus.
    pinMode(CMT_PIN_NRST, OUTPUT);
    digitalWrite(CMT_PIN_NRST, LOW);
    Serial.println(F("NRST held LOW (dongle STM8 in reset)"));
#else
    // EE_7: pin 8 is used as FCSB here — it must NEVER be held LOW and
    // must idle HIGH (a LOW-hold on this line kills the whole SPI bus).
    pinMode(CMT_PIN_NRST, OUTPUT);
    digitalWrite(CMT_PIN_NRST, HIGH);
    Serial.println(F("EE_7: NRST/FCSB pin idle HIGH (no LOW-hold!)"));
#endif
#endif
#if CMT_OMS_T1
    // Wireless M-Bus Mode C/T, 868.95 MHz, 100 kbps, 3-of-6 (software).
    // On-air presence of these telegrams proven by RTL-SDR + rtl_433.
#if defined(CMT_RX_STOCK_TEST) && CMT_RX_STOCK_TEST
    // Stock-Profil-Test: EBYTE-Default-Map (2.4 kbps, Sync 0x2DD4) so wie
    // die Stock-Firmware — deren Frames streamen (Null-Batches). Kommt bei
    // uns dieselbe Kadenz raus, ist die RX-Pipeline funktional und der
    // Fehler sitzt im T100K-spezifischen Teil.
    const uint8_t *map = RFPDK_REG_MAP;
    Serial.println(F("Profile: Rx-STOCK-TEST (EBYTE 2.4 kbps map, register-genuine)"));
#else
    const uint8_t *map = RFPDK_REG_MAP_T100K;
    Serial.println(F("Profile: wM-Bus Mode C/T (868.95 MHz, GFSK, 100 kbps, 3of6 soft-decode)"));
#endif
#else
    const uint8_t *map = RFPDK_REG_MAP;
#endif
    Serial.println(F("radio.begin() ..."));
    bool beginOk = radio.begin(map, CMT2300A_REG_MAP_SIZE);
    Serial.println(beginOk ? F("radio.begin OK") : F("radio.begin FAILED"));
    if (!beginOk) {
        Serial.println(F("CMT2300A: init failed"));
        // Raw register probe: is the SPI bus alive at all?
        Serial.print(F("probe 0x61(state) 0x01(cmt) 0x6A 0x6D:"));
        Serial.printf(" %02X %02X %02X %02X\n",
                      radio.readReg(0x61), radio.readReg(0x01),
                      radio.readReg(0x6A), radio.readReg(0x6D));
        // continue anyway, web page will report empty state
    } else {
#if CMT_OMS_T1
        // T1 profile: modulation/deviation/sync are baked into the
        // RFPDK map - do not let setModulation() tweak the EBYTE
        // assumptions into it. Just make sure the PLL sits on 868 MHz.
        // The RFPDK map already carries the correct PLL words for 868.95 MHz
        // (VCO_BANK=1 @ 0x1F!). setFrequencyHz() would RE-COMPUTE them with
        // the driver's band table (VCO_BANK=0) and land on the wrong
        // frequency - so we do NOT retune here. Just pulse PLL relock.
        Serial.println(F("PLL relock pulse (map frequency 868.95 MHz) ..."));
        {
            uint8_t ctl = radio.readReg(CMT_REG_CTL1_MODE_CTL);
            radio.writeReg(CMT_REG_CTL1_MODE_CTL, ctl | 0x20);
            radio.writeReg(CMT_REG_CTL1_MODE_CTL, ctl & ~0x20);
        }
#if defined(CMT_TMODE_PROBE) && CMT_TMODE_PROBE
        // Runde 83g (2026-09-08): T1-DR-Bank @ 868.95 — die Bandplan-Prämisse
        // (T-Mode = 868.3) war FALSCH. Beweiskette: der Root-Daemon startet
        // rtl_wmbus (device=rtlwmbus, listento=any, /etc/wmbusmeters.conf);
        // rtl_wmbus.c:880 verlangt "rtl_sdr _MUST_ be set to 868.625MHz",
        // Zeile 1326 mappt iplus/qplus (+325 kHz -> 868.95M) auf den T1/C1-
        // Kanal und iminus/qminus (-325 kHz -> 868.3M) auf den S1-Kanal
        // (ACCESS_CODE_S1 = 0x547696). 868.3 ist in dieser Architektur der
        // S1-Kanal — deshalb blieben Soak#1..#5 (83..83f @ 868.3) trotz
        // qcaloric-TX im Journal leer. T1 teilt mit C1 Kanal UND Access-Code
        // 0x543D (rtl_wmbus ACCESS_CODE_T1_C1) -> SWFRAM-RX-Flow unveraendert
        // nutzbar: Sync feuert, VarLen liest 3of6-codierte Bytes als L,
        // PKT_DONE/TMO -> RAW-Dumps (sv.dump=1 @ agc20). setFrequencyHz
        // pulst den Relock intern.
        Serial.println(F("[TPROBE] retune 868.95 MHz (T1-DR-Bank) ..."));
        {
            bool okT = radio.setFrequencyHz(868950000UL);
            Serial.printf("[TPROBE] retune %s PLL 0x18..0x1F:", okT ? "OK" : "FAIL");
            for (uint8_t a = 0x18; a <= 0x1F; a++) Serial.printf(" %02X", radio.readReg(a));
            Serial.println();
        }
#endif
        // IRQ enables live at 0x68 (E49x_GoIRQ) - NOT at 0x64. Enable
        // PREAM/SYNC/NODE/CRC/PKT_DONE + TX_DONE for diagnostics.
        // IRQ enables live at 0x68 (E49x_GoIRQ) - NOT at 0x64. Enable
        // PREAM/SYNC/NODE/CRC/PKT_DONE + TX_DONE for diagnostics.
        radio.writeReg(0x68, 0x3F);
        // GAIN-TEST (0x24/0x25/0x26 = Demo-900M-Werte) ENTFERNT: auch diese
        // Bytes sind ratengekoppelt — der Mix killt alle Frames (0 in 4 min).
        // Der DR/Gain-Zugang braucht die RFPDK-Formeln (Ghidra-RE auf
        // ~/.rfpdk-wine/CMOSTEK RFPDK-V1.45.exe) oder Zwischenraten-Exports.
#if defined(CMT_DR_MIX) && CMT_DR_MIX
        {   // REGISTER-MIX (2026-09-01): Stock-DR-Bank + 100k-CDR. Test-
            // ERGEBNIS: 0 komplette Frames (die Stock-BW killt die 100k-CDR)
            // — nur zur Reaktivierung per -DCMT_DR_MIX=1, Standard = AUS.
            static const uint8_t drMix[24] = {
                0x41,0x62,0x27,0x16,0x41,0x6D,0x80,0x86,
                0x41,0x62,0x27,0x16,0x20,0x04,0x01,0x18,
                0x10,0x99,0xC1,0x9B,0x06,0x0A,0x9F,0x39
            };
            for (uint8_t a = 0; a < 24; a++)
                radio.writeReg(0x20 + a, pgm_read_byte(&drMix[a]));
            Serial.println(F("[DR-MIX] Stock-AGC/BW + CDR_BR_TH=0x0104 (100k)"));
        }
#endif
        {   // FIFO-Mechanik-Test: TX-Hälfte beschreiben (E49x-Flow,
            // luftverifiziert) und mit dem zyklischen Read zurücklesen.
            uint8_t wr[8] = {0xC1,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
            uint8_t rd[8] = {0};
            radio.writeFifo(wr, sizeof(wr));
            // auf Lesen der TX-Hälfte umschalten (bit2=1, bit0=0)
            radio.writeReg(0x69, (radio.readReg(0x69) | 0x04) & ~0x01);
            radio.readFifo(rd, sizeof(rd));
            Serial.printf("[FIFO-RTRIP V%d]", radio.lastReadMode());
            for (uint8_t i = 0; i < sizeof(rd); i++)
                Serial.printf(" %02X", rd[i]);
            Serial.println(F("  (erwartet: C1 23 45 67 89 AB CD EF)"));
        }
#if defined(CMT_RX_BISECT) && CMT_RX_BISECT
        // Bisektion Stock -> T100K, Schritt 1: NUR die Data-Rate-Bank
        // (0x20-0x37) der T100K-Karte ueber die Stock-Map legen + Sync 543D
        // + Preamble 0x55 + 868.95 MHz (bb1=RX+IF, bb2=TX). Ueberlebt die
        // RX-Kette das nicht, sitzt der Killer in diesen 24 Bytes.
        static const uint8_t drT100K[24] = {
            0x3F, 0xF0, 0x73, 0x10, 0x63, 0x12, 0x0F, 0x0A,
            0x9F, 0x6C, 0x29, 0x29, 0xC0, 0x04, 0x01, 0x53,
            0x20, 0x00, 0xB4, 0x00, 0x00, 0x01, 0x00, 0x00
        };
        for (uint8_t a = 0; a < 24; a++)
            radio.writeReg(0x20 + a, pgm_read_byte(&drT100K[a]));
        radio.writeReg(0x43, 0x3D);
        radio.writeReg(0x44, 0x54);
        radio.writeReg(0x3B, 0x55);
        // FIXED-Length-Framing (31 B): in VarLen liest der Chip das erste
        // Byte nach dem Sync als Länge — das ist im wM-Bus der Mode-C-
        // Marker 0x54 (84!) → er wartet auf 84 Bytes, das 65-Byte-Telegramm
        // endet vorher → PKT_DONE nie. Fixed-31 komplettiert JEDES Telegramm
        // nach 31 Bytes: FIFO = [54 CD 3E 44 93 44 <id> …] - parserfertig.
        radio.writeReg(0x45, 0x00);   // Length-Type = fixed
        radio.writeReg(0x46, 0x1F);   // Payload = 31 Bytes
        // FIFO_MERGE AUS (0x69 bit1=0): separate 32-Byte-FIFOs. Die
        // Paket-Handler-Schreibungen landen dann garantiert in der
        // RX-Hälfte — bei Merge war der RX-FIFO sichtbar LEER.
        radio.writeReg(0x69, radio.readReg(0x69) & ~0x02);
        radio.goStandby();
        radio.setFrequencyHz(868950000UL);
        Serial.println(F("[BISECT] Stock-Map + T100K-DR-Bank + sync543D/pre55 + 868.95"));
        Serial.print(F("[BISECT] PLL:"));
        for (uint8_t a = 0x18; a <= 0x1F; a++) Serial.printf(" %02X", radio.readReg(a));
        Serial.println();
#endif
#if !(defined(CMT_RX_STOCK_TEST) && CMT_RX_STOCK_TEST)
        // 0x38/0x3C-Patches ENTFERNT (2026-08-31): die BISECT-Konfig (die
        // EINZIGE mit vollendeten Paketen!) liess beide Register auf den
        // Stock-Werten — 0x38=0x12 und 0x3C=0x02 sind also teil des
        // funktionierenden Rahmens. Die "Preamble 1 Unit"-These ist tot.
        // RSSI Valid Source (0x16): auch revertiert (Stock läuft mit PJD).
        // ---- ZEROS-ROOT-CAUSE (2026-09-05, Runde 34): das DEMO-FRAMING-
        // Experiment (2026-09-01) setzte 0x45 bit0=1 (PKT_TYPE=VARIABLE
        // LENGTH) + 0x4C bit0=1 (CRC_EN) und blief unausgerottet in JEDE
        // Firmware seit long50. Mechanismus der 00-Frames: in VarLen liest
        // der Chip das erste Byte nach dem Sync als Laengenbyte — im wM-Bus
        // ist das L=0x54 (84) > 64-B-FIFO → Overflow-Drop + FIFO-Clear →
        // PKT_DONE mit leerem FIFO (frames==sync, RAW 64x00, [RX 1-8]).
        // FIX: Fixed-Length + CRC aus (Software-Scan macht CRC) = der
        // Zustand der T100K-Map und der historisch CRC-OK-bewiesenen Laeufe.
        radio.writeReg(0x45, radio.readReg(0x45) & ~0x01);   // PKT_TYPE=0 FIXED
        radio.writeReg(0x4C, radio.readReg(0x4C) & ~0x01);   // CRC_EN=0
        // Sync 0x543D (SDR-verifiziert) — 0x3B ist TX_PREAMB_SIZE[15:8]
        // (Datasheet Tabelle 21, 600-DPI-Crop-Runde), NICHT "Preamble-Wert":
        // der alte 0x3B=0x55-Write setzte die TX-Preamble-Size auf 0x55xx
        // Bits (TX-Diag-Anomalie!). Die Map traegt 0x3B=0xAA (Stock) — kein
        // Write noetig. Heartbeat (10 s) schreibt 0x43/0x44 wertgleich nach.
        radio.writeReg(0x43, 0x3D);   // sync lo
        radio.writeReg(0x44, 0x54);   // sync hi (zuerst im Air)
#endif  // !CMT_RX_STOCK_TEST
        // TT_DEM (falsifiziert + REVERTED 2026-08-30): 0x59/0x5A sind NICHT
        // die Deviation — die liegt in 0x56/0x57 (LE, 12.407 Hz/LSB). Der
        // fruehere "Restore" las hier zusaetzlich aus dem falschen Array
        // (RFPDK_REG_MAP statt map) und ueberschrieb die RFPDK-T100K-Werte
        // mit EBYTE-Defaults (0x42/0xB0). Block komplett entfernt; begin()
        // hat die map-Register bereits geladen.
#if defined(CMT_POLARITY_INV) && CMT_POLARITY_INV
        Serial.println(F("Polarity test patch DISABLED (map defaults for TX)"));
#endif
        // Verify the PLL words are the RFPDK ones.
        Serial.print(F("PLL regs 0x18-0x1F:"));
        for (uint8_t a = 0x18; a <= 0x1F; a++) Serial.printf(" %02X", radio.readReg(a));
        Serial.printf("  (RFPDK: 42 22 D3 8D 42 17 7A 1D)\n");
        // Full register-map integrity check: dump 0x0C-0x5F and compare
        // against the loaded map (plus our runtime patches).
        Serial.print(F("REG CHECK diffs:"));
        int nDiff = 0;
        for (uint8_t a = 0x0C; a <= 0x5F; a++) {
            uint8_t v = radio.readReg(a);
            uint8_t exp = pgm_read_byte(&map[a - 0x0C]);
            // Runde 34: die "sturen Diffs" waren ALLE erklaert — 0x45/0x4C
            // (DEMO-FRAMING-Patch, jetzt revertiert), 0x3B (0x55-Write,
            // entfernt), 0x38 (STALE Patch-Erwartung; der 2026-08-31
            // entfernte Patch lebte hier als exp-Zeile weiter!). Die Map
            // traegt jetzt ALLE aktiven Werte (0x43=3D/0x44=54 inklusive)
            // — diffs>0 ist wieder ein echtes Fehlsignal.
            if (v != exp) {
                Serial.printf(" 0x%02X:exp=%02X got=%02X", a, exp, v);
                nDiff++;
            }
        }
        Serial.printf("  (diffs=%d)\n", nDiff);
        Serial.println(F("goRx() ..."));
        radio.goRx();
        Serial.println(F("Continuous RX armed (868 MHz)"));
#else
        radio.setTxPower(CMT2300A_TX_POWER_DBM);
        radio.setModulation((CmtModulation)CMT2300A_MODULATION);
#endif
    }

    // Web
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/tx", HTTP_POST, handleTx);
#if !(defined(CMT_WIFI_OFF) && CMT_WIFI_OFF)
    server.begin();
#endif
    Serial.println(F("Web server started on :80"));
#endif  // CMT_STM8_ONLY — Rest von setup() endet hier für den Bridge-Modus
}

void loop() {
#if defined(CMT_STM8_ONLY) && CMT_STM8_ONLY
    // C5-Probe + rohe Funkbytes der Stock-Firmware-Brücke
    static uint32_t lastAt2 = 0;
    static uint8_t baudIdx = 0;
    static const uint32_t bauds[] = { 9600, 115200, 19200, 4800, 38400 };
    if (millis() - lastAt2 > 7000) {
        lastAt2 = millis();
        Serial2.end();
        Serial2.begin(bauds[baudIdx], SERIAL_8N1, CMT_PIN_INT2, CMT_PIN_INT1);
        delay(2);
        Serial.printf("[STM8-UART] baud=%lu g15=%d g16=%d\n",
                      (unsigned long)bauds[baudIdx],
                      digitalRead(CMT_PIN_INT1), digitalRead(CMT_PIN_INT2));
        baudIdx = (baudIdx + 1) % (sizeof(bauds) / sizeof(bauds[0]));
        while (Serial2.available()) (void)Serial2.read();
        const uint8_t info[10] = {0xC5,0xC5,0xC5,0x01, 0,0,0,0,0,0};
        Serial2.write(info, sizeof(info));
        String resp;
        uint32_t t2 = millis();
        while (millis() - t2 < 800) {
            while (Serial2.available()) resp += (char)Serial2.read();
        }
        if (resp.length()) {
            Serial.printf("[STM8-C5] Antwort %u B:", resp.length());
            for (unsigned i = 0; i < resp.length(); i++)
                Serial.printf(" %02X", (uint8_t)resp[i]);
            Serial.println();
        } else {
            Serial.println(F("[STM8-C5] keine Antwort"));
        }
    }
    static String rawAcc2;
    while (Serial2.available()) {
        rawAcc2 += (char)Serial2.read();
        if (rawAcc2.length() >= 24) {
            Serial.printf("[STM8-RX raw]");
            for (unsigned i = 0; i < rawAcc2.length(); i++)
                Serial.printf(" %02X", (uint8_t)rawAcc2[i]);
            Serial.println();
            rawAcc2 = "";
        }
    }
    return;
#endif
#if !(defined(CMT_WIFI_OFF) && CMT_WIFI_OFF)
    server.handleClient();
#endif
#if !(defined(CMT_STM8_ONLY) && CMT_STM8_ONLY)
    pollRx();
#if defined(CMT_TX_TEST) && CMT_TX_TEST
    // TX-Beacon: 2 s Takt ueber FCSB=g5 mit dem Dev-Patch (TT_DEM). Das
    // Urteil faellt der Watchdog-Log (Beacon-ID 01234567 dekodiert?).
    static uint32_t lastTxBeacon = 0;
    if (millis() - lastTxBeacon > 2000) {
        lastTxBeacon = millis();
        static uint8_t beacon[31];      // FIFO fuellt 31 B (Fix-Length-Modus 0x46=0x1F)
        // CRC-valides Minimal-wM-Bus-Mode-C-Telegramm (rtl_433 -R 104
        // verifizierbar): [54 CD] + Block1 (10 B: L C M2 A4 V T) + CRC16 +
        // 1 Data-Byte + CRC16.  rtl_433 checkt beide CRCs.
        beacon[0] = 0x54; beacon[1] = 0xCD;
        beacon[2] = 0x0A; beacon[3] = 0x44;
        beacon[4] = 0x93; beacon[5] = 0x44;   // M-Feld: air = [M0 93][M1 44] -> rtl_433 M=0x4493 "QDS"
        beacon[6] = 0x67; beacon[7] = 0x45;   // A-ID BCD little-endian: bcd2int(d7)*1e6+... -> 1234567
        beacon[8] = 0x23; beacon[9] = 0x01;
        beacon[10] = 0x35; beacon[11] = 0x1A;
        uint16_t c1 = wmbusCrc(beacon + 2, 10);
        beacon[12] = c1 >> 8; beacon[13] = c1 & 0xFF;
        beacon[14] = 0x78;                      // CI=0x78 Application Status (wie Qundis)                       // ein Data-Byte
        uint16_t c2 = wmbusCrc(beacon + 14, 1);
        beacon[15] = c2 >> 8; beacon[16] = c2 & 0xFF;
        memset(beacon + 17, 0x55, 14);   // Pad (Decoder liest nur 15 Bytes)
        radio.goStandby();
        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x01);   // TX FIFO clear (0x6C)
        delay(2);
        radio.writeFifo(beacon, sizeof(beacon));
        radio.goTx();
        delay(8);
        uint8_t st = radio.readReg(CMT_REG_CTL1_MODE_STA);
        uint8_t lo = radio.readReg(0x6A);
        uint8_t hi = radio.readReg(0x6D);
        radio.goStandby();
        Serial.printf("[TX] st@8ms=0x%02X lo=0x%02X hi=0x%02X (TX_DONE=%d)\n",
                      st, lo, hi, (lo >> 3) & 1);
    }
    // Continuous TX test: beacon every 2 s so the SDR sees an unmistakable
    // periodic pattern.
    static uint32_t lastTx = 0;
#endif
#if defined(CMT_STM8_ONLY) && CMT_STM8_ONLY
#else
#if CMT_OMS_T1
    // Heartbeat so an attached serial monitor shows the recorder is alive
    // even when the band is quiet.
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 10000) {
        lastBeat = millis();
#if !(defined(CMT_RX_STOCK_TEST) && CMT_RX_STOCK_TEST)
        // ---- Konvention A FEST (Beacons dekodieren in A! Polarität = OK):
        // Sync 0x543D. Die A/B-Alternierung ist Geschichte (TX-Kette seit
        // dem TX-Bank-Fix rtl_433-geprüft CRC-OK). 0x3B-Write ENTFERNT
        // (Runde 34): 0x3B = TX_PREAMB_SIZE[15:8] (nicht "Preamble-Wert") —
        // 0x55xx Bits TX-Preamble-Size war der [TX diag]-Anomalie-Kandidat.
        radio.writeReg(0x43, 0x3D);
        radio.writeReg(0x44, 0x54);
        Serial.println(F("[POL] Konvention A fix (sync=543D)"));
        // (REP-BIT-Flip und RX-Sweep entfernt 2026-09-02: der Flip toggelt die
        // Demod-Polarität (0x1F Bit4) je Heartbeat — unter dem SWFRAM-Test
        // lief die Hälfte der Fenster mit falscher Polarität; der Sweep
        // verstimmt die Frequenz. Beide haben ihre Erkenntnis geliefert.)
#endif  // !CMT_RX_STOCK_TEST
#if defined(CMT_REGVAR) && CMT_REGVAR
        // ---- Register-Varianten-Test (Ghidra-Feldmap 2026-09-02): die
        // undokumentierten RX-Register sind entschlüsselt (0x22=flt_bw_sel/
        // mixer_bw_sel, 0x24=cic/fir/cor_stage, 0x26=slicer_cor_th, 0x2F=AGC
        // en/mask/method/cnt, 0x30=AGC loop gain/rate, 0x0C=analog gain).
        // JEDE Variante nur 1-3 Register, 30 s Fenster, Zaehler: Frames/
        // Flotten-Praefix 54CD/RSSI. Frage: welche Aenderung laesst die
        // starken Flotten-Telegramme (-68..+18 dBm) komplettieren?
        {
            struct RegVar {
                const char *name;
                uint8_t r[3];   // 0xFF = kein Write
                uint8_t v[3];
            };
            static const RegVar vars[] = {
                {"baseline",      {0xFF, 0xFF, 0xFF}, {0, 0, 0}},
                {"lna_mode2",     {0x0C, 0xFF, 0xFF}, {0x8E, 0, 0}},
                {"lna_mode0",     {0x0C, 0xFF, 0xFF}, {0x2E, 0, 0}},
                {"agc_mask_off",  {0x2F, 0xFF, 0xFF}, {0x51, 0, 0}},
                {"agc_loop7",     {0x30, 0xFF, 0xFF}, {0x38, 0, 0}},
                {"cic_bw7",       {0x24, 0xFF, 0xFF}, {0x67, 0, 0}},
                {"fir_bw5",       {0x24, 0xFF, 0xFF}, {0x6B, 0, 0}},
                {"slicer_cor_1f", {0x26, 0xFF, 0xFF}, {0x1F, 0, 0}},
                {"combo_lna_agc", {0x0C, 0x2F, 0x30}, {0x8E, 0x51, 0x38}},
                {"restore",       {0x0C, 0x2F, 0x30}, {0xAE, 0x53, 0x20}},
            };
            static uint8_t varIdx = 0;
            const RegVar &rv = vars[varIdx];
            varIdx = (varIdx + 1) % (sizeof(vars) / sizeof(vars[0]));
            radio.goStandby();
            for (int k = 0; k < 3; k++) {
                if (rv.r[k] != 0xFF) {
                    radio.writeReg(rv.r[k], rv.v[k]);
                    Serial.printf("[REGVAR] 0x%02X <- 0x%02X\n", rv.r[k], rv.v[k]);
                }
            }
            radio.setFrequencyHz(868950000UL);
            radio.goRx();
            uint8_t nFrames = 0, nFleet = 0;
            int rssiLo = 999, rssiHi = -999;
            int max6f = -1;
            uint32_t tw = millis();
            while (millis() - tw < 30000) {
                uint8_t buf[64];
                uint8_t rxLen = 0;
                int rssi = -127;
                CmtRxResult r = radio.rxPacket(buf, sizeof(buf), &rxLen, &rssi, 10);
                int f6 = (uint8_t)radio.readReg(0x6F);
                if (f6 > max6f) max6f = f6;
                if (r == CMT_RX_OK) {
                    nFrames++;
                    if (rssi < rssiLo) rssiLo = rssi;
                    if (rssi > rssiHi) rssiHi = rssi;
                    bool fleet = (rxLen >= 2 && buf[0] == 0x54 && buf[1] == 0xCD);
                    if (fleet) nFleet++;
                    Serial.printf("[REGVAR %s] frame %uB rssi=%d %s:",
                                  rv.name, rxLen, rssi, fleet ? "FLEET" : "noise");
                    for (uint8_t i = 0; i < rxLen && i < 8; i++) Serial.printf(" %02X", buf[i]);
                    Serial.println();
                }
            }
            Serial.printf("[REGVAR %s] RESULT frames=%u fleet=%u rssiLo=%d rssiHi=%d 0x6Fmax=%d\n",
                          rv.name, nFrames, nFleet, rssiLo, rssiHi, max6f);
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
        }
#endif  // CMT_REGVAR
#if defined(CMT_RAWCAP) && CMT_RAWCAP
        // ---- Roh-Bitstrom-Fang (nach dem REGVAR-Fazit: 10 Gain/AGC/BW-
        // Varianten, 0 Flotten-Frames — der Fehler ist VOR dem Sync-Wort).
        // Preamble- (0x38[7:3]=0) und Sync-Detektor (0x3C=0x00) AUS:
        // der Chip komplettiert Fixed-31-Frames non-stop und wir lesen den
        // DEMOD-Bitstrom roh. Ein Flotten-Telegramm zeigt sich als
        // 543D + 54 CD ... irgendwo im Strom. Die Framerate misst nebenbei
        // die echte Demod-Bitrate (frames/s * 248 bit = rate).
        {
            radio.goStandby();
            uint8_t preamSave = radio.readReg(0x38);
            uint8_t syncSave = radio.readReg(0x3C);
            radio.writeReg(0x38, 0x02);   // RX_PREAM_Size=0, Data_MODE=Packet (0x00 waere Direct!)
            radio.writeReg(0x3C, 0x00);   // Sync None
            radio.setFrequencyHz(868950000UL);
            radio.goRx();
            uint32_t tw = millis();
            uint32_t nFrames = 0;
            uint8_t buf[31], prev[31];
            bool havePrev = false;
            int max6f = -1;
            while (millis() - tw < 20000) {
                int f6 = (uint8_t)radio.readReg(0x6F);
                if (f6 > max6f) max6f = f6;
                uint8_t hi = radio.readReg(0x6D);
                if (hi & 0x01) {                       // PKT_DONE
                    radio.clearIntFlagHi(0x01);
                    radio.readFifo(buf, 31);
                    nFrames++;
                    // Flottenmuster ueber die Framengrenze suchen
                    for (int p = -31; p < 30; p++) {
                        uint8_t b0 = p < 0 ? prev[p + 31] : buf[p];
                        uint8_t b1 = p + 1 < 0 ? prev[p + 32] : buf[p + 1];
                        if (havePrev && b0 == 0x54 && b1 == 0xCD) {
                            Serial.printf("[RAWCAP 54CD @%ld] RSSI=%d prev:",
                                          (long)nFrames, max6f);
                            for (int i = 0; i < 31; i++) Serial.printf(" %02X", prev[i]);
                            Serial.print(" cur:");
                            for (int i = 0; i < 31; i++) Serial.printf(" %02X", buf[i]);
                            Serial.println();
                            break;
                        }
                    }
                    memcpy(prev, buf, 31);
                    havePrev = true;
                    if (nFrames % 50 == 1) {   // Roh-Frame-Probe (Bitmuster sichtbar machen)
                        Serial.printf("[RAWCAP probe] RSSI=%d:", max6f);
                        for (int i = 0; i < 31; i++) Serial.printf(" %02X", buf[i]);
                        Serial.println();
                    }
                    // re-arm (startRx = FIFO clear + goRfs + goRx)
                    radio.goStandby();
                    radio.startRx();
                }
            }
            Serial.printf("[RAWCAP] frames=%lu in 20s => rate ~%lu bps (248 bit/frame) 0x6Fmax=%d\n",
                          (unsigned long)nFrames,
                          (unsigned long)(nFrames * 248 / 20), max6f);
            // Register zurueck
            radio.goStandby();
            radio.writeReg(0x38, preamSave);
            radio.writeReg(0x3C, syncSave);
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
        }
#endif  // CMT_RAWCAP
#if defined(CMT_TMODE_PROBE) && CMT_TMODE_PROBE
// Runde 83b: Data-Rate-Bank (0x20..0x37) aus RFPDK_REG_MAP_T1
// (include/cmt2300a_config.h; 32.8 kbps / 50 kHz Dev / Counting-CDR /
// GFSK). Wird PRO SWFRAM-Zyklus nach der agc20-Varianten-Anwendung
// geschrieben (Varianten ueberschreiben 0x22/0x24/0x25/0x26/0x30 mit
// den 100k-Sweetspots). CDR_BR_TH = 0x0318 = 792 = 26e6/32800.
static const uint8_t kT1DataRate[24] PROGMEM = {
    0xB4, 0x4A, 0x21, 0x10, 0xEA, 0x13, 0x1F, 0x0A,
    0x9F, 0x5A, 0x29, 0x29, 0xC0, 0x18, 0x03, 0x53,
    0x18, 0x00, 0xB4, 0x00, 0x00, 0x01, 0x00, 0x00 };
#endif
#if defined(CMT_SWFRAM) && CMT_SWFRAM
        // ---- Software-Framing / Sync-Shift (der RAWCAP-Beweis: der Demod
        // liefert gueltige Telegrammbits — CRC-geprueft — aber der Burst-
        // Anlauf (AGC/CDR-Settle) frisst Preamble + Sync-Erstes-Byte, sodass
        // der Hardware-Sync (54 3D) nie greift. FIX: Preamble-Detektor AUS
        // (0x38=0x02) und das Sync-Wort auf das BURST-UEBERLEBENDE Muster
        // schieben: "3D 54" bzw. "3D 54 CD" (= Sync-Tail + 54CD-Header).
        // Der CRC-Scan ueber jedes Frame validiert Telegramme unabhaengig
        // von der Sync-Position.
        {
            // Hebel-Runde 3: AFC-Windup-Hypothese. Die AFC (ki=12 kp=6)
            // integriert in den RX-Idle-Phasen auf Rauschen weiter; beim
            // Burst-Start steht sie daneben -> Demod-Muell bis sie reinzieht.
            // L1: AFC aus (0x28: 0x9F->0x8F). L2: + Sync-Toleranz 2 Bit
            // (0x3C bits[6:5]=2 -> 0x42). L3: Tracing-CDR (0x2B=0x28).
            // Runde 14: 3-Byte-Sync "3D 54 CD" feuert DETERMINISTISCH am
                // Header (off=0) -> 64B-Paket = Telegramm 0..63 KOMPLETT.
                // B2-Check probiert BEIDE L-Interpretationen: (a) L zaehlt
                // Daten ohne CRCs (CRC2 bei 12+L-9), (b) L = alles nach L
                // (CRC2 bei 12+L-13). Beacon-Beleg: (a) stimmt fuer L=10;
                // der Fleet-Data-Vergleich entscheidet.
                // Fest: 0x0C=0xA2, 0x2F=0x52, 0x24=0x63, 0x25=0x12, 0x26=0x0F,
                // 0x28=0x8F, 0x2B=0x29, 0x2C=0xC0, 0x43b1=0x3D/0x54 je Variante.
                // Runde 17: DIE ENTLARVUNG — der AGC dreht im Idle die
                // Verstärkung hoch (RSSI-Timeline: 0x6F=0x00 im Idle!), beim
                // Burst muss er ABBREGELN -> während des Abregelns ist der
                // Demod gesättigt = DER DIE-OFF. Hebel: agc_loop_gain/rate
                // (0x30[2:0]/[6:3], aktuell gain=0 rate=4 = TRÄGE!) auf MAX.
                // Fest: AGC an (0x2F=0x53), sync "54 CD" 2B (0x3C=0x02),
                // 0x46=0x40, 0x0C=0xA2, 0x24=0x63, 0x25=0x12, 0x26=0x0F,
                // 0x28=0x8F, 0x32=0xB4, 0x2C=0xC0, 0x43=0xCD, 0x44=0x54.
                // Runde 32: ROOT CAUSE der off-Streuung gefunden. Mode C-A
                // Preamble = {55 54 3D 54 CD} (rtl_433 m_bus.c PREAMBLE_CA);
                // "3D 54 CD" = der CDR-ueberlebende Tail. Der Streaming-Flow
                // laesst den Chip PERMANENT in Rausch-Paketen laufen (~1.5
                // Noise-Syncs/s bei 2-Byte-Sync); der Burst-Sync trifft
                // mitten in ein Rausch-Frame -> FIFO-Pointer nicht resettet
                // -> Telegramm bei zufaelligem off (0..48 beobachtet), CRC2-
                // Tail meist Einfach-Abdeckung (Oracle: keine <=3-Bit-Loesung).
                // 3-Byte-Sync tol=0: Noise-Syncs ~2^-24 -> tot (BEWIESEN:
                // frames 56 -> 1 im ersten sync3B-Fenster!). Chip armiert
                // pro Repeat neu -> off=0 deterministisch, JEDER Repeat ein
                // Vollcapture -> asmMerge-Majority ueber 3 Repeats = B2OK.
                // Rotation: sync3B/sync3Bt1 dominieren (6/10), die AGC-A/Bs
                // (loopMax/loopGain7 = AGC an, agcOffMax/agcOffMid = AGC aus)
                // bleiben mit je 1 Fenster alive.
                // sync3Bt1 (tol=1) gestrichen: 1-Bit-Toleranz multipliziert die
                // Noise-Syncs x729 -> wieder 65 Frames/Fenster = Flooding.
                // Runde 34 (2026-09-05): ZEROS-ROOT-CAUSE gefunden — drei
                // eigene Bugs, alle aus Runde 28-32:
                //  (1) DEMO-FRAMING (0x45 bit0=1 VarLen + 0x4C bit0=1 CRC)
                //      blief seit 2026-09-01: L=0x54 (84) > 64-B-FIFO ->
                //      Overflow-Drop -> PKT_DONE mit leerem FIFO = 00-Frames.
                //  (2) 3-Byte-Sync {3D 54 CD} frisst L+C: Luftmuster
                //      55 55 | 54 3D | 54 CD 3E 44... — der 3-Byte-Sync
                //      matcht Position 4-6, der FIFO startet bei M1, L(84)
                //      und C sind als Sync-Byte 2+3 konsumiert -> Scan
                //      findet kein Block1. 2-Byte {54 3D} = FIFO ab L.
                //  (3) 10-s-Heartbeat schrieb 0x43/0x44 wertfremd mid-
                //      window -> 3-Byte-Sync ab 10 s tot (15/25 s). Mit
                //      2-Byte-Fenstern sind die Heartbeat-Writes wertgleich.
                // => Framing-Matrix: merge64 (64-B-FIFO, 0x46=0x40, der
                // historisch oracle-bewiesene Modus) vs fx31 (32-B-RX-Halb-
                // FIFO, 0x46=0x1F, die BISECT-Konfig) x 4 AGC-Varianten
                // (gA=AGC-on MAX-Loop 3F, gB=0xA6/20, gC=0xAE/20, gD=0x53/20).
                // mg64-Frames gehen in asmMerge; fx31 lesen nur 31 B (Tail
                // jenseits der RX-Haelfte nie im FIFO), deshalb dort kein
                // Merge (Null-Votes wuerden reale Bytes outvoten).
                // Runde 35: Sync = 4 Byte {54 3D 54 CD} = Mode-C-Format-A-
                // Preamble (rtl_433 m_bus.c:941 PREAMBLE_CA). Der 2-Byte-Sync
                // matchte mitten im Access-Code -> FIFO startete bei [54 CD],
                // CRC4-Byte lag bei pos 64 > 63 und fehlte. r3C=0x06 =>
                // SYNC_SIZE=4 (Bits 3:1, Wert = nbytes-1; AN143/AN192,
                // deckungsgleich mit empirisch 0x02=2 Byte). Jetzt: Frame
                // komplett bei buf[0] (64 B exakt im 64-B-FIFO, L-Feld an
                // buf[0], CRC4 bei j62-63 — Frame=12+(L-9)+2*nblk=64 bei L=55).
                // Runde 38 (2026-09-05) EMPFINDLICHKEITS-SWEEP. Baseline-Quote
                // (Produktiv-Konfig mg64-gA, 3 Captures): 37-40 % der
                // Flotten-Bursts, kumulativ 0 False-Positives. Befunde: (a)
                // gefangene Frames sind die STAERKSTEN (SDR-rssi ~+1 dB,
                // Chip-RSSI 89-155); verpasste Mittel -6.1 dB -> ~6-10 dB
                // Empfindlichkeitsluecke ggue. rtl_433-Demod. (b) 17 % Dead-
                // time durch den 5-s-RSSI-SPIKE-Heartbeat-Block (jetzt 1 s).
                // (c) Bursts kommen in Paaren ~1.13 s (Qundis-Repeat).
                // Sweep: je 25 s EINsingle-Knob gegen die mg64-gA-Baseline
                // (REGVAR-Neins aus der 2-Byte-Sync-Aera sind ueberholt).
                // 0xFF = Register unveraendert lassen.
                // Runde 39 (2026-09-05) PRODUKTIV-PINNUNG: Sweep-Auswertung
                // frame-genau (Chip-Roher-FIFO deblockt vs rtl_433-Data) zeigt:
                // die "verpassten" Frames lagen ueberwiegend in bw21-Fenstern
                // (0x22=0x21 demod-tot), dem afc9f-Deaf-Fenster (rssiHi=58,
                // AFC-Verstimmung) oder Deadtime — nicht in gesunden Fenstern.
                // Winner max-ae (0x0C=0xAE, AFC aus 0x8F) pinnen, Rotation aus.
                // Runde 40 (2026-09-05) RSSI-BUCKET-ANALYSE: Fangquote nach
                // SDR-Pegel (in-Fenster): >=0 dB: 84%, -4..0: 22%, <-4: 0%.
                // Der Gegner ist die Empfindlichkeit, nicht das Fenster. Beweis
                // RAW-Fang f=7 (6154525): Burst kam AN (RSSI 77 vs. Geschwister
                // 130-161), Bitfehler Byte 5 (45->65) -> CRC1 fail. Der SDR-ADC
                // ist gesaettigt (+1.2..+1.4 dB fuer ALLE starken Meter) und
                // sieht die relativen Pegel nicht. AGC unterversorgt schwache
                // Bursts: Gain ist fuer starke Bursts heruntergeregelt, der
                // 77er-Burst bleibt unverstärkt. Hebel: AGC AUS (0x2F=0x52),
                // statisches Max-Gain (0x0C=0xAE) — agcoff, Single-Knob.
                // Runde 42: tgtHigh — agc_target (0x32, RFPDK fix 0xB4=71%)
                // auf 0xF0=94%: der Demod darf bis 94% Aussteuerung regeln
                // (schwache Bursts mehr Reserve-Gain), Abregelung für starke
                // bleibt. Der letzte nicht-probierste AGC-Regler.
                // Runde 45: r46 = 0x49 (73) statt 0x40 — der Packet-Handler
                // zaehlt selbst bis Frame-Ende (EN 13757-4 12.5.3: Rahmen =
                // 12+(L-9)+2*nblk; L=62 -> 73 B), PKT_DONE laesst NACH dem
                // letzten Luft-Byte statt am 64. -> der FIFO haelt alle 73
                // (Mid-Burst-Drain haelt den Peak <= 64: Mock 50/41, lost 0).
                struct SyncVar { const char *name; uint8_t r0C, r2F, r30, r42, r41, r43, r44, r3C, r46, r22, r26, r24, r25, r28, r2A, r2B, r32, merge, dump; };
                static const SyncVar svars[] = {
                    {"prod-ae",  0xAE, 0x53, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x8F, 0x29, 0x29, 0xFF, 1, 1},
                    {"agcoff",   0xAE, 0x52, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x8F, 0x29, 0x29, 0xFF, 1, 1},
                    {"tgtHigh",  0xAE, 0x53, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x8F, 0x29, 0x29, 0xF0, 1, 1},
                    // Runde 56b: AFC an (r28 bit4, T1-Map-Default 0x9F). Die
                    // progressiven CRC-Fails (r55: TTTFF/TTFFF = CDR-Slip
                    // MID-Frame) sind der Frequenz-Offset-Hebel: die Meter-
                    // Carrier liegen neben dem Kanal, der CDR rutscht ohne
                    // AFC-Nachfuehrung ab. afdon sonst identisch zu prod-ae.
                    {"afcon",    0xAE, 0x53, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x29, 0xFF, 1, 1},
                    // Runde 66 (2026-09-06): CDR TRACING statt COUNTING.
                    // BBAD-Beweiskette (r65_stable.py, rtl_433-Wahrheit): die
                    // chronischen Meter kommen on-air CRC-sauber an, unser
                    // Demod schiebt EIN Bit an Blockgrenzen ein (fe 8b 98 40
                    // = fd 17 30 80 + Insert@384; 7e d6 3f = fd ac 7e +
                    // Insert@528) bzw. flippt Einzelnbits im letzten Drittel
                    // — gerätedeterministisch, RSSI-/TH-unabhängig. AFC
                    // (afcon, Runde 59) nahm den statischen Offset, der Rest
                    // ist Symbolraten-/Carrier-Tracking: Datasheet 4.3.8 —
                    // COUNTING braucht akkuraten Symboltakt, TRACING trackt
                    // die TX-Rate (bis 15.6% Fehler). 0x2B bit0: 0x29 =
                    // Counting (Map-Default), 0x28 = Tracing (Hebel-Runde 3).
                    {"cdrt",     0xAE, 0x53, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x28, 0xFF, 1, 1},
                    // Runde 67 (2026-09-06): TRACING-VERDICT (r66, 35 Frames):
                    // cdrt 15K/13X = 43% BOK ggü. afcon 45K/7X = 86% — TRACING
                    // ist NETTO SCHLECHTER (saubere Geraete regressen: 929
                    // 6K/0->2K/1X, 839 6K/0->2K/1X, 809 6K/0->1K/1X; Binomial
                    // p~0.3%). Revert zu Counting (0x2B bit0=1) + FELDMAP-HEBEL:
                    // 0x2B[6:4] = cdr_avg 2->3 (Ghidra-RE, Memory
                    // s3-dongle-wiring-verified): CDR-Periodenschaetzung ueber
                    // mehr Kanten mitteln. Slip-Gesetz (r66_slipruns.py): alle
                    // High-Quality-Slip-Fits (>=40/48 Bits) sitzen in
                    // Konstant-Bit-Runs von 14-21 Bit (00 00@14-15 = 18 Nullen,
                    // FF FF FD@29-32 = 21 Einser, 00 00 32@50-52) — der CDR
                    // free-runnt dort mit verrauschtem Periodenschaetzer
                    // (cdr_avg=2, AGC-Loop r30=0x3F max). Bei kristallgenauer
                    // Rate (ppm) ist aggressives Mitteln gratis; Datasheet
                    // 4.3.8: COUNTING haelt "unlimited length of 0" NUR bei
                    // 100% alignedem Symboltakt. 0x29: mode=1,range=2,avg=2 ->
                    // 0x39: mode=1,range=2,avg=3.
                    {"cdavg",    0xAE, 0x53, 0x3F, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 68 (2026-09-06): cdavg-VERDICT (r67, 499 Frames,
                    // L-getrennt): L55 sauber 55K/2X = 3.5% X (afcon-Niveau,
                    // Kriterium a auf Baseline-Mischung ERFUELLT), chronische
                    // 73913647 2 BOK + 86913647 1 BOK (erste BOKs ever) —
                    // ABER 94424347 0K/6X (weiterhin nie BOK, Kriterium b
                    // OFFEN) und L62 sauber 184K/21X = 10.2% (839: 30.8%).
                    // L94 = 0K/94X strukturell tot (109 B on-FIFO > 64-B-
                    // FIFO, B2 P/F dann B3+ F = Overflow-Klasse, NICHT CDR).
                    // RSSI(X) 52..161: auch STARKES Signal failt -> AGC-
                    // Thrash-Verdacht. HEBEL: r30 0x3F -> 0x20 (Ghidra-RE-
                    // Feldmap: [2:0] agc_loop_gain 7->0, [6:3] agc_loop_rate
                    // 7->4 = RFPDK-100k-Auto-Export; unser 0x3F war fruehes
                    // Experiment, nie RFPDK-Default). Alles andere wie cdavg
                    // (0x2B=0x39).
                    {"agc20",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 85c (2026-09-12): 54-CD-WALL — 2-B-Sync-Hebel.
                    // Beweisstand: Luft = [preamble][Chip-Sync 543D][54CD-Tail]
                    // [Telegramm] (rtl_433, 83g-Klarstellung). Die aktive Map
                    // (T100K, config.h) hat 0x3C=0x02 = 2-B-Sync; agc20
                    // ueberschreibt mit 0x06 = 4-B-Sync 543D-54CD -> der Sync-
                    // Matcher konsumiert {54 CD} und der FIFO startet beim
                    // L-Feld (L-first, dr=3-bewiesen). Der Probe (ohne svar-
                    // Row -> Map-Wert 0x3C=0x02) faengt ab {54 CD} und heilt
                    // die Chroniker (82-88% FULL/PARS vs 0% BOK Produktion).
                    // sync2b = agc20, NUR 0x3C=0x02 (r46 wird unten sync-
                    // abhaengig auf 0x6F=111 gesetzt: 109-B-Frame + 2 Tail).
                    {"sync2b",   0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x02, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 70 (2026-09-06): fir6 — der r69-IQ-Befund
                    // (r69_iq_run.py, Run-Length-Demod + Shift-Hamming):
                    // die Luft ist BIT-EXAKT sauber (hdr=0 run=0 in allen 24
                    // zugeordneten Frames, chronisch UND sauber; DP auf Burst
                    // 82: sub=7 ins=41 nur am Burst-Ramp, FIFO 2-59 perfekt).
                    // Die Korruption ist CHIP-INTERN. Neue Signatur: Median der
                    // 1-Bit-Runs (6-14 Samples @ 1 MS/s) = Transition-Timing-
                    // Bias (GFSK-ISI): chronisch 47434294 9.00, 47433220 9.50,
                    // 47434354 9.75 vs. sauber 10.00 — bei NORMALER Baudrate
                    // (Frame-Span alle 99.9-100.5 kBd, Baud-Hypothese tot).
                    // Mechanismus: Payload-Muster -> Kanten gezogen -> CDR-
                    // Periodenfehler -> Slip im Konstant-Run. HEBEL: der letzte
                    // nicht-maximierte BW-Knopf der Fieldmap (flt_bw_sel=3 max,
                    // mixer_bw_sel=1, cic_bw=3 max, NUR fir_bw=4) — fir_bw 4->6
                    // wie 50k-Profil (0x63->0x73: stage1/cic3 bleiben). Weniger
                    // Gruppenlaufzeit-Distortion an den Ton-Kanten = weniger
                    // Kanten-Zug = stabilerer CDR.
                    {"fir6",     0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x73, 0x12, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 71 (2026-09-07): fir3 — Runde-70-VERDICT (r70_
                    // lsplit.py, 581 Frames): fir6 REGREDIERT. Sauber L62-X
                    // 15.1% vs agc20 9.2%, L55-X 5.2% vs 3.0%; 80913647
                    // 38K/4X -> 12K/11X, 839 33K/6X -> 26K/11X, 24204047
                    // 26K/16X -> 0K/10X; chronisch 944/203 weiter 0 BOK.
                    // Gewinn NUR 18324347 (3K/11X -> 21K/10X) + 59969446
                    // erstmals ueberhaupt (6K/9X). Ausserdem Panic-Rate
                    // 5 -> 24 (gleiche Typen, RX-Pfade). FALSIFIKATION der
                    // ISI-Richtung: breiter FIR = MEHR Slips -> CDR-Eingang
                    // rauschgetrieben, nicht ISI-getrieben. HEBEL r71: der
                    // umgekehrte einstufige Gradient fir_bw 4->3 (0x63->
                    // 0x5B, stage1/cic3 bleiben). Prognose-Monotonie: wenn
                    // fir3 < agc20 < fir6 in L62-X, ist der Rausch-Mechanismus
                    // bewiesen; wenn fir3 > agc20, ist 0x63 der Sweetspot und
                    // die Bandbreite ist ausgereizt -> 0x2A/0x25.
                    {"fir3",     0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x5B, 0x12, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 72 (2026-09-07): fir3-VERDICT (r71, Interim 103
                    // Frames/77min vs r68 640/90min) — fir3 ist eine KLIPPE,
                    // kein Gradient: CRC-OK-Volumen kollabiert (~15% von
                    // agc20), 0 L62-Frames in 81 Frames Interim, 0 Panics.
                    // Interpretation: fir_bw 3 schliesst das Auge bei 100kbps
                    // NRZ (Filterbandbreite < Symbolrate-Signalanteil) —
                    // BEIDE Richtungen weg von 0x63 schlechter (fir6 = mehr
                    // Rauschen/Slips, fir3 = kein Empfang) -> 0x63 ist das
                    // Optimum der fir_bw-Achse, Achse ausgereizt. "fir2"-
                    // Zweig pointless (fir3 verliert schon 84% Volumen).
                    // Revert auf agc20. HEBEL r72: 0x2A slicer_peak_th
                    // (Ghidra-RE-Fieldmap, nie variiert, 0x29 fix in ALLEN
                    // RFPDK-Exports 2.4k..100k). 0x29->0x31 (+8): hoehere
                    // Peak-Threshold = mehr Slicer-Hysterese = weniger
                    // rauschgetriebene Kanten am CDR-Eingang (r70-falsifi-
                    // kationskonform). Kein Nibble-RE moeglich (Ghidra-
                    // Setup aus /tmp gereinigt, Datasheet-Registermap
                    // bildbasiert) -> Blind-Probe Richtung +8, ganzer Byte.
                    // Kriterium r72 = Kriterium r70/r71: L62-X sauber < 10%
                    // UND chronisch (944/203) BOK, ohne L55-Regression;
                    // Volumen muss bei ~640 Frames/90min bleiben.
                    {"spk31",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x31, 0x39, 0xFF, 1, 1},
                    // Runde 73 (2026-09-07): spk31-VERDICT (r72, Teilstand
                    // 26 Frames @ HB1400s) — REGRESSION, identische Kollaps-
                    // Signatur wie fir3: 26 CRC-OK @ HB1400s vs agc20 163
                    // (r71 fir3: 26), 0 L62-Frames, chronisch 0 BOK, 0
                    // Panics. 0x2A +8 verliert das Auge wie fir_bw=3 ->
                    // slicer_peak_th ist ein Operating-Point des Demod-
                    // Auges, kein Rauschfilter. Revert auf agc20. HEBEL
                    // r73: 0x2A 0x29->0x21 (symmetrische Gegenprobe −8,
                    // High-Nibble 2 BEHALTEN — 0x31 hat evtl. uebers High-
                    // Nibble einen Modus beruehrt; 0x21 testet sauber das
                    // Low-Nibble/Unter-8-Richtung). Kriterium wie r70-72
                    // + Volumen-Rueckkehr ~640/90min.
                    {"spk21",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x21, 0x39, 0xFF, 1, 1},
                    // Runde 75 (2026-09-07): spk21-VERDICT (r73, Frueh-
                    // verdict @HB1400s 24 Frames) — KOLLAPS identisch fir3/
                    // spk31 (24 vs agc20 163 @ gleichem Soak-Alter). 0x2A −8
                    // kollabiert WIE +8 (r72) -> beide Richtungen weg vom
                    // Operating-Point 0x29, Achse tot. Nibble-Buchhaltung:
                    // 0x29±8 = 0x21/0x31 (Carry/Borrow) — BEIDE Proben
                    // aenderten Low-Nibble 9->1 UND High-Nibble -> rich-
                    // tungsblind; Low-Nibble 9 ist der Working-Point, kein
                    // weiterer 0x2A-Hebel ohne Nibble-RE. Revert auf agc20.
                    // HEBEL r75: 0x25 cor_filt_alpha 0x12->0x00 (Playbook-
                    // Queue nach 0x2A-Tot; gepacktes Feld, nie variiert,
                    // 0x12 fix in allen Exports; EIN Schritt, Variante 0x00
                    // vor 0x32). Kriterium wie r70-73 + Volumen-Rueckkehr
                    // ~640/90min.
                    {"cor00",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x00, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 76 (2026-09-07): cor00-VERDICT (r75, Fruehver-
                    // dict @ ~HB870s: 14 Frames, 11 BOK/3 BBAD, Pace 5@483s
                    // unterhalb Kollaps-Klasse r71-73 6-8@457s) — KOLLAPS
                    // identisch fir3/spk31/spk21: 0 L62-Frames (agc20 257K),
                    // nur L55/49 durch. 0x25=0x00 toetet das cor_filt-Kor-
                    // rektfilter -> Demod-Auge zu. 4. Single-Reg-Kollaps in
                    // Serie (fir3/spk31/spk21/cor00). Revert auf agc20.
                    // HEBEL r76: 0x25-Variante 0x32 (Playbook; High-Nibble
                    // 1->3 OHNE Carry, Low-Nibble 2 bleibt — saubere
                    // Nibble-Probe im Gegensatz zu 0x2A±8). Kriterium wie
                    // r70-75; wenn 0x32 auch kollabiert/neutral -> cor_
                    // filt_alpha inert/kritisch -> Hebel-Queue cdr_range
                    // 0x2B[3:2], dann L94-Diagnose.
                    {"cor32",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x32, 0x9F, 0x29, 0x39, 0xFF, 1, 1},
                    // Runde 77 (2026-09-07): cor32-ABBRUCH auf Julian-An-
                    // weisung nach Pace-Signatur (7-8 Frames @ ~360s, nur
                    // L55, 0 L62 = gleiche Kollaps-Klasse). cor_filt_alpha
                    // als ACHSE TOT gewertet: 0x00 kollabiert (r75), 0x32
                    // kollabiert (r76), 0x12 Map-Default = einziger Work-
                    // ing-Point. 5. Kollaps in Serie auf 4 verschiedenen
                    // Achsen -> gepackte Demod-Register = Messer-Rand.
                    // Revert auf agc20.
                    // HEBEL r77 (Julian): cdr_range 0x2B[3:2] 2->3 (0x39->
                    // 0x3D, bitweise verifiziert: mode[1:0]=1 counting und
                    // avg[6:4]=3 bleiben). Law: COUNTING-CDR free-runnt in
                    // Konstant-Bit-Runs (14-21 Bit) mit verrauschtem Perio-
                    // denschaetzer (Slip-Law r66); mehr Range = groessere
                    // Akzeptanz-Bandbreite fuer den Symboltakt. Kriterium
                    // wie r70-76; danach L94-Diagnose (Julian).
                    {"cdrR3",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x3D, 0xFF, 1, 1},
                    // Runde 82 (2026-09-08): HEBEL cdavg4 — 0x2B[6:4] cdr_avg
                    // 3->4 (0x39->0x49; bitweise verifiziert: [6:4]=4,
                    // [3:2]=range 2 bleibt, mode=counting bleibt). Letzter
                    // nie variierte Feldnachbar der CDR-Cluster-Achsen.
                    // VERDICT (r82, Fruehverdict @HB1167s, Kriterium r70-77):
                    // KOLLAPS #7 — 20 Frames K=13/X=7, Pace ~24 @ HB1400
                    // (= Kollaps-Klasse 24-26, agc20: 163), L-Split {55:20}
                    // = 0 L62-Frames (agc20: L62 = 77% Volumen, P(0/20)
                    // ~1e-13). Twist: Chroniker 94424347 bekam den ERSTEN
                    // BOK ueberhaupt (Boot-Capture + Soak K=1) — avg=4
                    // bessert den Lock auf kurzen Frames, ist aber tracking-
                    // lahm -> Slips akkumulieren auf langen Frames, L62-
                    // Auge zu. Achse beidseitig des Optimums 3 durchprobiert
                    // (r67: 2->3 besser; r82: 4 kollabiert) -> CDR-Cluster
                    // KOMPLETT geschlossen. Revert agc20 (Produktiv-Basis
                    // r79/80: Feed + Repair bewiesen).
                    {"cdavg4",   0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x49, 0xFF, 1, 1},
                    // Runde 84 (2026-09-12): HEBEL cdrt3 — 0x2B bit0 1->0
                    // (COUNTING->TRACING), avg[6:4]=3 und range[3:2]=2 bleiben
                    // (0x39->0x38, bitweise verifiziert). VERDICT (r84-Soak
                    // @HB1400s): KEINE Heilung — 86913647 13x/80913647 12x
                    // BBAD (0 BOK, n=25), Volumen 118 @HB1614 ~ 62% der
                    // agc20-Referenz (163 @HB1400, kein Kollaps-Klassen-
                    // Wert 24-26), L62-ohne-Chroniker X 8.5% ok, 0 Panics.
                    // TRACING+avg3 = tracking-stabiler aber volumenschwaecher
                    // und chroniker-blind -> modexavg Raster komplett,
                    // CDR-Cluster endgueltig geschlossen. r83z-Lang-Capture-
                    // Datenlage: 86913647/80913647 failen 100% X in JEDEM
                    // RSSI-Bin (34/34 @60-79 ... 7/7 @140-159; 809: 14/14
                    // ... 3/3) — gerätespezifisch, NICHT RF (59969446 als
                    // Kontrast: 25% X @60-79, 0% @140+). Repair-Ceiling
                    // (Log/r84_repair_ceiling.txt): residuelle BBADs = F2+x
                    // 209/265 (Multi-Fehler, r81-Modell bestätigt), F1x 47,
                    // F2+r nur 9 -> Software ausgereizt, 869/809 nur an der
                    // CDR-Wurzel erreichbar. Mechanismus: TRACING trackt die
                    // TX-Rate/Phase (Datasheet 4.3.8: bis 15.6% Symbolraten-
                    // fehler), COUNTING free-runnt in Konstant-Runs. r66
                    // testete TRACING NUR mit avg2 (0x28, verrauschter Schät-
                    // zer) — avg3 war exakt der Fix, der r67 die ersten
                    // Chroniker-BOKs brachte; Kombination mode×avg nie
                    // getestet. Risiko: Kollaps #8 (Kriterium-Soak @HB1400s,
                    // agc20-Referenz 163 @HB1400). Erfolgskriterium: Volumen
                    // >100 @HB1400 UND 869/809 BOK>0 ohne Regression der
                    // sauberen Geräte (L62-X < 10%).
                    {"cdrt3",    0xAE, 0x53, 0x20, 0x54, 0xCD, 0x3D, 0x54, 0x06, 0x49, 0xFF, 0x0F, 0x63, 0x12, 0x9F, 0x29, 0x38, 0xFF, 1, 1},
                };
            // Runde 59 (2026-09-05) PRODUKTIV-PINNUNG afcon: r57 4/4 CRC-OK,
            // r58 3/5 -> 7/9 (78%) ggü. prod-ae 2/8, agcoff 1/6, tgtHigh 3/5.
            // Die progressiven CRC-Fails (TTTFF/TTFFF = CDR-Slip MID-Frame)
            // sind der Frequenz-Offset-Hebel: 0x28 bit4 (AFC-Nachfuehrung).
            // Runde 66: PINNUNG cdrt — VERDICT (Runde 67): schlechter, revert.
            // Runde 67: PINNUNG cdavg — VERDICT (Runde 68, 499 Frames):
            // L55 sauber 3.5% X = afcon-Niveau, 739/869 erstmals BOK,
            // ABER 94424347 0K/6X + L62 10.2% X + L94 0/94 (Overflow).
            // Runde 68: PINNUNG agc20 (r30 0x3F->0x20 AGC-Loop RFPDK-100k-
            // Auto gain 0/rate 4; 0x2B=0x39 bleibt). Afcon-Baseline
            // (scratches/r66_baseline.py, nur afcon-Frames r55-r65):
            // chronisch 94424347 0K/4X, 73913647 0K/3X, 20324347 0K/1X,
            // 86913647 0K/1X, 75332806(L=94) 0K/1X; sauber 66324347 7K/0,
            // 89913647 7K/0, 54969446 6K/0, 80913647 6K/0, 83913647 6K/0,
            // 92913647 6K/0, 59434347 4K/0. Kriterium r68: 94424347
            // erstmals BOK UND L62-X-Rate der sauberen Geraete < 10%
            // (r67: 10.2%, 839: 30.8%).
            // Runde 70: PINNUNG fir6 — VERDICT (Runde 71, 581 Frames):
            // REGRESSION, revert. Sauber L62-X 15.1% (agc20: 9.2%), L55-X
            // 5.2% (3.0%), chronisch 0 BOK; einziger Gewinn 18324347.
            // Runde 71: PINNUNG fir3 — VERDICT (r71, Interim 103 Frames/
            // 77min): KLIPPE, kein Gradient. Volumen ~15% von agc20, 0
            // L62-Frames, 0 Panics. fir_bw-Achse ausgereizt (0x63 =
            // Optimum, beide Nachbarn schlechter), Revert auf agc20.
            // Runde 72: PINNUNG spk31 — VERDICT (r72, Teilstand 26 Frames
            // @ HB1400s): REGRESSION, identische Kollaps-Signatur wie
            // fir3 (26 vs agc20 163 @ gleichem Soak-Alter; 0 L62-Frames).
            // slicer_peak_th +8 = Auge zu, revert. Runde 73: PINNUNG
            // spk21 (0x2A 0x29->0x21, Gegenprobe −8, High-Nibble 2
            // beibehalten). Kriterium wie r70-72 + Volumen-Rueckkehr
            // ~640/90min; wenn 0x21 auch kollabiert/neutral -> 0x2A
            // als gepacktes Feld werten (High-Nibble-Modus) und Hebel
            // 0x25 cor_filt-alpha testen.
            // Runde 73: PINNUNG spk21 — VERDICT (r73, Fruehverdict @HB1400s
            // 24 Frames): KOLLAPS wie +8 (r72). 0x2A-Achse TOT (Operating-
            // Point 0x29; 0x29±8 = 0x21/0x31 via Carry/Borrow, beide Proben
            // richtungsblind, Low-Nibble 9 = Working-Point). Revert agc20.
            // Runde 75: PINNUNG cor00 — VERDICT (r75, Fruehverdict @ ~HB870s
            // 14 Frames, 11 BOK/3 BBAD): KOLLAPS identisch fir3/spk31/spk21
            // (0 L62-Frames, nur L55/49). cor_filt_alpha=0 toetet das Kor-
            // rektfilter -> Demod-Auge zu. Revert agc20. Runde 76: PINNUNG
            // cor32 (0x25 0x12->0x32, High-Nibble 1->3 ohne Carry; wenn
            // auch kollabiert/neutral -> Hebel cdr_range 0x2B[3:2], dann
            // L94-Diagnose).
            // Runde 76: PINNUNG cor32 — ABBRUCH auf Julian-Anweisung
            // (7-8 Frames @ ~360s, nur L55, 0 L62 = Kollaps-Pace). cor_
            // filt_alpha ACHSE TOT (0x12 einziger Working-Point). Runde
            // 77: PINNUNG cdrR3 (Julian) — 0x2B cdr_range 2->3 = 0x3D
            // (bitweise verifiziert); danach L94-Diagnose.
            static int8_t svIdx = -1;
            if (svIdx < 0) {
                for (uint8_t i = 0; i < sizeof(svars) / sizeof(svars[0]); i++)
                    // Runde 84: PINNUNG cdrt3 — VERDICT (Soak @HB1400s): keine
                    // Heilung der Chroniker (86913647 13x BBAD, 80913647 12x
                    // BBAD = 0 BOK), Volumen 118 @HB1614 (~62% der agc20-
                    // Referenz 163 @HB1400), 0 Panics, L62-ohne-Chroniker
                    // sauber (X 8.5%). TRACING+avg3 kostet Volumen, heilt
                    // nichts -> CDR-Cluster modexavg VOLLSTÄNDIG getestet
                    // (agc20 counting-avg3 bleibt Working-Point). Revert.
                    // Runde 85c: PINNUNG sync2b — der 2-B-Sync-Hebel gegen
                    // die 54-CD-WALL (Chroniker 0% BOK in Produktion, Probe
                    // 82-88% FULL/PARS mit Map-Sync 0x3C=0x02).
                    if (!strcmp(svars[i].name, "sync2b")) svIdx = i;
            }
            const SyncVar &sv = svars[svIdx];
            radio.goStandby();
            uint8_t p24 = radio.readReg(0x24), p25 = radio.readReg(0x25), p26 = radio.readReg(0x26);
            uint8_t p28 = radio.readReg(0x28), p2B = radio.readReg(0x2B), p2C = radio.readReg(0x2C);
            uint8_t p3C = radio.readReg(0x3C), p46 = radio.readReg(0x46);
            uint8_t p42 = radio.readReg(0x42), p43 = radio.readReg(0x43), p44 = radio.readReg(0x44);
            uint8_t p41 = radio.readReg(0x41);
            uint8_t p32 = radio.readReg(0x32), p30 = radio.readReg(0x30);
            uint8_t p0C = radio.readReg(0x0C), p2F = radio.readReg(0x2F);
            uint8_t p69 = radio.readReg(CMT_REG_CTL1_IO_SEL);
            uint8_t p22 = radio.readReg(0x22);   // Runde 38: flt_bw (bw21-Variante)
            radio.writeReg(0x0C, sv.r0C);    // Analog-Gain (LNA_MODE)
            radio.writeReg(0x2F, sv.r2F);    // AGC en/mask
            radio.writeReg(0x30, sv.r30);    // AGC-LOOP gain/rate (DIE HEBEL!)
            if (sv.r32 != 0xFF) radio.writeReg(0x32, sv.r32);  // Runde 42: agc_target (0xFF=unveraendert)
            radio.writeReg(0x24, sv.r24);    // cor_filt_stage/fir_bw/cic_bw — Runde 71-VERDICT: 0x63 Sweetspot (fir3-Klippe, fir6-Rauschen), alle r72+ Varianten 0x63
            radio.writeReg(0x25, sv.r25);    // cor_filt_alpha 2/1 — r75-VERDICT: 0x00 kollabiert (Auge zu); r76-Hebel cor32=0x32, sonst 0x12 (Map-Default)
            radio.writeReg(0x26, sv.r26);    // slicer_cor_th (Runde 38 aus Tabelle)
            if (sv.r22 != 0xFF) radio.writeReg(0x22, sv.r22);  // flt_bw (0xFF=unveraendert)
            radio.writeReg(0x28, sv.r28);    // AFC aus (0x8F) / an (0x9F)
            radio.writeReg(0x2A, sv.r2A);    // slicer_peak_th — r72+73-VERDICT: 0x29±8 (spk31/spk21) kollabieren BEIDE -> Achse tot, 0x29 Working-Point; alle r75+ Varianten 0x29
            radio.writeReg(0x2B, sv.r2B);    // CDR mode/range/avg — r67 cdavg (0x29->0x39, avg 3); r77 (Julian) cdrR3: range 2->3 (0x3D); r84 cdrt3: bit0 1->0 = TRACING (0x38)
            radio.writeReg(0x2C, 0xC0);      // det1+3rd
            radio.writeReg(0x42, sv.r42);    // sync byte2
            radio.writeReg(0x41, sv.r41);    // sync byte3 (Mode-C-Access-Code-Tail)
            radio.writeReg(0x44, sv.r44);    // sync byte0 (zuerst im Air)
            radio.writeReg(0x43, sv.r43);    // sync byte1
            radio.writeReg(0x3C, sv.r3C);    // bits3:1=SYNC_SIZE (Wert=nbytes-1), bit0=sync-manch
            radio.writeReg(0x46, sv.r46);    // Runde 45b: SYNC_VALUE[55:48] —
                                             // NICHT Payload (Datasheet T21);
                                             // Write griff, Zaehler ignoriert.
            // Runde 46: 0x4A war Fehlgriff — laut Table 21 (300-dpi-Lesung)
            // ist 0x4A = NODE_VALUE[23:16], nicht Payload.
            // Runde 47: 0x45=0x41 (PAYLOAD-LENG-Enable + PKT_TYPE=1) griff
            // (P45=41 live), veraenderte aber nichts.
            // Runde 48: Durchsatz-Fix (SWSPI 12->4 us/Bit, Rahmung 110->50,
            // [FF]-Druck hinter den Drain). Danach bewiesen: der Latch ist
            // NICHT die FIFO-Kapazitaet, sondern L+1 (r48: L=62 -> 63 real
            // 19/19, L=55 -> 56 real 4/4) - PKT_TYPE=1 = Variabellen-Modus,
            // der Handler liest das L-Feld und kennt die Block-CRCs nicht.
            // Runde 49: PKT_TYPE=0 (Festlaengen-Modus, 0x45=0x40) -> der
            // Zaehler nutzt PAYLOAD_LENGTH (0x46=0x49=73). Bewiesen: alle
            // drei Laengen komplett (73/64/109 real), 9 BOK (EN-13757).
            // Runde 50: Rahmung 5/5/15 + [FF]-Druck raus -> BOK-Rate 31%->54%,
            // Firmware-Akzeptanz 80 %. Der Timing-Anteil ist weg.
            // Runde 51: L=94 bleibt 0 BOK, IMMER TTTTFF (Blöcke 5-6 =
            // j84..108, alle Tails frame-einzigartig = kein Stale). Block 4
            // (j66..83, Rest-Read) validiert -> der Chip schreibt weiter,
            // aber PKT_DONE feuerte bei count=73 MID-FRAME und der Packet-
            // Handler resettet danach (naechstes-Packet-Suche) und verhunzt
            // j84..108. FIX: PAYLOAD_LENGTH=109 (0x6D) = das wahre Ende von
            // L=94; L=55/L=62 laufen dann ueber den bewiesenen RX-TMO-Pfad
            // (Runde 45: liefert komplette Rahmen). Der Handler rechnet
            // 'need' aus dem L-Feld - unberuehrt.
            radio.writeReg(0x45, 0x40);
            // Runde 85c: 2-B-Sync (r3C=0x02) -> das Luft-Tail {54 CD} liegt
            // IM Zaehler: PAYLOAD_LENGTH 109+2=111 (0x6F), sonst schneidet
            // PKT_DONE die letzten 2 B der L94-Frames ab.
            radio.writeReg(0x46, (sv.r3C == 0x02) ? 0x6F : 0x6D);
            // FIFO_MERGE (0x69 bit1): mg64 = ein 64-B-FIFO (historisch
            // oracle-bewiesen), fx31 = 2x32-B (BISECT). bit2 (TX/RX-Halb)
            // mitgeklemmt, bit0 (SPI-Richtung) laesst der GoFIFO-Write unten.
            radio.writeReg(CMT_REG_CTL1_IO_SEL,
                           (radio.readReg(CMT_REG_CTL1_IO_SEL) & ~0x06)
                               | (sv.merge ? 0x02 : 0x00));
#if defined(CMT_TMODE_PROBE) && CMT_TMODE_PROBE
            // Runde 83b (2026-09-08) T1-Delta, PRO Zyklus NACH der Varianten-
            // Anwendung (agc20 schreibt 0x24/0x25/0x26/0x30 sonst mit den
            // 100k-Sweetspots zurueck): Data-Rate-Bank 0x20..0x37 komplett
            // aus RFPDK_REG_MAP_T1 (32.8 kbps: CDR_BR_TH 0x2D/0x2E = 0x0318
            // = 792 = 26e6/32800, flt_bw sel 2 @ 0x22, RFPDK-Demod-Cluster).
            // Sync = agc20s 4-B-Wort 54 3D 54 CD (0x44..0x41, 0x3C=0x06 =
            // SYNC_SIZE 4 B) — Klarstellung 83g: agc20.r3C ist 0x06, NICHT
            // 0x02; der TX-Beacon pinnt vor GO_TX voruebergehend 0x3C=0x02
            // und traegt das Air-Tail 54CD im FIFO (Air-Frame = Chip-Sync
            // 543D + 54CD + Telegramm, rtl_433-verifiziert). Runde 83f
            // (2026-09-08): 83e-Wechsel auf 0x2DD4 RUECKGENOMMEN (rtl_wmbus
            // ACCESS_CODE_T1_C1 = 0x543D, 16-Bit, 0 Fehler; 0x2DD4 ist der
            // EBYTE-Werkdefault in beiden Maps, den agc20 ueberschreibt).
            // Runde 83g: Retune auf 868.95 — 868.3 war der S1-Kanal des
            // rtl_wmbus (rtl_sdr @ 868.625M, +325k -> T1/C1 @ 868.95M,
            // -325k -> S1 @ 868.3M); die Flotte (qsmoke SDR-bewiesen,
            // qcaloric/qwater, LSE-Bridges) laeuft via Daemon auf 868.95.
            // Der TX-Beacon ist Positivkontrolle: dekodiert der Daemon
            // unser 32.8-kBd-Beacon (lse_01234567) wie das 100-kBd-
            // Produktiv-Beacon, ist T1@868.95 end-to-end bewiesen.
            // Manchester bleibt AUS (0x4F=0x60): T1-3of6 ist roh-NRZ
            // (DC-balanciert).
            // Runde 83h (2026-09-08) B1-Zweig (CMT_PROBE_SYNC2B): Produktion-
            // Bank (100 kBd) + 2-Byte-Sync-Override (0x44=0x54/0x43=0x3D,
            // 0x3C=0x02). Hypothese (iii): die Flotte hat T1-3of6-Frames
            // @ 100 kBd @ 868.95. BEWEIS der Sync-Blockade: Byte 3 der
            // Produktiv-4B-Sync 543D-54CD = 0x54 = 01010100b -> das erste
            // 3of6-Symbol nach 543D waere 010101 = 0x15, und HIGH_NIBBLE_
            // 3OUTOF6[0x15] = LOW_NIBBLE_3OUTOF6[0x15] = 0xFF (t1_c1_packet_
            // decoder.h:50/59) — kein gueltiges 3of6-Symbol in JEDEM Nibble-
            // Alignment (Shifts 0..3: 0x15/0x2A/0x14/0x28, alle 0xFF).
            // D.h. die 4B-Sync schliesst ALLE T1-Frames aus — Produktivstand
            // UND 83g-Soak konnten sie prinzipbedingt nie ins FIFO lassen;
            // der 83i-3of6-Resweep (0 Treffer) war fuer T1 uninformativ.
            // Asymmetrie bleibt: qcaloric (M=0x3265) sendet ~5x/min (Journal
            // 06:34:40..06:39:41), der Chip fing im selben Fenster 22
            // [wM-C O]-Frames anderer Kohorten und 0 qcaloric.
            // B1 laesst T1-Frames ins FIFO; der Offline-3of6-Dekod (scratches/
            // r83i_3of6_resweep.py, CRC1-Konvention verifiziert) entscheidet.
            // Runde 83j (2026-09-08): Bank- und Sync-Hebel jetzt ORTHOGONAL
            // (CMT_PROBE_BANK_T1 / CMT_PROBE_SYNC2B) — B2 (T1-32.8-Bank +
            // 2-B-Sync) wird ohne Struktur-Aenderung buildbar. Matrix:
            //   keine Flags          = 83g-Default (T1-Bank + agc20 4B-Sync)
            //   SYNC2B               = B1 (Prod-Bank 100 kBd + 2-B-Sync 543D)
            //   BANK_T1+SYNC2B       = B2 (T1-Bank 32.8 kBd + 2-B-Sync 543D)
            //   S1                   = Probe A: Prod-Bank mit CDR_BR_TH 260->
            //                          397 (CDR-Mitte 65.536 kBd), Sync-Wort
            //                          33 30 3F 3C = bit-gedoppeltes 0x547696
            //                          (Python-Verifikation 83h), 868.3 MHz
            //                          (S1-Kanal, rtl_wmbus.c:1326 -325k).
            // Prod-Bank-Basis fuer S1: breites Flt-BW (0x22=0x73) passt zu
            // 65.5 kBd Manchester-Luft; Flotten-C-Frames bleiben sichtbar.
            // Manchester AUS (0x4F=0x60) — der Offline-Dekod
            // (scratches/r83j_s1_decode.py) entscheidet; Slip-Sweep noetig,
            // da SYNC_SIZE 4 B von den 6 gedoppelten Bytes matcht (FIFO
            // startet 16 Half-Bits vor dem Payload-Anfang).
#if defined(CMT_PROBE_S1) && CMT_PROBE_S1
            radio.writeReg(0x2D, 0x8D);   // CDR_BR_TH = 397 = 26e6/65536
            radio.writeReg(0x2E, 0x01);   //   (65.536 kBd, 0.068 % Abstand)
            radio.writeReg(0x44, 0x33);   // Sync-Wort S1 = 33 30 3F 3C
            radio.writeReg(0x43, 0x30);   //   = bit-gedoppeltes 0x547696
            radio.writeReg(0x42, 0x3F);
            radio.writeReg(0x41, 0x3C);
            radio.writeReg(0x3C, 0x06);   // SYNC_SIZE = 4 B ((0x06>>1)&7)+1
            radio.setFrequencyHz(868300000UL);   // S1-Kanal (-325 kHz)
#else
#if defined(CMT_PROBE_BANK_T1) && CMT_PROBE_BANK_T1
            for (uint8_t a = 0; a < 24; a++)
                radio.writeReg(0x20 + a, pgm_read_byte(&kT1DataRate[a]));
#endif
#if defined(CMT_PROBE_SYNC2B) && CMT_PROBE_SYNC2B
            radio.writeReg(0x44, 0x54);   // Sync-Wort B1/B2 = 543D, 2 Byte
            radio.writeReg(0x43, 0x3D);
            radio.writeReg(0x3C, 0x02);   // SYNC_SIZE = 2 B ((0x02>>1)&7)+1
#endif
            radio.setFrequencyHz(868950000UL);
#endif
            static bool t1Once = false;
            if (!t1Once) {
                t1Once = true;
                Serial.printf("[TPROBE] T1 DR-Bank 0x2D/0x2E=%02X/%02X 0x22=%02X 0x3C=%02X SYNC=%02X%02X PLL:",
                              radio.readReg(0x2D), radio.readReg(0x2E),
                              radio.readReg(0x22), radio.readReg(0x3C),
                              radio.readReg(0x44), radio.readReg(0x43));
                for (uint8_t a = 0x18; a <= 0x1F; a++) Serial.printf(" %02X", radio.readReg(a));
                Serial.println();
            }
#else
            radio.setFrequencyHz(868950000UL);
#endif
#if defined(CMT_PROBE_RSSILOG) && CMT_PROBE_RSSILOG
            // Runde 83k Probe C: RSSI-Burst-Recorder @ 868.95 — rate-agnostisch
            // (0x6F = Energie VOR CDR/Sync, der Sync-Matcher blockt fremde
            // Raten prinzipbedingt). Datenbasis 83h/83g: Noise-Dumps <=48
            // (RSSI-SPIKE-Max je Fenster 44..49), echte Flotten-Frames
            // RSSI 50..155 (Median ~123) -> Threshold 52 trennt. Endlos-Poll
            // OHNE delay (SPI ~25-40 us -> ~15-30 kSa/s), Ausgabe NUR am
            // Burst-Ende ([BURST] t/dur/peak) + 5-s-Noise-Floor-Anker — kein
            // UART-Backpressure-Gap. Flotten-Kohorten (100 kBd, L=55/62/94)
            // kalibrieren Preamble+AGC-Ramp; qcaloric-L=0x31 = Rest-Cluster.
            // SWFRAM/Beacon/HB werden vom Endlos-Loop blockiert — keine TX-
            // Verschmutzung (das 32.8-kBd-Beacon waere ein kollidierendes
            // 15-ms-Cluster).
            // Runde 83n Probe D (2026-09-09): Envelope + SYNC_OK-Zahl +
            // FIFO-Capture je Burst. Frage: sync't der Chip die LSE-Outer-
            // Frames (C=0xC4, Envelope 52-59, 2/7 decaps-Momente >= 52)
            // oder blockt der Sync strukturell (Preamble/CDR)? Beweisstand
            // 83m: 3694 Dumps, 0 C=0xC4/CRC1-OK an irgendeinem Alignment —
            // die Outer-Frame-Bytes waren NIE im FIFO. Je Burst: hi=0x6D
            // (SYNC_OK=bit3 gezaehlt+cleared, PKT_DONE=bit0 cleared,
            // Streaming-Flow), ff=0x6E-Maske 0x33 -> room=min(128-capLen,32)
            // -> readFifo -> capBuf (r83c-Caps, kein need0-L-Feld-Lookahead:
            // Capture = roh, Analyse sucht [54 CD/3D][4x][C4][65 32] frei).
            // Nach Burst-Ende: [BURST] + [BRAW] (capBuf roh) + FIFO clear
            // (Chip-Pointer/Counter reset, naechster Burst = clean capture).
            radio.goStandby();
            radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);  // RX FIFO clear
            radio.goRx();
            {
                const int THR = 52;
                bool inBurst = false;
                uint32_t burstT0 = 0, tQuiet = 0;
                int burstPeak = 0, burstFirst = 0; uint32_t burstN = 0;
                int flMin = 999, flMax = -1; uint32_t flT0 = millis();
                uint8_t capBuf[128]; uint16_t capLen = 0; uint16_t capSync = 0;
                bool capActive = false;
                uint8_t rbuf[32];
#if defined(CMT_PROBE_SYNCCAP) && CMT_PROBE_SYNCCAP
                // Runde 83p Probe E: SYNC-getriggerte Capture OHNE RSSI-Gate.
                // Probe D (83n) bewies: 41/41 Envelope-Bursts sync=1, aber 0/5
                // Outer-Momente im FIFO -> Outers unter der THR=52-Envelope.
                // Der Chip-FIFO fuellt sich aber schon beim CDR-Lock (FIFO
                // startet NACH dem Sync mit [54 CD][L][C]...), unabhaengig
                // vom RSSI. Also: SYNC_OK (0x6D bit3) kontinuierlich pollen;
                // ausserhalb eines Envelope-Bursts -> room-capped Drain bis
                // 8 ms nach dem letzten Nicht-leer-Flag, 1x blindes Enddrain
                // (TH feuert erst ab Level ~27), dann [SCAP]/[SRAW] + FIFO
                // reset. scapLen==0 nach 8 ms = Noise-Sync (kein Enddrain).
                // Kollision mit einem echten Envelope-Burst: SCAP wird still
                // verworfen (kein UART-Delay am Burst-Start).
                uint8_t scapBuf[128]; uint16_t scapLen = 0; uint16_t scapSync = 0;
                bool scapActive = false, scapEnddrain = false;
                uint32_t scapT0 = 0, scapLast = 0; int scapPeak = 0;
                uint32_t scapN20 = 0;   // 83q: Envelope-Samples >20 im Fenster
#endif
#if defined(CMT_PROBE_OFS) && CMT_PROBE_OFS
                // Runde 83q Probe F: Frequenz-Offset-Zyklus. Probe E bewies
                // (769 BRAW, 93 SCAP 93x cap=0, harter Byte-Match-Anker
                // 16:03:04, 0 Outer-Signaturen): der Chip demoduliert die
                // LSE-Outers NIRGENDWO — Sync feuert nur auf Noise (peak
                // 24-41, nicht an decaps angereichert). Outers sind ~25-30 dB
                // unter qsmoke; wenn ihr Traeger gegenueber unserer 26-MHz-
                // Referenz 20-40 kHz off sitzt, drueckt das den narrowband
                // Envelope unter THR (qsmoke schafft die Marge) und der CDR
                // lockt nie. 5 Offsets x 60 s, voller PLL-Retune inkl.
                // Relock; [ORET] = Timeline fuer die Analyse-Join. Start bei
                // Delta=0 (Positivkontrolle qsmoke), dann +15/-15/+30/-30 kHz.
                static const int32_t OFS[5] = {0, 15000, -15000, 30000, -30000};
                uint8_t oIdx = 0; uint32_t oT0 = millis();
#endif
                while (true) {
                    int f = (uint8_t)radio.readReg(0x6F);
                    uint32_t now = millis();
#if defined(CMT_PROBE_OFS) && CMT_PROBE_OFS
                    if (now - oT0 >= 60000) {
                        oT0 = now;
                        oIdx = (oIdx + 1) % 5;
                        scapActive = false;   // laufenden Noise-Sync verwerfen
                        uint32_t fO = (uint32_t)(868950000L + OFS[oIdx]);
                        bool okO = radio.setFrequencyHz(fO);   // -> Standby
                        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
                        radio.goRx();
                        Serial.printf("[ORET] t=%lu o=%+d ok=%u\n",
                                      (unsigned long)now, (int)OFS[oIdx], (unsigned)okO);
                    }
#endif
#if defined(CMT_PROBE_SYNCCAP) && CMT_PROBE_SYNCCAP
                    if (!inBurst && !scapActive) {
                        uint8_t hi2 = radio.readReg(0x6D);
                        if (hi2 & 0x08) {  // SYNC_OK ausserhalb des Envelope-Gates
                            radio.clearIntFlagHi(0x08);
                            scapActive = true; scapEnddrain = false;
                            scapT0 = now; scapLast = now; scapPeak = f;
                            scapLen = 0; scapN20 = 0;  // FIFO NICHT clear: Bytes kommen ab Sync
                        }
                    }
                    if (scapActive) {
                        if (f > scapPeak) scapPeak = f;
                        if (f > 20) scapN20++;
                        uint8_t ff2 = radio.readReg(0x6E);
                        if (ff2 & 0x33) {
                            if (scapLen < sizeof(scapBuf)) {
                                uint16_t room2 = (uint16_t)(sizeof(scapBuf) - scapLen);
                                if (room2 > 32) room2 = 32;
                                radio.readFifo(rbuf, room2);
                                memcpy(scapBuf + scapLen, rbuf, room2);
                                scapLen += room2;
                            }
                            scapLast = now;
                        }
                        if (now - scapLast > 8) {
                            if (scapLen == 0) {
                                // Noise-Sync (echte Frames streamen <1 ms nach
                                // dem Sync in den FIFO) — kein Enddrain.
                                Serial.printf("[SCAP] t=%lu peak=%d n20=%lu sync=1 cap=0\n",
                                              (unsigned long)scapT0, scapPeak,
                                              (unsigned long)scapN20);
                                scapActive = false;
                                radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
                            } else if (!scapEnddrain) {
                                scapEnddrain = true;
                                if (scapLen < sizeof(scapBuf)) {
                                    uint16_t room2 = (uint16_t)(sizeof(scapBuf) - scapLen);
                                    if (room2 > 32) room2 = 32;
                                    radio.readFifo(rbuf, room2);
                                    memcpy(scapBuf + scapLen, rbuf, room2);
                                    scapLen += room2;
                                }
                            } else {
                                Serial.printf("[SCAP] t=%lu peak=%d n20=%lu sync=%u cap=%u\n",
                                              (unsigned long)scapT0, scapPeak,
                                              (unsigned long)scapN20,
                                              (unsigned)1, (unsigned)scapLen);
                                Serial.printf("[SRAW] t=%lu sync=%u cap=%u:",
                                              (unsigned long)scapT0, (unsigned)1,
                                              (unsigned)scapLen);
                                for (uint16_t i = 0; i < scapLen; i++)
                                    Serial.printf(" %02X", scapBuf[i]);
                                Serial.println();
                                scapActive = false;
                                radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
                            }
                        }
                    }
#endif
                    if (f > THR) {
                        if (!inBurst) {
                            inBurst = true; burstT0 = now;
                            burstPeak = f; burstFirst = f; burstN = 1;
#if defined(CMT_PROBE_SYNCCAP) && CMT_PROBE_SYNCCAP
                            // 83p: laeuft noch ein SYNC-Trigger-Capture, wird
                            // es still verworfen (kein UART-Delay am Burst-
                            // Start; der Burst-Start-Clear wuerde die Bytes
                            // sowieso vernichten). Positivkontrolle bleibt.
                            scapActive = false;
#endif
                            // Clean capture: Chip-FIFO/Counter reset am Burst-
                            // Start (der Chip haette sonst Rausch-Restlevel
                            // aus der Pause in den Burst gezogen).
                            radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
                            capLen = 0; capSync = 0; capActive = true;
                        }
                        else { if (f > burstPeak) burstPeak = f; burstN++; }
                        tQuiet = now;
                        uint8_t hi = radio.readReg(0x6D);
                        if (hi & 0x08) { radio.clearIntFlagHi(0x08); capSync++; }  // SYNC_OK
                        if (hi & 0x01) radio.clearIntFlagHi(0x01);                 // PKT_DONE (streaming)
                    } else if (inBurst && now - tQuiet > 8) {
                        // Enddrain: das TH-Flag feuert erst bei Level >= ~27
                        // (r43-Mock) — der Frame-Rest (Level < 27) wuerde im
                        // Chip-FIFO stehen bleiben. Blind ziehen (room-capped,
                        // r83c-Grenzen); Anhaenge = Rauschen, im capBuf immer
                        // HINTER den Frame-Bytes (Offset-Scan tolerant).
                        if (capLen < sizeof(capBuf)) {
                            uint16_t room = (uint16_t)(sizeof(capBuf) - capLen);
                            if (room > 32) room = 32;
                            radio.readFifo(rbuf, room);
                            memcpy(capBuf + capLen, rbuf, room);
                            capLen += room;
                        }
                        Serial.printf("[BURST] t=%lu dur=%lu peak=%d first=%d n=%lu sync=%u cap=%u\n",
                                      (unsigned long)burstT0, (unsigned long)(tQuiet - burstT0),
                                      burstPeak, burstFirst, burstN,
                                      (unsigned)capSync, (unsigned)capLen);
                        if (capLen) {
                            Serial.printf("[BRAW] t=%lu sync=%u cap=%u:", (unsigned long)burstT0,
                                          (unsigned)capSync, (unsigned)capLen);
                            for (uint16_t i = 0; i < capLen; i++) Serial.printf(" %02X", capBuf[i]);
                            Serial.println();
                        }
                        inBurst = false; capActive = false;
                        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);  // FIFO reset fuer naechsten Burst
                    }
                    // R83n-Fix (Mock-Befund): Drain NUR im f>THR-Zweig ließ
                    // die Frame-Tail-Bytes stehen (nach dem RSSI-Abfall liegt
                    // der Frame-Rest bis ~30 B noch im Chip-FIFO, TH feuert
                    // erst wieder bei Level 27). Drain laeuft deshalb im
                    // Burst UND im 8-ms-Quiet-Fenster bis zum Print.
                    if (capActive && capLen < sizeof(capBuf)) {
                        uint8_t ff = radio.readReg(0x6E);
                        if (ff & 0x33) {
                            uint16_t room = (uint16_t)(sizeof(capBuf) - capLen);
                            if (room > 32) room = 32;
                            radio.readFifo(rbuf, room);
                            memcpy(capBuf + capLen, rbuf, room);
                            capLen += room;
                        }
                    }
                    if (f < flMin) flMin = f;
                    if (f > flMax) flMax = f;
                    if (now - flT0 >= 5000) {
                        Serial.printf("[FLOOR] t=%lu min=%d max=%d\n",
                                      (unsigned long)flT0, flMin, flMax);
                        flMin = 999; flMax = -1; flT0 = now;
                    }
                }
            }
#endif
            // STREAMING-FLOW: EINMAL armen, nach PKT_DONE OHNE goStandby/goRfs
            // weitermachen (PLL bleibt gelockt, AN192: der Chip verlaesst RX
            // nicht selbst). Nur FIFO clear + GoRx-Befehl = minimal gap.
            // Runde 33: E49x_GoReceive EXAKT (der bewiesene rxPacket-Fix):
            // GoFIFO(READ) VOR dem ClearFIFO — der TX-Beacon (writeFifo)
            // hinterlaesst 0x69 im FIFO-WRITE-Modus -> der Demod kann in
            // kein einziges Fenster-Byte schreiben -> PKT_DONE mit leerem
            // FIFO = die 00-Frames (RAW-Dump-Beweis in long56!).
            radio.writeReg(CMT_REG_CTL1_IO_SEL, radio.readReg(CMT_REG_CTL1_IO_SEL) & ~0x05);
            radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
            radio.goRfs();
            radio.goRx();
            uint32_t tw = millis();
            // Runde 50: nDrain = Mid-Burst-FIFO-Drains (still gezahlt, im
            // [SWFRAM] gemeldet - der [FF]-Serial.printf ist aus dem
            // Burst-Pfad raus: ~30 Zeichen @115200 blockieren ~2.6 ms,
            // mehr als der ganze 730-us-Burst; er verschob Drain 2 und
            // den PKT_DONE-Parse. Verdacht: Tail-Block-Fails = Timing,
            // nicht Luft. Diskriminator: bleibt das Fail-Muster byte-
            // identisch -> Luft-Demod (AGC/CDR); kollabiert es -> Race.
            uint32_t nFrames = 0, nCrcOk = 0, nSync = 0, nBurstWin = 0, nFalse = 0, nB2ok = 0, nAsm = 0, nDrain = 0,
                     nDrainMark = 0,   // Runde 52: nDrainMark = Drains vor dem letzten PKT_DONE (je-Frame-Delta)
                     nRepTry = 0, nRepOk = 0, nRepFlip = 0, nRepIns = 0, nRepDrop = 0,  // Runde 80: Slip-Repair
                     nAnnexOk = 0;   // Runde 83y: EIE-Annex-Akzeptanz (ei6500)
            int rssiHi = -999;
            uint8_t buf[64];
            uint8_t asmBuf[128]; uint16_t asmLen = 0; bool asmActive = false;  // Runde 43
            static uint8_t rssiHist[48]; static uint8_t rssiIdx = 0;
            memset(rssiHist, 0, sizeof(rssiHist)); rssiIdx = 0;
            while (millis() - tw < 25000) {
                int f6 = (uint8_t)radio.readReg(0x6F);
                rssiHist[rssiIdx++ & 47] = (uint8_t)f6;
                if (f6 > rssiHi) rssiHi = f6;
                if (f6 > 60) nBurstWin++;          // Burst-Präsenz (ms-Samples)
                uint8_t hi = radio.readReg(0x6D);
                if (hi & 0x08) { radio.clearIntFlagHi(0x08); nSync++; asmLen = 0; asmActive = true; }  // SYNC_OK
                // Runde 43: Mid-Packet-FIFO-Drain — nach SYNC_OK den Chip-
                // Inhalt in asmBuf ziehen (TH-gated). Mock: peak 27 <= 64
                // verlustfrei fuer L=55/62.
                // Runde 52: Cap 64 -> 96 (3 Drains). Der Runde-43-Schluss
                // "der Chip-Inhalt kann nie >64 B sein" ist tot: L=94
                // verliert im Rest-Read 25 von 45 Bytes (junk, kein Ring-
                // Stale). Kandidat: der Chip stopped/dropped beim Level 64,
                // falls Drain 2 zu spaet kommt. Drain 3 haelt das Level
                // niedrig; asmBuf[128] reicht (96+13=109). Das FF-Flag feuert
                // nur bei Level>=Schwelle (Beweis: Block 1-3 validieren,
                // also kommen die Drains nicht zu frueh).
                if (asmActive && !(hi & 0x01) && asmLen < 96) {
                    uint8_t ff = radio.readReg(0x6E);   // FF-Kandidat (Probe fixt die Maske)
                    // Runde 48: Druckzeile NUR beim Drain-Ereignis und NACH dem
                    // Drain. Vorher kostete je gated Iteration 3 Extra-Reads
                    // (0x6C/0x6A/0x71) + ~70 UART-Zeichen > 1.4 ms - mehr als
                    // der ganze 730-us-Burst; der Drain kam zu spaet, der FIFO
                    // lief bei Byte 64 voll (der PKT_DONE-Latch).
                    // Runde 43b: Maske 0x33 (FF-Kandidat je Burst) statt 0xC0 —
           // 0xC0 feuerte nur beim FIFO-FULL (gleiches Instant, in dem
           // der Chip bereits dropped). 0x33 = TH/Not-Empty-Kandidat —
           // Mock (pacing2): peak 50, lost 0 verlustfrei.
           if (ff & 0x33) {
                        // Runde 56: nie ueber den Telegramm-Bedarf hinaus lesen.
                        // Beweis r55 (36/69 Frames byte-gleich zu rtl_433): die
                        // echten Bytes 0..need-1 sind kontiguierlich, DAHINTER
                        // Rausch-Muell - der TH-Drain nach dem Telegramm-Ende
                        // zieht Rausch-Bytes in den FIFO und blind-32 kopiert
                        // sie in asmBuf (Tail bis 96 B; bei L=94 lag die Luecke
                        // mitten im Frame). need aus dem L-Feld (EN 13757-4:
                        // 12+(L-9)+2*ceil((L-9)/16)), room kappt genau dort.
                        // Runde 85c (2026-09-12): 2-B-Sync -> der Chip
                        // schreibt das Luft-Tail {54 CD} (rtl_433-bewiesen)
                        // VOR das Telegramm in den FIFO. L-Feld sitzt dann
                        // bei asmBuf[2]; Bedarf = Telegramm + 2.
                        uint8_t L0 = 0;
                        uint16_t need0;
                        if (asmLen >= 3 && asmBuf[0] == 0x54 && asmBuf[1] == 0xCD &&
                            asmBuf[2] >= 0x0A && asmBuf[2] <= 0xF0) {
                            L0 = asmBuf[2];
                            need0 = (uint16_t)(2 + 12 + (L0 - 9) + 2 * ((L0 - 9 + 15) / 16));
                        } else {
                            L0 = (asmLen >= 1 && asmBuf[0] >= 0x0A && asmBuf[0] <= 0xF0) ? asmBuf[0] : 0;
                            need0 = L0 ? (uint16_t)(12 + (L0 - 9) + 2 * ((L0 - 9 + 15) / 16)) : 96;
                        }
                        // Runde 83c (2026-09-08): need0 auf sizeof(asmBuf)
                        // deckeln — Garbage-L (Rausch-Frame; der Range-Check
                        // 0x0A..0xF0 laesst 56 % aller Zufallsbytes durch) trieb
                        // need bis 273, am PKT_DONE hob rem asmLen auf need
                        // (>128) und das fx31-memset lief mit unsigned-wrap-
                        // Size (128-129 -> 0xFFFFFFFF) ueber den loopTask-
                        // Stack: Crash-Loop (IDLE0-Canary, Core 1 Store/
                        // LoadProhibited) in 8/8 Boots der T1-Probe. Echte
                        // Frames L<=94 -> need<=109, nie betroffen — latent
                        // auch in Produktion.
                        if (need0 > sizeof(asmBuf)) need0 = sizeof(asmBuf);
                        uint16_t room = (need0 > asmLen) ? (uint16_t)(need0 - asmLen) : 0;
                        if (room > 32) room = 32;
                        if (room) {
                            radio.readFifo(buf, room);
                            memcpy(asmBuf + asmLen, buf, room);
                            asmLen += room;
                            nDrain++;   // Runde 50: still (kein printf im Burst-Pfad)
                        }
                    }
                }
                // Runde 45: RX-TMO-Parse — die L=55-Minderheit (41/246 CRC-OK
                // telegrams live) erreicht Payload-Count 73 nie; der Chip
                // laesst RX_TMO (0x6A bit4, cmt2300a.h:82 EBYTE-Layout) am
                // Timeout und der Rahmen ist trotzdem komplett: 64 Luft-Bytes
                // = 12+46+2*3 (EN 13757-4 12.5.3). Maske feuert nie -> kein
                // Verhalten-Change fuer die PKT_DONE-Mehrheit.
                uint8_t lo = radio.readReg(0x6A);
                bool pktDone = (hi & 0x01);
                bool rxTmo = !pktDone && asmActive && (lo & 0x10);
                if (pktDone || rxTmo) {                // PKT_DONE | RX-TMO
                    if (pktDone) radio.clearIntFlagHi(0x01);
                    else radio.clearIntFlagLo(0x10);
                    // Runde 52: Level-Probe am Paket-Ende (nur Dump-Varianten,
                    // vor dem Rest-Read): 0x70/0x71 = FIFO-Zaehler-Kandidaten,
                    // 0x6E = FF-Flag. Zu erklaeren: L=94 liefert im Rest-Read
                    // nur ~20 echte Bytes (j64..83, Block 4 OK), j84..108
                    // Junk - und der Junk ist weder Ring-Stale (0/17 im
                    // Alignmentsweep ueber r49+r51) noch timing-erklaerbar
                    // (80 us/B Luft vs 62 us/B Drain - der FIFO kann
                    // timing-halber nie voll laufen). Fragen: Wie viel liegt
                    // IM FIFO am PKT_DONE?
                    // Wie viele Drains liefen je Frame? Und bleibt nach dem
                    // Rest-Read ein Reststand (Clear-Timer mid-Read)?
                    uint8_t lvl70 = 0, lvl71 = 0, ffd = 0;
                    if (sv.dump) {
                        lvl70 = radio.readReg(0x70);
                        lvl71 = radio.readReg(0x71);
                        ffd = radio.readReg(0x6E);
                    }
                    uint32_t drainsF = nDrain - nDrainMark; nDrainMark = nDrain;
                    // Runde 43: Rest am PKT_DONE aus dem L-Feld ableiten
                    // (EN 13757-4 12.5.3: Rahmen = 12+(L-9)+2*nblk,
                    // nblk=(L-9+15)/16; Mock: BOK fuer 73 B verlustfrei).
                    // Maske feuert nie -> Fail-safe: rem = 64 = alter 64-B-Flow.
                    // Runde 85c: {54 CD}-Praefix (2-B-Sync) -> L-Feld bei
                    // asmBuf[2], Bedarf +2 (Tail liegt IM Zaehler 0x46).
                    uint8_t L = 0;
                    uint16_t need;
                    if (asmLen >= 14 && asmBuf[0] == 0x54 && asmBuf[1] == 0xCD &&
                        asmBuf[2] >= 0x0A && asmBuf[2] <= 0xF0) {
                        L = asmBuf[2];
                        need = (uint16_t)(2 + 12 + (L - 9) + 2 * ((L - 9 + 15) / 16));
                    } else {
                        L = (asmLen >= 12 && asmBuf[0] >= 0x0A && asmBuf[0] <= 0xF0) ? asmBuf[0] : 0;
                        need = L ? (uint16_t)(12 + (L - 9) + 2 * ((L - 9 + 15) / 16)) : sv.r46;
                    }
                    if (need > sizeof(asmBuf)) need = sizeof(asmBuf);  // Runde 83c: Garbage-L-Cap
                    uint16_t rem = (need > asmLen && need - asmLen <= 64) ? (uint16_t)(need - asmLen) : 0;
                    if (rem) { radio.readFifo(buf, rem); memcpy(asmBuf + asmLen, buf, rem); asmLen += rem; }
                    // Runde 83c: Guard — bei asmLen==128 ist sizeof-asmLen
                    // unsigned-negativ; alt -> memset mit ~2^64.
                    if (asmLen < sizeof(asmBuf))
                        memset(asmBuf + asmLen, 0, sizeof(asmBuf) - asmLen);  // fx31: kein Stale im Merge
                    uint8_t pktLen = (uint8_t)asmLen;
                    const uint8_t *frame = asmBuf;
                    asmActive = false;
                    nFrames++;
                    if (sv.dump) {   // Runde 33: RAW-Sichtbarkeit auch ohne CRC-Treffer
                        // Runde 45b: Register-Readback je Frame — griff die
                        // Payload-Schreibung (0x46=0x49)? Datasheet Table 21:
                        // 0x4A = PAYLOAD_LENGTH[6:0], 0x49 = CUS_PKT15
                        // (PAYLOAD LENG in den High-Bits). Wenn P46==0x49 liest,
                        // griff die Schreibung und 0x46 ist nicht der Zaehler.
                        // Runde 85c: Sync-Readback je Frame — Runtime-Verifikation
                        // des 2-B-Sync-Hebels (S41..S44 = Sync-Wort-Register,
                        // S3C = SYNC_SIZE (0x02 = 2 B); Erwartung sync2b:
                        // S3C=02, S44=54 S43=3D, [RAW] startet mit 54 CD).
                        Serial.printf("[RAW %s] f=%lu rssi=%d D=%c P45=%02X P46=%02X P49=%02X P4A=%02X "
                                      "S41=%02X S42=%02X S43=%02X S44=%02X S3C=%02X "
                                      "L70=%02X L71=%02X L71b=%02X FF=%02X dr=%lu:",
                                      sv.name, (unsigned long)nFrames, f6,
                                      pktDone ? 'K' : 'T',
                                      radio.readReg(0x45), radio.readReg(0x46),
                                      radio.readReg(0x49), radio.readReg(0x4A),
                                      radio.readReg(0x41), radio.readReg(0x42),
                                      radio.readReg(0x43), radio.readReg(0x44),
                                      radio.readReg(0x3C),
                                      lvl70, lvl71, radio.readReg(0x71), ffd,
                                      (unsigned long)drainsF);
                        for (uint8_t j = 0; j < pktLen; j++) Serial.printf(" %02X", frame[j]);
                        Serial.println();
                    }
                    radio.writeReg(CMT_REG_CTL1_IO_SEL,
                                   radio.readReg(CMT_REG_CTL1_IO_SEL) & ~0x05);  // GoFIFO READ
                    radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);  // RX-FIFO clear
                    // Runde 85a (2026-09-12): per-frame GoRx bei PKT_DONE
                    // ENTFERNT. Der Probe-Pfad (r83q-BRAW) beweist den
                    // Chip-Zustand RX-ohne-GoRx als funktionierend:
                    // 86913647 82% FULL / 80913647 88% FULL im BRAW gegen
                    // 0% BOK in Produktion (Kontroll-Soak r84_revert heute:
                    // 0/10 und 0/14, gesund 919 18/18). GoRx aus RX raus =
                    // Mode-Bounce (CDR/AGC/PLL-Reset zwischen den Frames);
                    // AN192/r33: der Chip verlaesst RX nicht selbst, und der
                    // Probe-Pfad haelt RX dauerhaft ohne einen einzigen GoRx.
                    // GoRx bleibt nur im RX-TMO-Fall (Chip-State dort
                    // unbelegt in der Evidenz).
                    if (!pktDone)
                        radio.writeReg(CMT_REG_CTL1_MODE, CMT_GO_RX);  // GoRx (PLL gelockt)
                    // CRC-Scan: wM-Bus EN-13757-Blockstruktur (Runde 34/35,
                    // hardware-bewiesen): B1 = j0..9 + CRC1 (der Filter oben
                    // hat CRC1 schon validiert), dann je 16 Datenbytes + CRC,
                    // letzter Block beliebig kurz. L zaehlt die Bytes NACH dem
                    // L-Feld ohne CRCs (EN 13757-4 12.5.3; rtl_433 m_bus.c:
                    // num_data_blocks=(L-9+15)/16). Frame = 12+(L-9)+2*nblk;
                    // L=55 -> 64 B, CRC4 bei j62-63. 4-Byte-Sync => Frame bei
                    // buf[i] (i=0), CRC4 im 64-B-FIFO erreichbar.
                    for (uint8_t i = 0; i + 11 < pktLen; i++) {
                        if (frame[i] < 0x0A || frame[i] > 0xF0) continue;
                        uint16_t c = wmbusCrc(frame + i, 10);
                        uint16_t rx = (uint16_t)(frame[i + 10] << 8) | frame[i + 11];
                        if (c != rx) continue;
                        const uint8_t *t = frame + i;
                        if (t[1] != 0x44) { nFalse++; continue; }  // nur SND_NR
                        nCrcOk++;
                        uint8_t L = t[0];
                        // Block-Walk: B2=j12..27+CRC, B3=j30..45+CRC, ... bis
                        // L-9 Bytes (nach Header) abgearbeitet sind (inkl.
                        // CRC4@j62-63 bei L=55). Token: BOK = alle Block-CRCs.
                        const char *b2 = "B-";
                        bool b2ok = false;
                        bool annex = false;                 // Runde 83y: EIE-Annex (BOKA)
                        uint16_t failPos = 0, failRem = 0;   // Runde 80: erster FAIL-Block
                        if (L >= 9) {
                            uint16_t pos = 12, rem = L - 9;
                            b2ok = true;
                            while (rem) {
                                uint8_t blk = rem > 16 ? 16 : (uint8_t)rem;
                                if ((uint16_t)(i + pos + blk + 2) > pktLen) { b2 = "B?"; b2ok = false; break; }
                                uint16_t cb = wmbusCrc(frame + i + pos, blk);
                                uint16_t rb = (uint16_t)(frame[i + pos + blk] << 8) | frame[i + pos + blk + 1];
                                if (cb != rb) {
                                    b2 = "BBAD"; b2ok = false;
                                    failPos = pos; failRem = rem;   // alle Block-CRCs davor passierten
                                    break;
                                }
                                pos += blk + 2; rem -= blk;
                            }
                            if (b2ok) b2 = "BOK";
                        }
                        // Runde 83y (2026-09-11): EIE-Annex-Signatur (ei6500,
                        // M=0x2515, L=94): der Meter CRC-t nur die ersten 4
                        // Payload-Bloecke (B2..B5 = 64 B); dahinter liegt die
                        // AES-CBC-TPL (CFG 2550 = synchron AES_CBC_IV nb=5:
                        // 80 B verschluesselt + 4 B klar, offline am
                        // wmbusmeters-TPL-Parser bewiesen, Log/r83y_crcless.txt).
                        // Der 25-B-Tail hat BY DESIGN keine EN-13757-Block-CRCs
                        // -> Walk-Signatur TTTTFF = Design-Signatur, kein
                        // Korruptionsmuster. Kriterium: erster FAIL-Block
                        // >= 12+4*18 = 84 (4 CRC-te Bloecke davor) und Tail
                        // >= 20 B und L==94/M=0x2515. qsmoke (L<=62) erreicht
                        // strukturell max. 66 -> feuert nie (r83y_rule_quant:
                        // 0 False-Fire auf 984 BBADs). Feed = CRC-less-Prefix
                        // + roher Tail, L-Byte = len-1 (0x62, warn-frei).
                        if (!b2ok && failPos >= 84 && L == 94 &&
                            t[2] == 0x25 && t[3] == 0x15 &&
                            (uint16_t)(pktLen - (uint16_t)(i + failPos)) >= 20) {
                            b2 = "BOKA"; b2ok = true; annex = true;
                            nAnnexOk++;
                        }
#if defined(CMT_WMBUS_REPAIR) && CMT_WMBUS_REPAIR
                        // Runde 80 (2026-09-07): Per-Block-Bit-Repair (r78-
                        // Playbook "einziger Hebel unterhalb der Register-Ebene",
                        // r66-Slip-Law). Korruption = 1-Bit-Insert/Drop in
                        // Konstant-Runs (14-21 Bit) oder isolierte Einzelbitflips.
                        // Alle Block-CRCs VOR dem FAIL-Block passierten -> der
                        // Slip sitzt im FAIL-Block (Daten+CRC, max 18 B = 144 Bit).
                        // Kandidaten je Bitposition p: Flip(p), Ins0/Ins1(p)
                        // (fehlt ein Bit: einsetzen, Ende truncaten), Drop0/Drop1(p)
                        // (ueberschuessiges Bit: entfernen, Ende padden). Gueltig
                        // nur wenn ALLE Block-CRCs ab dem FAIL-Block danach passen
                        // (CRC-16-Kette). Erfolg -> Tail nach asmBuf zurueck-
                        // schreiben; der Frame ist dann CRC-verifiziert und geht
                        // ueber das b2ok-Gate in den Feed (gleiche Vertrauensstufe
                        // wie BOK). Kosten ~0.6 ms (720 Kandidaten, Early-Exit am
                        // ersten Fehlblock), nur je BBAD-Frame.
                        if (!b2ok && failPos) {
                            uint16_t tstart = (uint16_t)(i + failPos);
                            uint16_t tlen = (uint16_t)(pktLen - failPos);
                            uint16_t span = (uint16_t)(failRem > 16 ? 16 : failRem) + 2;
                            nRepTry++;
                            // Runde 83c: tstart+tlen = i+pktLen — bei Rausch-
                            // Frames mit i>0 lief der Read/Write bis
                            // i+pktLen-1 hinter asmBuf. Produktion sicher
                            // (echte Frames i=0), Rausch-Flut kann i>0.
                            if (tlen >= span && tlen <= sizeof(asmBuf) &&
                                (uint16_t)(tstart + tlen) <= sizeof(asmBuf)) {
                                static uint8_t rbits[128 * 8];
                                static uint8_t rtail[128];
                                uint16_t nbit = (uint16_t)tlen * 8;
                                for (uint16_t j = 0; j < tlen; j++) {   // Tail -> Bits laden
                                    uint8_t v = asmBuf[tstart + j];
                                    for (uint8_t b = 0; b < 8; b++) rbits[j * 8 + b] = (uint8_t)((v >> (7 - b)) & 1);
                                }
                                uint8_t repCls = 0, repVr = 0, lastb = 0, pb = 0;
                                uint16_t repP = 0;
                                for (uint8_t cls = 0; cls < 3 && !b2ok; cls++) {
                                    for (uint16_t p = 0; p < span * 8 && !b2ok; p++) {
                                        for (uint8_t vr = 0; vr < (cls ? 2 : 1) && !b2ok; vr++) {
                                            if (cls == 0) {
                                                rbits[p] ^= 1;                     // Flip(p)
                                            } else if (cls == 1) {
                                                lastb = rbits[nbit - 1];           // Ins vr@p
                                                for (uint16_t j = nbit - 1; j > p; j--) rbits[j] = rbits[j - 1];
                                                rbits[p] = vr;
                                            } else {
                                                pb = rbits[p];                     // Drop p, Ende pad vr
                                                for (uint16_t j = p; j < nbit - 1; j++) rbits[j] = rbits[j + 1];
                                                rbits[nbit - 1] = vr;
                                            }
                                            for (uint16_t j = 0; j < tlen; j++) {
                                                uint8_t v = 0;
                                                for (uint8_t b = 0; b < 8; b++) v = (uint8_t)(v << 1) | rbits[j * 8 + b];
                                                rtail[j] = v;
                                            }
                                            if (cmtTailBlocksOk(rtail, tlen, failRem)) {
                                                memcpy(asmBuf + tstart, rtail, tlen);
                                                b2 = "RPR"; b2ok = true;
                                                repCls = cls; repVr = vr; repP = p;
                                                if (cls == 0) nRepFlip++; else if (cls == 1) nRepIns++; else nRepDrop++;
                                                nRepOk++;
                                            }
                                            if (!b2ok) {                           // Rueckgaengig
                                                if (cls == 0) rbits[p] ^= 1;
                                                else if (cls == 1) {
                                                    for (uint16_t j = p; j < nbit - 1; j++) rbits[j] = rbits[j + 1];
                                                    rbits[nbit - 1] = lastb;
                                                } else {
                                                    for (uint16_t j = nbit - 1; j > p; j--) rbits[j] = rbits[j - 1];
                                                    rbits[p] = pb;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (b2ok)
                                    Serial.printf("[OMS-REPAIR %s] A=%02X%02X%02X%02X cls=%s bit=%u blk@%u vr=%u tlen=%u\n",
                                                  sv.name, t[4], t[5], t[6], t[7],
                                                  repCls == 0 ? "flip" : repCls == 1 ? "ins" : "drop",
                                                  (unsigned)(failPos * 8 + repP), (unsigned)failPos,
                                                  repVr, (unsigned)tlen);
                            }
                        }
                        // Runde 86 (2026-09-12): Kombiniert-Op-Repair fuer die
                        // Slip-Klasse. r85c-Teilsoak (Log/r85c_sync2b_soak.txt,
                        // 9 BBAD, verbatim-Walk-Sim): Fail-Ketten reichen bis
                        // Frame-Ende (869/809 [12,30,48,66] komplett fail;
                        // 20324347 B3-ok-Insel zwischen zwei Ketten) -> r66-
                        // Slip-Law "1-Bit-Shift + Flip an der Grenze". r80 deckt
                        // nur Einzel-Ops. Kandidaten: Shift ins/drop @p, Flip
                        // @q in [p-3, p+3] (Flip sitzt physikalisch an der Slip-
                        // Grenze). Gate failRem > 16: >= 2 Block-CRCs dahinter
                        // validieren den Kandidaten (False-Fire ~2^-32 je
                        // Kandidat). Nur je BBAD-Frame nach scheiternem r80,
                        // ~4k Kandidaten, Early-Exit am ersten Fehlblock.
                        if (!b2ok && failPos && failRem > 16) {
                            uint16_t tstart = (uint16_t)(i + failPos);
                            uint16_t tlen = (uint16_t)(pktLen - failPos);
                            uint16_t span = 18;   // failRem > 16 -> immer 16+2
                            nRepTry++;
                            if (tlen >= span && tlen <= sizeof(asmBuf) &&
                                (uint16_t)(tstart + tlen) <= sizeof(asmBuf)) {
                                static uint8_t rbits2[128 * 8];
                                static uint8_t rtail2[128];
                                uint16_t nbit = (uint16_t)tlen * 8;
                                for (uint16_t j = 0; j < tlen; j++) {
                                    uint8_t v = asmBuf[tstart + j];
                                    for (uint8_t b = 0; b < 8; b++)
                                        rbits2[j * 8 + b] = (uint8_t)((v >> (7 - b)) & 1);
                                }
                                uint8_t repCls = 0, repVr = 0;
                                uint16_t repP = 0, repQ = 0;
                                for (uint8_t cls = 1; cls <= 2 && !b2ok; cls++) {
                                    for (uint8_t vr = 0; vr < 2 && !b2ok; vr++) {
                                        for (uint16_t p = 0; p < span * 8 && !b2ok; p++) {
                                            uint16_t qlo = p > 3 ? (uint16_t)(p - 3) : 0;
                                            uint16_t qhi = (uint16_t)(span * 8 - 1);
                                            if (p + 3 < span * 8) qhi = (uint16_t)(p + 3);
                                            for (uint16_t q = qlo; q <= qhi && !b2ok; q++) {
                                                uint8_t lastb, pb;
                                                rbits2[q] ^= 1;               // Flip @q
                                                if (cls == 1) {               // Ins vr@p
                                                    lastb = rbits2[nbit - 1];
                                                    for (uint16_t j = nbit - 1; j > p; j--)
                                                        rbits2[j] = rbits2[j - 1];
                                                    rbits2[p] = vr;
                                                } else {                      // Drop p, pad vr
                                                    pb = rbits2[p];
                                                    for (uint16_t j = p; j < nbit - 1; j++)
                                                        rbits2[j] = rbits2[j + 1];
                                                    rbits2[nbit - 1] = vr;
                                                }
                                                for (uint16_t j = 0; j < tlen; j++) {
                                                    uint8_t v = 0;
                                                    for (uint8_t b = 0; b < 8; b++)
                                                        v = (uint8_t)((v << 1) | rbits2[j * 8 + b]);
                                                    rtail2[j] = v;
                                                }
                                                if (cmtTailBlocksOk(rtail2, tlen, failRem)) {
                                                    memcpy(asmBuf + tstart, rtail2, tlen);
                                                    b2 = "RPR"; b2ok = true;
                                                    repCls = cls; repVr = vr; repP = p; repQ = q;
                                                    if (cls == 1) nRepIns++; else nRepDrop++;
                                                    nRepOk++;
                                                }
                                                if (!b2ok) {                  // Rueckgaengig
                                                    if (cls == 1) {
                                                        for (uint16_t j = p; j < nbit - 1; j++)
                                                            rbits2[j] = rbits2[j + 1];
                                                        rbits2[nbit - 1] = lastb;
                                                    } else {
                                                        for (uint16_t j = nbit - 1; j > p; j--)
                                                            rbits2[j] = rbits2[j - 1];
                                                        rbits2[p] = pb;
                                                    }
                                                    rbits2[q] ^= 1;
                                                }
                                            }
                                        }
                                    }
                                }
                                if (b2ok)
                                    Serial.printf("[OMS-REPAIR2 %s] A=%02X%02X%02X%02X cls=%s bit=%u q=%u blk@%u vr=%u tlen=%u\n",
                                                  sv.name, t[4], t[5], t[6], t[7],
                                                  repCls == 1 ? "ins+flip" : "drop+flip",
                                                  (unsigned)(failPos * 8 + repP), (unsigned)repQ,
                                                  (unsigned)failPos, repVr, (unsigned)tlen);
                            }
                        }
#endif
                        if (b2ok) nB2ok++;
#if defined(CMT_WMBUS_FEED) && CMT_WMBUS_FEED
                        // Runde 79 (2026-09-07): rtlwmbus-Feed (r74 offline
                        // bewiesen: 79/79-Dekodierung via stdin:rtlwmbus).
                        // BOK-Frame CRC-LESS strippen — out = f[0:10] +
                        // 16er-Datenbloecke ohne CRC-Paare -> L+1 Bytes, L
                        // unveraendert, Invariante payload[0]==size-1 haelt
                        // per Konstruktion (out[0]=L, len=L+1) — und als
                        // Zeile C1;1;1;<ts>;<rssi>;0;0x<hex> ausgeben.
                        // Grammatik checkRTLWMBUSFrame (wmbus_rtlwmbus.cc):
                        // Teile 1-3 literal "1;1", ts strptime
                        // %Y-%m-%d %H:%M:%S (Restzeile toleriert — r74
                        // bewies das mit ".000"-Suffix), rssi <= 4 Zeichen.
                        // Gate MUSS b2ok sein: Telegram::parse macht KEINEN
                        // CRC-Check. ts = Boot-Anker (settimeofday in
                        // setup) + uptime. Nicht-BOK-Zeilen fehlen.
                        if (b2ok) {
                            char ln[600];   // 40 Prefix + 2*(0xF0+1) hex + slack
                            struct tm tmv;
                            time_t now = time(NULL);
                            localtime_r(&now, &tmv);
                            char *p = ln + strftime(ln, 40,
                                                    "C1;1;1;%Y-%m-%d %H:%M:%S.000;", &tmv);
                            p += sprintf(p, "%d;0;0x", rssiHi);
                            if (annex) {
                                // Runde 83y: BOKA-Feed — CRC-less-Prefix
                                // (gueltige Bloecke bis failPos) + ROHER
                                // Annex-Tail, L-Byte = outLen-1
                                // (wmbusmeters-Konvention; r83y_lrewrite:
                                // warn-frei, TPL-Decode komplett: CFG 2550,
                                // nb=5, 80 B AES + 4 B klar).
                                uint16_t outLen = (uint16_t)(10 + (failPos - 12)
                                                    - 2 * ((failPos - 12) / 18)
                                                    + (pktLen - failPos));
                                p += sprintf(p, "%02X", (uint8_t)(outLen - 1));
                                for (uint8_t j = 1; j < 10; j++)
                                    p += sprintf(p, "%02X", frame[i + j]);
                                uint16_t fpos = 12;
                                while (fpos < failPos) {
                                    for (uint8_t j = 0; j < 16; j++)
                                        p += sprintf(p, "%02X", frame[i + fpos + j]);
                                    fpos += 18;
                                }
                                for (uint16_t j = (uint16_t)(i + failPos);
                                     j < (uint16_t)(i + pktLen); j++)
                                    p += sprintf(p, "%02X", frame[j]);
                            } else {
                                for (uint16_t j = 0; j < 10; j++)
                                    p += sprintf(p, "%02X", frame[i + j]);
                                uint16_t fpos = 12, frem = L - 9;
                                while (frem) {
                                    uint8_t blk = frem > 16 ? 16 : (uint8_t)frem;
                                    for (uint8_t j = 0; j < blk; j++)
                                        p += sprintf(p, "%02X", frame[i + fpos + j]);
                                    fpos += blk + 2; frem -= blk;
                                }
                            }
                            *p++ = '\n';
                            // Runde 79b: niemals auf TX-Raum warten — kein
                            // Reader am Port heisst vollen TX-Ring und ein
                            // blockierendes Serial.write, das den Main-Loop
                            // mit anhängt. Feed-Zeile lieber droppen als die
                            // Empfangs-Engine zu blockieren.
                            if (Serial.availableForWrite() >= (uint32_t)(p - ln))
                                Serial.write((const uint8_t *)ln, p - ln);
                        }
#endif
                        Serial.printf("[OMS-C CRC-OK %s] L=%u C=0x%02X M=%02X%02X A=%02X%02X%02X%02X V=0x%02X T=0x%02X %s RSSI=%d off=%u:",
                                      sv.name, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                                      t[8], t[9], b2, rssiHi, i);
                        for (uint8_t j = 0; j < pktLen; j++) Serial.printf(" %02X", frame[j]);
                        Serial.print("\n[RSSI-TL] ");
                        for (uint8_t j = 0; j < 48; j++)
                            Serial.printf(" %02X", rssiHist[(uint8_t)(rssiIdx - 48 + j)]);
                        Serial.println();
                        // Runde 31: Cross-Burst-Assembly — die Telegramm-
                        // Wiederholungen je Burst byte-weis verschmelzen,
                        // Konfliktstellen per CRC2-Bruteforce arbitrieren.
                        // Runde 34: nur mg64-Fenster (fx31 hat nur 31 B —
                        // Null-Tail wuerde reale Merge-Bytes outvoten).
                        if (sv.merge && asmMerge(t, frame, L, i, sv.name, millis())) nAsm++;
                        break;
                    }
                }
            }
            Serial.printf("[SWFRAM %s] frames=%lu crcOK=%lu false=%lu B2=%lu annex=%lu rep=%lu/%lu(f=%lu,i=%lu,d=%lu) asm=%lu sync=%lu burstMs=%lu rssiHi=%d drains=%lu\n",
                          sv.name, (unsigned long)nFrames, (unsigned long)nCrcOk,
                          (unsigned long)nFalse, (unsigned long)nB2ok,
                          (unsigned long)nAnnexOk,
                          (unsigned long)nRepOk, (unsigned long)nRepTry,
                          (unsigned long)nRepFlip, (unsigned long)nRepIns, (unsigned long)nRepDrop,
                          (unsigned long)nAsm,
                          (unsigned long)nSync, (unsigned long)nBurstWin, rssiHi,
                          (unsigned long)nDrain);
            radio.goStandby();
            radio.writeReg(0x30, p30);
            radio.writeReg(0x32, p32);
            radio.writeReg(0x0C, p0C);
            radio.writeReg(0x2F, p2F);
            radio.writeReg(0x42, p42);
            radio.writeReg(0x41, p41);
            radio.writeReg(0x24, p24);
            radio.writeReg(0x25, p25);
            radio.writeReg(0x26, p26);
            radio.writeReg(0x22, p22);   // Runde 38: flt_bw zurueck
            radio.writeReg(0x28, p28);
            radio.writeReg(0x2B, p2B);
            radio.writeReg(0x2C, p2C);
            radio.writeReg(0x3C, p3C);
            radio.writeReg(0x46, p46);
            radio.writeReg(0x43, p43);
            radio.writeReg(0x44, p44);
            radio.writeReg(CMT_REG_CTL1_IO_SEL, p69);
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
        }
#endif  // CMT_SWFRAM
#if defined(CMT_BUFCAP) && CMT_BUFCAP
        // ---- Buffer-Mode-Gapless-Stream + Software-Framing. Der Durchbruch
        // fehlt nur noch in einem Punkt: die Telegramme (67 B) ueberschreiten
        // das 64-B-FIFO, Block2-CRC liegt bei Byte 65-66 — in EINEM Paket
        // nie erreichbar. Loesung: Data_MODE=1 (Buffer) laesst den Chip den
        // Demod-Strom OHNE Paket-FSM in den FIFO schreiben; wir lesen 32 B
        // je ~2.5 ms (12.3 kB/s ≈ Demod-Rate) in einen Ring und suchen dort
        // bitphasen-genaue (8 Phasen) nach Sync+Header 543D54CD. Volle
        // Telegramme inkl. Block2-CRC werden im Ring zusammengesetzt.
        {
            radio.goStandby();
            uint8_t p38 = radio.readReg(0x38), p0C = radio.readReg(0x0C);
            uint8_t p2F = radio.readReg(0x2F), p28 = radio.readReg(0x28);
            uint8_t p3C = radio.readReg(0x3C);
            radio.writeReg(0x0C, 0x62);      // LNA0 + LMT1 (moderat, AGC aus)
            radio.writeReg(0x2F, 0x52);      // AGC aus
            radio.writeReg(0x28, 0x8F);      // AFC aus
            radio.writeReg(0x38, 0x01);      // Data_MODE=1 (Buffer!), Pre off
            radio.writeReg(0x3C, 0x00);      // Sync aus (Software macht Framing)
            radio.setFrequencyHz(868950000UL);
            radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);
            radio.goRfs();
            radio.goRx();
            static uint8_t ring[288];
            uint16_t ringLen = 0;
            uint32_t nReads = 0, nB1 = 0, nB2 = 0, nOfs = 0;
            int rssiHi = -999;
            uint32_t tw = millis();
            uint8_t ch[32];
            while (millis() - tw < 25000) {
                int f6 = (uint8_t)radio.readReg(0x6F);
                if (f6 > rssiHi) rssiHi = f6;
                radio.readFifo(ch, 32);
                nReads++;
                if (ringLen + 32 > sizeof(ring)) {
                    uint16_t keep = sizeof(ring) - 32;
                    memmove(ring, ring + (ringLen - keep), keep);
                    ringLen = keep;
                }
                memcpy(ring + ringLen, ch, 32);
                ringLen += 32;
                static uint8_t sh[288];
                for (int k = 0; k < 8; k++) {
                    for (uint16_t j = 0; j + 1 < ringLen; j++)
                        sh[j] = k ? (uint8_t)((ring[j] << k) | (ring[j + 1] >> (8 - k))) : ring[j];
                    for (uint16_t p = 0; p + 70 < ringLen; p++) {
                        // Header "54 CD" — das vollstaendige Sync "54 3D" wird vom
                        // Burst-Anlauf angeknabbert, der Header ueberlebt immer.
                        if (sh[p] != 0x54 || sh[p + 1] != 0xCD) continue;
                        const uint8_t *t = sh + p + 2;   // Telegramm ab L
                        uint8_t L = t[0];
                        if (t[1] != 0x44 || L < 10 || L > 120) continue;
                        uint16_t c1 = wmbusCrc(t, 10);
                        uint16_t r1 = ((uint16_t)t[10] << 8) | t[11];
                        if (c1 != r1) continue;
                        nB1++;
                        if (12 + (L - 9) + 2 > (uint16_t)(ringLen - (p + 2))) continue;
                        uint16_t c2 = wmbusCrc(t + 12, L - 9);
                        uint16_t r2 = ((uint16_t)t[12 + (L - 9)] << 8) | t[13 + (L - 9)];
                        if (c2 != r2) continue;
                        nB2++;
                        Serial.printf("[OMS-FULL B1+B2] L=%u C=0x%02X M=%02X%02X A=%02X%02X%02X%02X V=0x%02X T=0x%02X RSSI=%d:",
                                      L, t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], rssiHi);
                        for (uint8_t j = 0; j < 12 + (L - 9) + 2 && j < 70; j++)
                            Serial.printf(" %02X", t[j]);
                        Serial.println();
                    }
                }
                // ~12.5 kB/s Demod: 32 B je 2.56 ms. Lesen dauert ~1.3 ms.
                delayMicroseconds(1100);
            }
            Serial.printf("[BUFCAP] reads=%lu ring=%u B1=%lu B2full=%lu rssiHi=%d\n",
                          (unsigned long)nReads, ringLen,
                          (unsigned long)nB1, (unsigned long)nB2, rssiHi);
            radio.goStandby();
            radio.writeReg(0x38, p38);
            radio.writeReg(0x0C, p0C);
            radio.writeReg(0x2F, p2F);
            radio.writeReg(0x28, p28);
            radio.writeReg(0x3C, p3C);
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
        }
#endif  // CMT_BUFCAP
        // ---- 5 s continuous RX with RSSI max-hold: the decisive RF-path
        // test. A Qundis telegram in the window pushes RSSI way above the
        // noise floor; a broken antenna path leaves it at the floor.
        radio.goStandby();
        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x02);  // RX FIFO clear
        radio.goRx();
        int maxRssi = -999, minRssi = 999;
        uint16_t irqAcc = 0;
        uint32_t t0 = millis();
        // 0x6F-only RSSI-Probe mit ms-Zeitstempeln ab 60: Korrelation gegen
        // unsere 2-s-Beacons (Zeiten stehen im Log als [TX]-Zeilen) und die
        // Flotte (rtl_433-Zeitstempel). 0x6E war ein Flag-Byte-Artefakt.
        int min6f = 999, max6f = -1;
        int nHi = 0;
        uint32_t spikeMs[12]; int8_t spikeVal[12];
        while (millis() - t0 < 1000) {   // Runde 38: 5s->1s (Deadtime 17%->4%)
            int f = (uint8_t)radio.readReg(0x6F);
            if (f < min6f) min6f = f;
            if (f > max6f) max6f = f;
            if (f > 60 && nHi < 12) { spikeMs[nHi] = millis(); spikeVal[nHi] = (int8_t)f; nHi++; }
            irqAcc |= radio.readIntFlag();
            delay(1);
        }
        minRssi = min6f; maxRssi = max6f;
        Serial.printf("[RSSI-SPIKE] 0x6F raw %d..%d | n>60: %d", min6f, max6f, nHi);
        for (int i = 0; i < nHi; i++)
            Serial.printf(" [%lu.%02d %d]", (unsigned long)spikeMs[i] % 100000, (int)(spikeMs[i] % 100), spikeVal[i]);
        Serial.println();
        Serial.printf("[RF-PROBE 1s (A-fix 543D/55)] rssi min=%d max=%d irqAcc=0x%04X "
                      "(PREAM=%d SYNC=%d PKT=%d)\n",
                      minRssi, maxRssi, irqAcc,
                      (irqAcc >> 12) & 1, (irqAcc >> 11) & 1, (irqAcc >> 8) & 1);
        radio.clearIntFlagHi(0xFF);
        radio.clearIntFlagLo(0xFF);
        // TX beacon: a valid Mode-C format-A frame (L/C/M/A + CRC verified)
        // with our sync (0x543D) prepended by the chip's TX path. The SDR
        // watches for it - this verifies the whole RF chain independently
        // of the RX config.
        static uint8_t beacon[31];      // FIFO fuellt 31 B (Fix-Length-Modus 0x46=0x1F)
        beacon[0] = 0x54; beacon[1] = 0xCD;
        beacon[2] = 0x0A; beacon[3] = 0x44;
        beacon[4] = 0x93; beacon[5] = 0x44;   // M-Feld: air = [M0 93][M1 44] -> rtl_433 M=0x4493 "QDS"
        beacon[6] = 0x67; beacon[7] = 0x45;   // A-ID BCD little-endian: bcd2int(d7)*1e6+... -> 1234567
        beacon[8] = 0x23; beacon[9] = 0x01;
        beacon[11] = 0x1A;
        beacon[14] = 0x78;                      // CI=0x78 Application Status (wie Qundis)
        uint16_t c2 = wmbusCrc(beacon + 14, 1);
        beacon[15] = c2 >> 8; beacon[16] = c2 & 0xFF;
        memset(beacon + 17, 0x55, 14);   // Pad (Decoder liest nur 15 Bytes)
        // Runde 62: V-Feld ([10], Teil von Block1+CRC1) pro Versuch variieren.
        // V ist im JSON unsichtbar (driver=unknown), veraendert aber die
        // Telegramm-Bytes -> Dedup umgangen. CRC1 wird hier und vor jedem
        // Retry neu gerechnet.
        beacon[10] = 0x35 + (g_beaconSeq++ & 0x0F);
        uint16_t c1 = wmbusCrc(beacon + 2, 10);
        beacon[12] = c1 >> 8; beacon[13] = c1 & 0xFF;
        // Runde 62 Pre-Probe: Chip-Zustand + FIFO-Flags VOR dem goStandby -
        // zeigt, ob sich der Stall-Wechselzyklus schon im Eingangszustand
        // (state/0x6C/latchte Flags) zeigt.
        Serial.printf("[TX pre] state=0x%02X 6C=0x%02X lo=0x%02X hi=0x%02X\n",
                      radio.readReg(CMT_REG_CTL1_MODE_STA), radio.readFifoFlag(),
                      radio.readReg(0x6A), radio.readReg(0x6D));
        radio.goStandby();
        // Runde 60 (2026-09-05) TX-PRODUKTIV-FRAMING: die rtl_433-verifizierte
        // TX-Bank-Fix-Aera lief mit 0x45=0x00/0x46=0x1F (Fixlaenge 31) +
        // 0x3C=0x02 (2-B-Sync 543D, Beacon traegt das Access-Code-Tail 54CD).
        // Der SWFRAM-RX-Flow zwingt 0x45=0x40/0x46=0x6D/0x3C=0x06 (Runde 49/
        // 51, RX-Hebel) -> der Beacon TXte 109-B-Payload mit 31-B-FIFO
        // (~1.07 s TX-Dauer, r59-Beweis: 0x61<3:0> liest 0x00 WAEHREND der
        // TX, nicht 0x06 - TX_DONE latcht erst nach der langen Sendung).
        // Der TX pinnt sein Framing selbst, danach RX-Produktivstand zurueck.
        // Runde 85c: RX-Stand JETZT sichern (svar-abhaengig: sync2b steht
        // auf 0x3C=0x02 + 0x46=0x6F, agc20 auf 0x3C=0x06 + 0x46=0x6D) -
        // die Restaurierung unten schreibt diesen Snapshot exakt zurueck
        // statt hartkodiert (der alte 0x40/0x6D/0x06-Zwang haette sync2b
        // nach jeder TX zerschossen).
        uint8_t rxP45 = radio.readReg(0x45);
        uint8_t rxP46 = radio.readReg(0x46);
        uint8_t rxP3C = radio.readReg(0x3C);
        radio.writeReg(0x45, 0x00);    // PKT_TYPE=0, LEN-Enable aus
        radio.writeReg(0x46, 0x1F);    // Payload = 31 B = FIFO-Vollstand
        radio.writeReg(0x3C, 0x02);    // SYNC_SIZE = 2 B (543D) wie Beacon-Layout
        // Runde 62: Framing-Readback - verifiziert, dass die 3 TX-Registerschreibe
        // wirklich anliegen (0x45/0x46/0x3C), bevor GO_TX kommt.
        Serial.printf("[TX frm] 45=%02X 46=%02X 3C=%02X\n",
                      radio.readReg(0x45), radio.readReg(0x46), radio.readReg(0x3C));
        // Runde 63 (2026-09-06): 0x03 = TX+RX FIFO clear - GUT-v5 besagt,
        // dass nach einer ABGESCHLOSSENEN TX die naechste GO_TX haengt
        // (r62: 1 clean + 8 Stalls, weil der r62-Retry jetzt vollstaendig
        // sendet und denselben Endzustand hinterlaesst wie Attempt 0;
        // r61-Alternanz kam vom 1-ms-Abort des stale-Retries). Verdacht:
        // FIFO/Engine-Pointer-Zustand. Beide FIFOs je TX hart zuruecksetzen.
        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);  // TX+RX FIFO clear @ 0x6C
        delay(2);
        radio.writeFifo(beacon, sizeof(beacon));
        Serial.printf("[TX fif] 6C=0x%02X nach %u B\n", radio.readFifoFlag(),
                      (unsigned)sizeof(beacon));
        // Runde 61 (2026-09-06) Stall-Guard: Beacon 1 (r60) radiete CW
        // 908 ms ohne TX_DONE - Einmalstall beim ersten TX nach Boot,
        // Prime-Verdacht TX_TEST-Weckwirkung (109-B-Fixlaenge mit 31-B-FIFO
        // = Engine verhungert, Chip im TX-Wait mit Tot-Lesevorgaengen 0x00;
        // TX_TEST ist jetzt AUS). Guard: STBY verifizieren, kurzes
        // TX_DONE-Fenster (echte TX = done@6ms, also 40 ms = 6x Margin),
        // bei Stall CW abwuergen, FIFO neu fuellen, 1x retry.
        if ((radio.readReg(CMT_REG_CTL1_MODE_STA) & 0x0F) != 0x02) {
            Serial.printf("[TX guard] STBY fehlt (state=0x%02X) -> goStandby nachschieben\n",
                          radio.readReg(CMT_REG_CTL1_MODE_STA));
            radio.goStandby();
        }
        uint32_t txT0 = 0;
        uint8_t txLo = 0;
        // Runde 62: 2-ms-Raster von (0x61<3:0>, 0x6A) ueber das 40-ms-Fenster
        // - zeigt, WANN der Chip in den Tot-Lesezustand kippt (state 0x00)
        // und ob 0x6A zwischendurch etwas latcht.
        uint8_t rastN = 0;
        static uint8_t rastT[12], rastSt[12], rastLo[12];
        for (uint8_t attempt = 0; attempt < 2; attempt++) {
            if (attempt) {
                Serial.printf("[TX stall] state=0x%02X lo=0x%02X -> retry\n",
                              radio.readReg(CMT_REG_CTL1_MODE_STA), txLo);
                radio.goStandby();
                radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);  // Runde 63: TX+RX
                delay(2);
                // Runde 62: Retry bekommt eigene Seq (Doppel-TX im Stall-
                // Fall sichtbar als 2 Receipts mit unterschiedlichem V).
                beacon[10] = 0x35 + (g_beaconSeq++ & 0x0F);
                uint16_t c1r = wmbusCrc(beacon + 2, 10);
                beacon[12] = c1r >> 8; beacon[13] = c1r & 0xFF;
                radio.writeFifo(beacon, sizeof(beacon));
            }
            // Runde 62: INT-Flags VOR GO_TX loeschen - eliminiert den
            // stale-TX_DONE-Fall (latchte Flags vom hangenden Attempt 0).
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
            radio.writeReg(CMT_REG_CTL1_MODE, CMT_GO_TX);  // direkt: kein
            // waitForState(0x06) - waehrend der TX liest 0x61 0x00 (r59), der
            // Wait wuerde 1 s verbraten. Stattdessen TX_DONE (0x6A bit3) pollen.
            txT0 = millis();
            txLo = 0;
            while (millis() - txT0 < 40) {
                txLo = radio.readReg(0x6A);
                if (rastN < 12) {
                    rastT[rastN] = (uint8_t)(millis() - txT0);
                    rastSt[rastN] = radio.readReg(CMT_REG_CTL1_MODE_STA) & 0x0F;
                    rastLo[rastN] = txLo;
                    rastN++;
                }
                if (txLo & 0x08) break;    // TX_DONE
                delay(2);
            }
            if (txLo & 0x08) break;
        }
        Serial.printf("[TX diag] done@%lums state=0x%02X lo=0x%02X hi=0x%02X fifo=0x%02X seq=0x%02X\n",
                      (unsigned long)(millis() - txT0), radio.readReg(CMT_REG_CTL1_MODE_STA),
                      txLo, radio.readReg(0x6D), radio.readFifoFlag(), beacon[10]);
        if (rastN) {
            Serial.printf("[TX rast]");
            for (uint8_t k = 0; k < rastN; k++)
                Serial.printf(" %u:%X/%02X", rastT[k], rastSt[k], rastLo[k]);
            Serial.println();
        }
        radio.goStandby();
        // Runde 63: NACH jeder TX (vollstaendig ODER abgebrochen) beide
        // FIFOs zuruecksetzen - simuliert den "Abort-frischen" Zustand, der
        // laut GUT-v5 den naechsten TX wieder lauffaehig macht.
        radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);
        delay(2);
        // RX-Produktivstand zurueck (Runde 49/51 + afcon) bevor der
        // RX-Probe unten laeuft - der naechste Window-Init schreibt es
        // zwar neu, aber der Probe-Zyklus hier ist sofort.
        // Runde 85c: exakter Snapshot-Restore (vor dem Pinning gesichert,
        // svar-abhaengig: sync2b = 0x3C=0x02 + 0x46=0x6F).
        radio.writeReg(0x45, rxP45);
        radio.writeReg(0x46, rxP46);
        radio.writeReg(0x3C, rxP3C);
        // RX probe: command RX explicitly and read the raw state while there.
        radio.goRx();
        delay(30);
        uint8_t st = radio.readReg(CMT_REG_CTL1_MODE_STA);
        uint16_t irq = radio.readIntFlag();
        Serial.printf("[HB %lus] probe state=0x%02X rssi(min..max)=%d..%d irq=0x%04X "
                      "(PREAM=%d SYNC=%d PKT=%d) pkts=%lu\n",
                      (unsigned long)(millis() / 1000), st, g_rssiMin, g_rssiMax, irq,
                      (irq >> 12) & 1, (irq >> 11) & 1, (irq >> 8) & 1,
                      (unsigned long)g_pktCount);
        g_rssiMin = 999; g_rssiMax = -999;
        radio.clearIntFlagHi(0xFF);
        radio.clearIntFlagLo(0xFF);
#if defined(CMT_STM8_BRIDGE_PROBE) && CMT_STM8_BRIDGE_PROBE
        // Stock-Firmware-Protokoll (Juli-Session verifiziert): transparente
        // Bridge @ 9600 8N1; "C5 C5 C5 01 ..." = PC-Kommando (Geräte-Info),
        // empfangene Funkbytes kommen RAW auf der UART.
        static uint32_t lastAt = 0;
        if (millis() - lastAt > 5000) {
            lastAt = millis();
            while (Serial2.available()) (void)Serial2.read();   // Puffer leeren
            const uint8_t info[10] = {0xC5,0xC5,0xC5,0x01, 0,0,0,0,0,0};
            Serial2.write(info, sizeof(info));
            String resp;
            uint32_t t2 = millis();
            while (millis() - t2 < 800) {
                while (Serial2.available()) resp += (char)Serial2.read();
            }
            if (resp.length()) {
                Serial.printf("[STM8-C5] Antwort %u B:", resp.length());
                for (unsigned i = 0; i < resp.length(); i++)
                    Serial.printf(" %02X", (uint8_t)resp[i]);
                Serial.println();
            } else {
                Serial.println(F("[STM8-C5] keine Antwort"));
            }
        }
        // Rohbytes (Funk-EMPFAENGER!) kontinuierlich mitloggen
        static String rawAcc;
        while (Serial2.available()) {
            rawAcc += (char)Serial2.read();
            if (rawAcc.length() >= 24) {
                Serial.printf("[STM8-RX raw]");
                for (unsigned i = 0; i < rawAcc.length(); i++)
                    Serial.printf(" %02X", (uint8_t)rawAcc[i]);
                Serial.println();
                rawAcc = "";
            }
        }
#endif
    }
#endif  // CMT_OMS_T1 heartbeat
#endif  // defined(CMT_STM8_ONLY)-else-Kette
#endif  // !(CMT_STM8_ONLY)
    delay(2);
}
