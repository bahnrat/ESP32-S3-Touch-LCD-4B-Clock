// ============================================================
//  konfiguration.h  -  Persoenliche Einstellungen
//  Diese Datei liegt im selben Ordner wie bahnhofsuhr_totp_waveshare.ino
//  und wird beim Kompilieren automatisch eingebunden.
//  Nur HIER etwas aendern, der Hauptsketch bleibt unberuehrt.
//  Anfuehrungszeichen und Semikolons nicht loeschen!
// ============================================================
#pragma once

// ---------- WLAN (nur fuer den Zeitabgleich beim Start) ----------
// Bis zu drei Netze. Leere Anfuehrungszeichen "" = Eintrag unbenutzt.
// Hinweis: Die Zugangsdaten liegen im Flash des Geraets (nicht verschluesselt).
const char *WLAN_SSIDS[3]       = {"Netz1", "", ""};
const char *WLAN_PASSWOERTER[3] = {"Passwort1", "", ""};

// true  = beim Start kurz per WLAN die Zeit holen und die RTC stellen
// false = RTC nur manuell ueber "Zeit stellen" im Menue
const bool ZEIT_BEIM_START_PER_WLAN = true;

// Zeitzone (Standard: Deutschland, Sommer-/Winterzeit automatisch)
const char *TZ_STRING = "CET-1CEST,M3.5.0,M10.5.0/3";

// ---------- vCard fuer die QR-Code-Anzeige ----------
// Jede Zeile in Anfuehrungszeichen, am Ende "\n". Letzte Zeile ohne "\n".
const char *VCARD_TEXT =
    "BEGIN:VCARD\n"
    "VERSION:3.0\n"
    "N:Nachname;Vorname\n"
    "FN:Vorname Nachname\n"
    "ORG:Firma\n"
    "TEL:+49...\n"
    "EMAIL:...\n"
    "END:VCARD";

// ---------- Uhr: Form des Zifferblatts ----------
// true  = quadratisches Zifferblatt: das ganze Display ist weiss, die Uhr selbst (Striche, Zeiger)
//         bleibt unveraendert; es gibt keine schwarzen Ecken
// false = rundes Zifferblatt auf schwarzem Grund
const bool UHR_ZIFFERBLATT_QUADRATISCH = true;
// Nur beim quadratischen Zifferblatt: true = duenner schwarzer Ring um die Uhr, false = ohne Ring
const bool UHR_QUADRAT_RING = false;

// ---------- Uhr: Form des Sekundenzeigers ----------
// true  = Lochzeiger (rote Scheibe mit Loch, deutsche Bahnhofsuhr)
// false = einfache rote Linie
const bool UHR_SEKUNDENZEIGER_ALS_LOCHZEIGER = true;

// ---------- Start ----------
// true  = nach dem Start sofort die Bahnhofsuhr zeigen (Vorfuehr-Modus)
// false = nach dem Start das Hauptmenue zeigen
const bool NACH_START_UHR_ZEIGEN = true;

// ---------- Bildschirmhelligkeit (Regler im Hauptmenue) ----------
// Die Helligkeit wird per PWM an GPIO4 (Hintergrundbeleuchtung) geregelt. Die Voreinstellung passt
// zum Waveshare ESP32-S3-Touch-LCD-4B. Nur bei Bedarf die folgenden Zeilen aktivieren (// entfernen):
// #define HELLIGKEIT_REGLER_EINBAUEN 0   // 0 = Regler weglassen
// #define HELLIGKEIT_PWM_INVERTIERT 0    // 0, falls der Regler verkehrt herum wirkt (Standard: 1)
// #define HELLIGKEIT_PWM_HZ 5000         // PWM-Frequenz, falls das Bild bei niedriger Helligkeit flackert

// Akkubetrieb: Display nach so vielen Sekunden ohne Beruehrung ausschalten (0 = nie).
// Mit Netzteil/USB bleibt es an. Voreinstellung: 30. Zum Aendern die Zeile aktivieren:
// #define BATTERIE_DISPLAY_AUS_SEKUNDEN 30
