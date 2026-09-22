// im871a_hci.cpp — ESP32-S3-Dongle als IMST iM871A-Emulation (Stufe A des
// e49-im871a-shim-Plans). HCI-Protokoll auf Serial/UART0 @ 57600 8N1.
//
// Wire-Format (bewiesen am echten Daemon, Log/hci_pty_oracle_*.txt, Grad A):
//   [0xA5][(ctrlbits<<4)|endpoint][msgid][len][payload][+ts?4][+rssi?1][+crc16?2]
//   ctrlbits: bit1 = timestamp, bit2 = rssi, bit3 = crc16.
//   CRC16: init 0xFFFF, poly 0x8408, LSB-first; Draht = ~crc little-endian;
//   Spanne = alles nach dem SOF bis vor den CRC (beim IND inkl. rssi-Byte).
//
// Der Daemon (wmbus_im871a.cc) schickt Requests OHNE ts/rssi/crc (4+len B);
// Antworten immer MIT crc16, IND zusaetzlich MIT rssi (ctrl 0xC2).
//
// IND-Semantik (wmbus_im871a.cc:907 verbatim): der Daemon re-insertiert
// payload_len als L-Byte vor den Payload -> Telegramm = [L][CRC-less C..].
// Der IND-Payload ist also der CRC-less-Content AB C (len-1 Bytes); len = L.
// Byte-identisch zur produktiv bewiesenen Feed-Zeile ab dem zweiten Byte.
//
// TX-Semantik (Orakel RUN2): WMBUSMSG_REQ-Payload = Content VERBATIM ab C
// (kein L, kein Sync). On-air baut die Firmware selbst HW-Sync 543D + den
// Access-Code-Tail 54 CD + [L] davor; L wird per CRC1-Klassifikator erkannt
// (scratches/hci_lbyte_check.py, ALLE OK) — volle Telegramme (mit CRCs) und
// CRC-less-Content unterscheiden sich damit automatisch.

#if defined(CMT_HCI_IM871A) && CMT_HCI_IM871A

#include "im871a_hci.h"
#include "cmt2300a.h"

extern CMT2300A radio;   // Definition main.cpp (gleiche Pins/Instanz)

namespace {

// ---------- HCI-CRC16 (verbatim aus crc16.cc / scratches/hci_crc_check.py) ----------
uint16_t hciCrc16(const uint8_t *d, uint16_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        uint8_t b = *d++;
        for (uint8_t k = 0; k < 8; k++) {
            if ((b ^ crc) & 1) crc = (crc >> 1) ^ 0x8408; else crc >>= 1;
            b >>= 1;
        }
    }
    return crc;
}

// ---------- wM-Bus CRC (EN 13757, Kopie von main.cpp wmbusCrc — dort static) ----------
uint16_t wmbusCrcH(const uint8_t *d, uint8_t n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x3D65) : (uint16_t)(crc << 1);
    }
    return crc ^ 0xFFFF;
}

// ---------- Parser-FSM ----------
enum { P_SOF, P_CTRL, P_MSGID, P_LEN, P_PAYLOAD };
uint8_t pState = P_SOF;
uint8_t pCtrl, pMsgid, pLen, pIdx;
uint8_t pBuf[96];               // Request-Payload (TX-Content bis ~77 B)

uint8_t  sUid[4];               // UID wire (LE) — Daemon-Display = reverse
uint8_t  sLinkMode = 0x0A;      // CT_N1A = Real-iM871A-Werkdefault (listento c1,t1)
volatile bool sAbort = false;   // TX-Request mid-window -> RX-Fenster abbrechen
volatile bool sRadioOn = false; // Runde 88: Werk-Idle wie der echte iM871A — bootet stumm,
                                // INDs erst nach dem ersten SET_CONFIG. Sonst vermuellt
                                // der Fleet-RX-Stream den Daemon-Detect-Buffer (Trial-0-
                                // Befund: IND-Fragment statt DEVICEINFO_RSP im Read).
