/*
 * stm8_swim_la.h
 *
 * Software-Logic-Analyzer für die SWIM-Leitung.
 */
#ifndef STM8_SWIM_LA_H
#define STM8_SWIM_LA_H

#include <Arduino.h>

// Initialisierung: Startet den Logic-Analyzer-Task auf Kern 1.
// Muss einmalig aus setup() aufgerufen werden, BEVOR Serial.begin.
void setup_logic_analyzer();

// Startet eine Aufnahme. Sobald der Task die Nachricht bekommt, läuft
// die LA bis logic_analyzer_stop() aufgerufen wird oder 4096 Zeilen
// gesendet sind.
void logic_analyzer_start();

// Stoppt die Aufnahme.
void logic_analyzer_stop();

#endif // STM8_SWIM_LA_H
