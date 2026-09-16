/*
 * stm8_swim_la.cpp
 *
 * Software-Logic-Analyzer für die SWIM-Leitung.
 *
 * Wichtig: liest direkt aus dem GPIO_IN-Register, um den
 * physischen Pin-Zustand zu sehen — auch wenn der Pin als
 * OUTPUT konfiguriert ist. digitalRead() würde sonst das
 * OUT-Register zurückgeben.
 *
 * GPIOs 0..31: GPIO_IN_REG   Bit = GPIO-Nummer
 * GPIOs 32..39: GPIO_IN1_REG Bit = GPIO-Nummer - 32
 *
 * Beim ESP32 spiegelt GPIO_IN_REG bei OUTPUT-Pins das OUT-Register.
 * Deshalb muss der LA-Pin ein eigener Pin sein, der als INPUT
 * konfiguriert ist und am SWIM-Bus mitlauscht.
 *
 * Wenn LA_REG = GPIO_IN_REG und LA_BIT = SWIM-Pin, sehen wir
 * nur den getriebenen Wert (Hilfe beim Verifizieren der Write-
 * Sequenz, aber keine externen Pulse).
 *
 * Wenn LA_REG = GPIO_IN1_REG (LA-Pin in 32..39), muss der LA-Pin
 * hardware-mäßig parallel zum SWIM-Bus verdrahtet sein.
 */

#include <Arduino.h>
#include <soc/gpio_reg.h>
#include <esp32/rom/gpio.h>

// Inline: 1 Sample lesen
// Wir benutzen digitalRead() statt GPIO_IN_REG, weil ESP32 das
// OUT-Register in GPIO_IN_REG spiegelt. digitalRead() liest den
// physischen Pin-Zustand für input-only GPIOs.
#define LA_PIN 35

static inline uint8_t la_sample() {
    return digitalRead(LA_PIN) ? '1' : '0';
}

static portMUX_TYPE la_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool la_running = false;
static volatile bool la_ready = true;

// 64 KB buffer
#define LA_BUF_SIZE  (64 * 1024)
static volatile uint8_t la_buf[LA_BUF_SIZE];

static void sample_loop(uint32_t max_samples) {
    uint32_t i = 0;
    for (; i < max_samples; i++) {
        la_buf[i] = la_sample();
        // ~ 200 kHz sample rate (5 us pro sample)
        // Genug fuer 1 ms LOW-Pulse = 200 Samples
        for (volatile int d = 0; d < 100; d++) { __asm__ __volatile__("nop"); }
    }
    for (; i < LA_BUF_SIZE; i++) la_buf[i] = 'X';
}

// Encode buffer as RLE
static void encode_and_send() {
    uint32_t end = LA_BUF_SIZE;
    for (uint32_t i = 0; i < LA_BUF_SIZE; i++) {
        if (la_buf[i] == 'X') { end = i; break; }
    }
    if (end == 0) return;

    char prev = la_buf[0];
    uint32_t run = 1;
    for (uint32_t i = 1; i < end; i++) {
        char c = la_buf[i];
        if (c == prev) {
            run++;
        } else {
            int rem = run;
            while (rem > 0) {
                int n = (rem > 35) ? 35 : rem;
                if (n < 10) {
                    Serial.write((prev == '1') ? ('1' + n - 1) : ('0' + n - 1));
                } else {
                    Serial.write((prev == '1') ? ('a' + n - 10) : ('A' + n - 10));
                }
                rem -= n;
            }
            prev = c;
            run = 1;
        }
    }
    int rem = run;
    while (rem > 0) {
        int n = (rem > 35) ? 35 : rem;
        if (n < 10) {
            Serial.write((prev == '1') ? ('1' + n - 1) : ('0' + n - 1));
        } else {
            Serial.write((prev == '1') ? ('a' + n - 10) : ('A' + n - 10));
        }
        rem -= n;
    }
    Serial.write('\n');
}

static void la_sample_task(void *arg) {
    while (true) {
        portENTER_CRITICAL(&la_mux);
        la_ready = true;
        portEXIT_CRITICAL(&la_mux);

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&la_mux);
        la_ready = false;
        la_running = true;
        portEXIT_CRITICAL(&la_mux);

        pinMode(LA_PIN, INPUT);
        sample_loop(LA_BUF_SIZE);
        la_running = false;

        encode_and_send();
        Serial.println("--- LA STOP ---");
    }
}

void setup_logic_analyzer() {
    pinMode(LA_PIN, INPUT);
    xTaskCreatePinnedToCore(la_sample_task, "logic_analyzer", 4096,
                            NULL, 1, NULL, 1);
}

void logic_analyzer_start() {
    TaskHandle_t h = xTaskGetHandle("logic_analyzer");
    if (!h) return;
    while (!la_ready) delay(1);
    xTaskNotifyGive(h);
    while (!la_running) delayMicroseconds(10);
}

void logic_analyzer_stop() {
    la_running = false;
}