volatile uint32_t sMuteUntil = 0; // Detect-Fenster: 400 ms ab Request-Erkennen (P_CTRL-
                                  // Uebergang) INDs suspendieren; deckt auch Daemon-Restart
                                  // (Re-Detect sendet wieder GET_DEVICEINFO)

// Zaehler fuer den Integrations-Beweis (Schritt 4)
uint32_t nReq = 0, nInd = 0, nTxAir = 0, nTxStall = 0, nParseErr = 0;

// ---------- Frame-Emitter (nicht-blockierend: droppen statt stauen) ----------
void hciSendFrame(uint8_t endpoint, uint8_t msgid, const uint8_t *pl, uint8_t plen,
                  bool withRssi, uint8_t rssi) {
    uint8_t fr[4 + 96 + 1 + 2];
    if (plen > 96) return;
    uint16_t n = 0;
    uint8_t ctrl = (uint8_t)(0x08 << 4) | endpoint;   // bit3 = crc16
    if (withRssi) ctrl |= (uint8_t)(0x04 << 4);       // bit2 = rssi -> 0xC
    fr[n++] = 0xA5;
    fr[n++] = ctrl;
    fr[n++] = msgid;
    fr[n++] = plen;
    if (plen) { memcpy(fr + n, pl, plen); n += plen; }
    if (withRssi) fr[n++] = rssi;
    uint16_t c = hciCrc16(fr + 1, n - 1);
    fr[n++] = (uint8_t)(~c & 0xFF);
    fr[n++] = (uint8_t)((~c >> 8) & 0xFF);
    if (Serial.availableForWrite() >= (int)n) Serial.write(fr, n);
}

// ---------- TX-L-Klassifikator (Python-verbatim, hci_lbyte_check.py ALLE OK) ----------
uint8_t hciTxL(const uint8_t *content, uint8_t clen) {
    if (clen >= 12) {
        uint8_t nb = (uint8_t)((clen - 11 + 17) / 18);   // ceil((clen-11)/18)
        uint8_t Lf = (uint8_t)(clen - 2 - 2 * nb);
        if (Lf >= 9) {
            uint8_t h[10];
            h[0] = Lf;
            memcpy(h + 1, content, 9);
            uint16_t c = wmbusCrcH(h, 10);
            uint16_t rx = (uint16_t)((content[9] << 8) | content[10]);
            if (c == rx) return Lf;          // volles Telegramm (mit CRCs)
        }
    }
    return clen;                             // CRC-less Content: L = eigene Laenge
}

// ---------- TX-Kette (verbatim aus der Produktiv-Rezeptur main.cpp r60-r63) ----------
bool hciDoTx(const uint8_t *content, uint8_t clen) {
    uint8_t fifo[3 + 96];
    uint8_t flen = (uint8_t)(3 + clen);
    if (clen > 96) return false;
    fifo[0] = 0x54;                          // Access-Code-Tail (HW-Sync 543D davor)
    fifo[1] = 0xCD;
    fifo[2] = hciTxL(content, clen);
    memcpy(fifo + 3, content, clen);

    uint8_t p45 = radio.readReg(0x45);
    uint8_t p46 = radio.readReg(0x46);
    uint8_t p3c = radio.readReg(0x3C);
    radio.goStandby();
    radio.writeReg(0x45, 0x00);              // r60: PKT_TYPE=0, LEN-Enable aus
    radio.writeReg(0x46, flen);              // r60: Fixlaenge = exakte FIFO-Bytezahl
    radio.writeReg(0x3C, 0x02);              // r60: 2-B-Sync (543D)
    radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);   // r63: TX+RX FIFO clear
    delay(2);
    radio.writeFifo(fifo, flen);
    if ((radio.readReg(CMT_REG_CTL1_MODE_STA) & 0x0F) != 0x02) radio.goStandby();
    radio.clearIntFlagHi(0xFF);              // stale TX_DONE eliminieren
    radio.clearIntFlagLo(0xFF);

    bool done = false;
    for (uint8_t attempt = 0; attempt < 2 && !done; attempt++) {
        if (attempt) {                       // 1 Stall-Retry (Produktiv-Muster)
            radio.goStandby();
            radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);
            delay(2);
            radio.writeFifo(fifo, flen);
            radio.clearIntFlagHi(0xFF);
            radio.clearIntFlagLo(0xFF);
        }
        radio.writeReg(CMT_REG_CTL1_MODE, CMT_GO_TX);   // direkt, kein State-Wait
        uint32_t t0 = millis();
        while (millis() - t0 < 100) {        // 100 ms fuer bis zu 99 B FIFO
            if (radio.readReg(0x6A) & 0x08) { done = true; break; }   // TX_DONE
            delay(2);
        }
    }
    if (done) nTxAir++; else nTxStall++;

    radio.goStandby();
    radio.writeReg(CMT_REG_CTL2_FIFO_FLAG, 0x03);
    delay(2);
    radio.writeReg(0x45, p45);               // RX-Framing restore
    radio.writeReg(0x46, p46);
    radio.writeReg(0x3C, p3c);
    radio.goRfs();
    radio.goRx();
    return done;
}

