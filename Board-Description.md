Auf dem Bild ist ein Evaluierungs- bzw. Test-Board (das Modell **EBYTE E49-900MBL-01**) in Form eines USB-Dongles für ein drahtloses Funkmodul zu sehen. 

Mit den exakten Beschriftungen lässt sich die Hardware-Architektur nun fehlerfrei und detailliert aufschlüsseln:

### 1. Die integrierten Chips (ICs)
Das Board arbeitet mit einer dreiteiligen Architektur:
*   **Das Haupt-Funkmodul (Großer Chip mit Metallabdeckung):** 
    *   **EBYTE E49-900M20S:** Dies ist der eigentliche Sub-GHz Wireless Transceiver (basierend auf einem CMT2300A-Chip). Das "900M" steht für das Frequenzband (hier 868 MHz), das "20S" für 20 dBm (100 mW) Sendeleistung in SMD-Bauform. Dieses Modul ist nicht selbst programmierbar, sondern wird über eine SPI-Schnittstelle gesteuert.
*   **Der Mikrocontroller (Chip zwischen Funkmodul und Steckerleiste):**
    *   **STMicroelectronics STM8L151G** *(Beschriftung: GQ2AD12 8L151G...)*: Dies ist ein extrem stromsparender 8-Bit-Mikrocontroller. Er fungiert als "Gehirn" des Boards. Er verarbeitet die seriellen Befehle, die vom PC kommen, wandelt sie in SPI-Befehle um, um das Funkmodul zu steuern, und liest die Zustände der Taster und Jumper aus.
*   **Der USB-zu-Seriell-Wandler (Chip unten links):**
    *   **WCH CH340X** *(Beschriftung: 340xf52)*: Dieser Chip wandelt das USB-Protokoll des PCs in eine einfache serielle Kommunikation (UART) um. Die umgewandelten Signale (RX/TX) werden direkt an den STM8L-Mikrocontroller weitergeleitet.

### 2. Pinout (Die lange schwarze Stiftleiste)
Die weißen Beschriftungen links neben der Stiftleiste zeigen die Belegung der herausgeführten Pins, die direkt mit dem **STM8L-Mikrocontroller** (und nicht dem Funkmodul selbst) verbunden sind:
*   **VCC:** Stromversorgung der Logik (häufig 3,3V auf diesen Boards).
*   **GND:** Masse (Ground).
*   **VIO:** I/O-Spannungsreferenz (bestimmt den Spannungspegel der Datenleitungen).
*   **PC0, PB7, PD0, PE0, PD1:** Dies sind allgemeine Ein-/Ausgabe-Pins (GPIOs) des STM8L-Mikrocontrollers. Sie können genutzt werden, um eigene Sensoren anzuschließen oder Signale abzugreifen.
*   **TXD:** Transmit Data (Sendeleitung für serielle Kommunikation / UART).
*   **RXD:** Receive Data (Empfangsleitung für serielle Kommunikation / UART).

### 3. Jumper (Steckbrücken)
Es gibt zwei Bereiche mit aufgesteckten schwarzen Jumpern zur Hardware-Konfiguration:
*   **Jumper unten links (M0 und M1):** Dies sind die Betriebsmodus-Pins. Durch Setzen oder Entfernen der Jumper (HIGH/LOW) erkennt der Mikrocontroller, in welchem Modus das System arbeiten soll (z. B. Normaler Übertragungsmodus, Stromsparmodus, Konfigurations-/Sleep-Modus).
*   **Jumper oben links (bei VCC/VIO):** Hier wird mit der Steckbrücke die Betriebsspannung (VCC) auf die Referenzspannung für die Logikpegel (VIO) geschaltet, damit die I/O-Pins mit dem richtigen Spannungspegel arbeiten.

### 4. Knöpfe / Taster
Rechts neben der Stiftleiste, zwischen den GPIO-Pins und dem E49-Modul, befinden sich zwei kleine schwarze SMD-Taster:
*   Der chinesische Aufdruck lautet **按键1** (Taste 1) und **按键2** (Taste 2).
*   Diese Taster sind mit Eingangs-Pins des STM8L-Mikrocontrollers verbunden und dienen bei diesem Evaluation-Kit in der Regel dazu, vordefinierte Test-Signale abzusenden oder das Modul zurückzusetzen (Reset), ohne externe Hardware anschließen zu müssen.

### 5. Zusätzliche Elemente
*   **Status-LEDs (neben dem CH340X):** Beschriftet mit **POW** (Power/Strom - leuchtet im Bild), **TXD** (Senden) und **RXD** (Empfangen). Sie geben visuelles Feedback über den Datenaustausch zwischen dem USB-Chip und dem Mikrocontroller.
*   **Antenne (rechts im Bild):** Auf der SMA-Antenne steht **TX868-**. Das bestätigt endgültig, dass dieses Modul hardwareseitig für das **868-MHz-Band** (ein lizenzfreies ISM-Band, das in Europa typisch für LoRa/Smart-Home/IoT-Anwendungen ist) abgestimmt ist.

wofür sind die 3 leeren Pins rechts von der SPI-Leiste, über den Knöpfen?
Das sind mit an Sicherheit grenzender Wahrscheinlichkeit die Anschlüsse für die **SWIM-Schnittstelle** (Single Wire Interface Module).

Hier sind die genauen technischen Hintergründe dazu:

### 1. Wofür steht SWIM?
SWIM ist das proprietäre Programmier- und Debugging-Protokoll des Herstellers STMicroelectronics speziell für seine STM8-Mikrocontroller (wie dem hier verbauten STM8L151G). 

### 2. Warum sind diese 3 Pins dort?
Um dem leeren STM8-Chip in der Fabrik seine Software (die Firmware, die UART-Befehle vom USB-Port in SPI-Befehle für das Funkmodul übersetzt) beizubringen, muss er programmiert werden. 
*   **In der Fabrik:** Während der Produktion wird ein Programmiergerät mit sogenannten "Pogo-Pins" (Federkontakten) kurz auf diese 3 Löcher gedrückt, um den Chip in wenigen Sekunden zu flashen. 
*   **Für Endkunden:** Da es ein Evaluierungs-Board ist, lässt EBYTE diese Löcher absichtlich unbestückt. Entwickler können hier selbst eine 3-polige Stiftleiste einlöten, falls sie die Standard-Firmware löschen und den STM8-Mikrocontroller mit einer komplett eigenen Software programmieren möchten (z. B. mit einem handelsüblichen *ST-Link V2* Programmer).

### 3. Was ist die genaue Belegung?
Eine reduzierte SWIM-Schnittstelle für STM8-Chips benötigt im Batterie- oder USB-Betrieb exakt drei Leitungen. Von oben nach unten (oder umgekehrt, je nach Platinen-Layout) sind dies typischerweise:
1.  **SWIM:** Die eigentliche, bidirektionale Datenleitung für den Code.
2.  **NRST (Reset):** Um den Chip in den Programmiermodus zu zwingen.
3.  **GND (Masse):** Als elektrischer Bezugspunkt.
*(Die nötige Arbeitsspannung bekommt der Chip in diesem Moment ohnehin über den angesteckten USB-Port).*

**Zusammenfassend:** Die lange Steckerleiste, die Sie erwähnt haben, dient dazu, mit der *fertigen Software* des STM8-Chips zu kommunizieren oder Sensoren anzuschließen. Die 3 leeren Pins hingegen sind der direkte "Hintereingang" zum Gehirn des STM8-Chips, um das Betriebssystem selbst auszutauschen.
