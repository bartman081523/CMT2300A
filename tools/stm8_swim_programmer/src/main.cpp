/*
 * ESP32 BSL-Entry-Sequenz für STM8L über CH340 am Dongle.
 *
 * Wenn 'P' empfangen wird, führt die ESP32 die korrekte STM8L BSL-Entry-
 * Sequenz durch:
 *   1. SWIM-Pin (PD1 / BS) auf HIGH setzen
 *   2. NRST auf LOW ziehen (Target im Reset halten)
 *   3. 100ms warten
 *   4. NRST loslassen (Pin als Input mit Pullup, damit Dongle übernehmen kann)
 *   5. SWIM HIGH halten
 *
 * Der STM8L wertet beim NRST-Rising-Edge den BS-Pin (= PD1/SWIM) aus:
 *   BS=HIGH -> Bootloader (BSL) wird gestartet
 *   BS=LOW  -> User-Code läuft
 *
 * Wiring:
 *   ESP32 GPIO25 (SWIM) -> Dongle TP2 (SWIM, ist PD1/BS)
 *   ESP32 GPIO26 (NRST) -> Dongle TP1 (NRST)
 *   ESP32 GND           -> Dongle TP3 (GND)
 *
 * CH340 am Dongle ist mit STM8 UART1 (PA2/PA3) verbunden.
 * Der PC kann dann stm8gal über /dev/ttyUSB1 (CH340) laufen lassen,
 * während die ESP32 die Entry-Sequenz getriggert hat.
 */

#include <Arduino.h>

#define SWIM_PIN 25
#define NRST_PIN 26
#define LED_PIN  2

void setup() {
    Serial.begin(115200);
    delay(300);

    // SWIM als Output HIGH, damit PD1 = BS = HIGH ist
    pinMode(SWIM_PIN, OUTPUT);
    digitalWrite(SWIM_PIN, HIGH);

    // NRST als Output HIGH (idle)
    pinMode(NRST_PIN, OUTPUT);
    digitalWrite(NRST_PIN, HIGH);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.println("READY (BSL entry via SWIM HIGH + NRST pulse)");
}

void bsl_entry_pulse() {
    Serial.println("BSL entry: SWIM=HIGH, NRST=LOW for 100ms, then NRST release");
    digitalWrite(LED_PIN, HIGH);

    // 1. NRST auf LOW
    digitalWrite(NRST_PIN, LOW);

    // 2. SWIM bleibt HIGH (sicherheitshalber nochmal setzen)
    digitalWrite(SWIM_PIN, HIGH);

    // 3. 100ms warten
    delay(100);

    // 4. NRST loslassen - STM8 wertet BS-Pin aus
    pinMode(NRST_PIN, INPUT_PULLUP);
    digitalWrite(SWIM_PIN, HIGH);  // weiterhin HIGH halten

    digitalWrite(LED_PIN, LOW);
    Serial.println("PULSED - STM8 BSL should now be running on UART1");
    Serial.println("Run stm8gal -p /dev/ttyUSB1 now!");
}

void loop() {
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'P' || c == 'p') {
            bsl_entry_pulse();
        }
    }
    delay(10);
}