// ---------- Requests (Dispatch nach Parser-Completion) ----------
void hciDispatch(const uint8_t *pl, uint8_t plen) {
    pState = P_SOF;                          // Frame komplett -> neu synchronisieren
    nReq++;
    uint8_t ep = pCtrl & 0x0F;

    if (ep == 0x01) {                        // DEVMGMT
        switch (pMsgid) {
        case 0x01:                           // PING_REQ -> PING_RSP (leer)
            hciSendFrame(1, 0x02, nullptr, 0, false, 0);
            break;
        case 0x0F: {                         // GET_DEVICEINFO -> RSP msgid 0x10
            uint8_t d[8] = { 0x33, 0x00, 0x15, 0x16, sUid[0], sUid[1], sUid[2], sUid[3] };
            hciSendFrame(1, 0x10, d, 8, false, 0);
            break;
        }
        case 0x05: {                         // GET_CONFIG -> RSP msgid 0x06 (Padding!)
            uint8_t d[8] = { 0x12, sLinkMode, sUid[0], sUid[1], sUid[2], sUid[3], 0x00, 0x00 };
            hciSendFrame(1, 0x06, d, 8, false, 0);
            break;
        }
        case 0x03:                           // SET_CONFIG -> Echo als RSP msgid 0x04
            if (plen >= 3) sLinkMode = pl[2];   // pl = [temp,iff1,lm,iff2,rssi,ts]
            sRadioOn = true;                 // Daemon hat konfiguriert -> INDs freischalten
            hciSendFrame(1, 0x04, pl, plen, false, 0);
            break;
        default:
            break;                           // unbekannt: still ignorieren
        }
        return;
    }

    if (ep == 0x02) {                        // RADIOLINK
        if (pMsgid == 0x01 || pMsgid == 0x04) {    // WMBUSMSG_REQ / DATA_REQ
            sAbort = true;                   // RX-Fenster sofort abbrechen
            hciDoTx(pl, plen);
            hciSendFrame(2, pMsgid == 0x01 ? 0x02 : 0x05, nullptr, 0, false, 0);
        }
        return;
    }
    nParseErr++;
}

}  // namespace

void hciSetup() {
    uint64_t mac = ESP.getEfuseMac();        // 6 B LE: b0..b5
    for (int k = 0; k < 4; k++) sUid[k] = (uint8_t)((mac >> (8 * k)) & 0xFF);
    Serial.printf("[HCI] im871a-emul uid=%02x%02x%02x%02x (wire %02X%02X%02X%02X)\n",
                  sUid[3], sUid[2], sUid[1], sUid[0],
                  sUid[0], sUid[1], sUid[2], sUid[3]);
}

