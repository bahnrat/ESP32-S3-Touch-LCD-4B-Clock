// ============================================================
//  konfiguration_funk.h  -  Einstellungen des Funk-Scanners
//  Nur hier etwas aendern.
// ============================================================
#pragma once

// Gemeinsamer Schluessel (16 Byte = 32 Hex-Zeichen). Auf Scanner UND Anzeige identisch!
// Eigenen Zufallswert eintragen, z.B. 16 Zufallsbytes aus einem Passwort-Generator als Hex.
// Alles 0x00 = "nicht eingerichtet", dann startet der Scanner nicht.
// Hinweis: Der Schluessel liegt im Flash und ist ohne Flash-Verschluesselung auslesbar.
const uint8_t FUNK_SCHLUESSEL[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// WLAN-Kanal fuer ESP-NOW (1..13), auf beiden Seiten gleich.
const int FUNK_KANAL = 1;
