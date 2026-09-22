#pragma once

// HCI (iM871A-Emulation) fuer den E49-ESP32: der Daemon sieht das Board als
// IMST iM871A-Dongle (57600 8N1, /dev/ttyACM0:im871a[<uid>]).
// Wire-Format: [0xA5][(ctrlbits<<4)|ep][msgid][len][payload][+ts?4][+rssi?1][+crc16?2]
// Beweise: Log/hci_pty_oracle_*.txt (Evidenzgrad A) + wmbus_im871a.cc (Spec).

#include <Arduino.h>

#if defined(CMT_HCI_IM871A) && CMT_HCI_IM871A
void hciSetup();                    // UID berechnen + Parser init (setup()-Phase)
void hciPoll();                     // Parser-FSM: Requests lesen, RSP/TX fahren
bool hciWindowAbort();              // true = TX-Request traf ein -> RX-Fenster sofort beenden
void hciIndSend(const uint8_t *frame, uint16_t pktLen, uint16_t i, int rssiHi,
                bool annex, uint16_t failPos);   // IND-Emitter (BOK/BOKA-Verdict-Dock)
#else
// Kein HCI-Build: alle Docks zu No-Ops kollabieren (Null-Kosten).
static inline void hciSetup() {}
static inline void hciPoll() {}
static inline bool hciWindowAbort() { return false; }
static inline void hciIndSend(const uint8_t *, uint16_t, uint16_t, int, bool, uint16_t) {}
#endif