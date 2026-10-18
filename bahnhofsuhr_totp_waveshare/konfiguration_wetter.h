// ============================================================
//  konfiguration_wetter.h  -  Einstellungen fuer die Wetteranzeige
//  Liegt im selben Ordner wie bahnhofsuhr_totp_waveshare.ino.
//  Daten: Open-Meteo (open-meteo.com), ohne Schluessel, nur aktuelle Werte.
// ============================================================
#pragma once

// true  = beim Start nach dem Zeitabgleich auch das Wetter holen (dann WLAN aus)
// false = Wetter nur manuell im Menue "Wetter" ueber "Aktualisieren"
const bool WETTER_BEIM_START_HOLEN = true;

// Angezeigter Ortsname (nur ASCII, keine Umlaute)
const char *WETTER_ORT = "Berlin";

// Koordinaten des Ortes (Dezimalgrad). Berlin (bitte durch den eigenen Ort ersetzen):
const double WETTER_BREITENGRAD = 52.5200;
const double WETTER_LAENGENGRAD = 13.4050;

// ---------- Startbildschirm ----------
// Was nach dem Start und nach 2 Minuten ohne Beruehrung angezeigt wird.
// Das ist nur der Vorgabewert; im Hauptmenue laesst es sich umschalten (wird gemerkt).
// false = analoge Uhr, true = Wetter
const bool STARTBILDSCHIRM_WETTER_STANDARD = false;

// Wenn der Startbildschirm "Wetter" ist: nach 2 Minuten ohne Beruehrung das Display
// ausschalten (Beruehren schaltet es wieder ein). Bei der Uhr bleibt es immer an.
const bool WETTER_BILDSCHIRM_AUS_NACH_TIMEOUT = true;

// ---------- Automatische Aktualisierung ----------
// Alle X Minuten das Wetter neu holen, solange das Wetter der Startbildschirm ist.
// 0 = nie automatisch (nur beim Start und manuell im Menue)
const unsigned long WETTER_AUTO_MINUTEN = 60;

// true = nur mit Netzteil/USB aktualisieren, bei reinem Akkubetrieb nicht (spart Energie)
const bool WETTER_AUTO_NUR_MIT_NETZTEIL = true;