void hciPoll() {
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        switch (pState) {
        case P_SOF:
            if (b == 0xA5) pState = P_CTRL;
            break;
        case P_CTRL:                         // Daemon-Requests: nur endpoint-Nibble
            if ((b & 0xF8) != 0 || ((b & 0x0F) != 0x01 && (b & 0x0F) != 0x02)) {
                pState = P_SOF; nParseErr++; break;
            }
            pCtrl = b;
            pState = P_MSGID;
            sMuteUntil = millis() + 400;     // Detect-Fenster ab jetzt: keine INDs
            break;
        case P_MSGID:
            pMsgid = b;
            pState = P_LEN;
            break;
        case P_LEN:
            pLen = b;
            pIdx = 0;
            if (pLen > sizeof(pBuf)) { pState = P_SOF; nParseErr++; break; }
            if (pLen == 0) { hciDispatch(pBuf, 0); break; }
            pState = P_PAYLOAD;
            break;
        case P_PAYLOAD:
            pBuf[pIdx++] = b;
            if (pIdx >= pLen) hciDispatch(pBuf, pLen);
            break;
        }
    }
}

bool hciWindowAbort() {
    bool a = sAbort;
    sAbort = false;
    return a;
}

void hciIndSend(const uint8_t *frame, uint16_t pktLen, uint16_t i, int rssiHi,
                bool annex, uint16_t failPos) {
    // Feed-Konstruktion verbatim nach main.cpp (CMT_WMBUS_FEED-Block), aber
    // binaer: out = [L][CRC-less C..] (+ Annex: + roher Tail, L-Byte = outLen-1).
    // IND-Payload = out+1 (ohne L-Byte), len = out[0] (Daemon re-insertiert L).
    // Runde 88: Detect-Schutz — nicht radio-on oder im Mute-Fenster: drop.
    // (Daemon-Detect parst das erste Frame im Read-Buffer; ein IND davor = Fail.)
    if (!sRadioOn || (int32_t)(millis() - sMuteUntil) < 0) return;
    uint8_t out[128];
    uint16_t outLen = 0;

    if (!annex) {
        if (pktLen < (uint16_t)(i + 11)) return;
        uint8_t L = frame[i];
        if (L < 9 || (uint16_t)(L + 1) > sizeof(out)) return;
        for (uint8_t j = 0; j < 10; j++) out[outLen++] = frame[i + j];
        uint16_t fpos = 12;
        int frem = L - 9;
        while (frem > 0) {
            uint16_t blk = (frem > 16) ? 16 : (uint16_t)frem;
            if ((uint16_t)(i + fpos + blk) > pktLen) return;   // unvollstaendig -> weg
            for (uint16_t j = 0; j < blk; j++) out[outLen++] = frame[i + fpos + j];
            fpos += blk + 2;
            frem -= blk;
        }
    } else {
        if (failPos < 12 || pktLen < (uint16_t)(i + failPos + 20)) return;
        uint16_t outT = (uint16_t)(10 + (failPos - 12) - 2 * ((failPos - 12) / 18)
                                   + (pktLen - failPos));
        if (outT > sizeof(out)) return;
        out[outLen++] = (uint8_t)(outT - 1);     // L'-Byte (BOKA-Regel)
        for (uint8_t j = 1; j < 10; j++) out[outLen++] = frame[i + j];
        uint16_t fpos = 12;
        while (fpos < failPos) {
            for (uint8_t j = 0; j < 16; j++) out[outLen++] = frame[i + fpos + j];
            fpos += 18;
        }
        for (uint16_t j = i + failPos; j < (uint16_t)(i + pktLen); j++) out[outLen++] = frame[j];
    }

    hciSendFrame(2, 0x03, out + 1, (uint8_t)(outLen - 1), true, (uint8_t)rssiHi);
    nInd++;
}

#endif  // CMT_HCI_IM871A