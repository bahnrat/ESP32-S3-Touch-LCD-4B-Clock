/*
 * SPDX-License-Identifier: GPL-3.0-only
 * AtomS3R-CAM - TOTP-QR-Scanner, FUNK-VARIANTE (ESP-NOW, AES-GCM verschluesselt)
 * Fuer das Waveshare ESP32-S3-Touch-LCD-4B (zweite Kamera). Die Kabel-Variante
 * (atoms3r_totp_scanner.ino) fuer die Qualia bleibt unveraendert.
 *
 * Wie es funktioniert:
 *   - Kein Router, kein Internet: Scanner und Anzeige senden direkt per ESP-NOW (Broadcast).
 *   - Jedes Paket ist mit AES-128-GCM verschluesselt und beglaubigt (siehe funk_protokoll.h).
 *     Ohne den gemeinsamen Schluessel kann niemand mitlesen oder ein Konto einschleusen.
 *   - Die Anzeige hoert nur im Menue "Neues Konto hinzufuegen" zu.
 *   - Ein aufgezeichnetes Paket kann hoechstens ein schon vorhandenes Konto erneut senden
 *     (die Anzeige antwortet dann "DUP"). Wurde ein Konto inzwischen geloescht, koennte es so
 *     wieder auftauchen - deshalb den Schluessel geheim halten und nur bei Bedarf senden.
 *
 * EINRICHTUNG:
 *   1. In konfiguration_funk.h einen EIGENEN zufaelligen Schluessel eintragen (16 Byte).
 *      Derselbe Schluessel muss spaeter in die Anzeige. Ohne Schluessel startet der Scanner nicht.
 *   2. funk_protokoll.h liegt in diesem Ordner und muss in der Anzeige identisch vorhanden sein.
 *   3. Strom ueber das USB-C der AtomS3R-CAM (Powerbank, Netzteil oder PC).
 *
 * ARDUINO-IDE:
 *   Board "ESP32S3 Dev Module", Flash 8MB, QIO, PSRAM "OPI PSRAM", USB CDC On Boot "Enabled".
 *   Bibliothek: ESP32QRCodeReader (ZIP, wie bei der Kabel-Variante).
 *
 * UNGEPRUEFT: Das Zusammenspiel mit der Anzeige wurde noch nie getestet. Die Anzeigeseite
 * (Empfaenger) gibt es noch nicht; das Protokoll steht in funk_protokoll.h.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP32QRCodeReader.h>
#include "konfiguration_funk.h"
#include "funk_protokoll.h"

// ---------- Funk ----------
static const uint8_t FUNK_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static volatile bool antwortDa = false;
static uint8_t antwortPaket[FUNK_MAX_PAKET];
static volatile size_t antwortLaenge = 0;

// Wird vom WLAN-Task aufgerufen: Paket nur zwischenspeichern, Auswertung in loop().
void funkEmpfangen(const esp_now_recv_info_t *info, const uint8_t *daten, int laenge) {
    if (laenge <= 0 || laenge > (int)FUNK_MAX_PAKET || antwortDa) {
        return;
    }
    memcpy(antwortPaket, daten, laenge);
    antwortLaenge = laenge;
    antwortDa = true;
}

bool funkStarten() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(FUNK_KANAL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, FUNK_BROADCAST, 6);
    peer.channel = FUNK_KANAL;
    peer.encrypt = false; // Verschluesselung uebernimmt funk_protokoll.h
    if (esp_now_add_peer(&peer) != ESP_OK) {
        return false;
    }
    esp_now_register_recv_cb(funkEmpfangen);
    return true;
}

bool funkSenden(const String &klartext) {
    uint8_t paket[FUNK_MAX_PAKET];
    size_t laenge = funkVerschluesseln(FUNK_SCHLUESSEL, klartext, paket);
    if (laenge == 0) {
        return false;
    }
    return esp_now_send(FUNK_BROADCAST, paket, laenge) == ESP_OK;
}

// Wartet auf eine verschluesselte Antwort der Anzeige ("OK", "DUP", "ERR"); leer = keine.
String funkAntwortLesen(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (antwortDa) {
            String klartext;
            bool echt = funkEntschluesseln(FUNK_SCHLUESSEL, antwortPaket, antwortLaenge, &klartext);
            antwortDa = false;
            if (echt) {
                return klartext;
            }
            // fremdes oder kaputtes Paket: ignorieren und weiter warten
        }
        delay(5);
    }
    return "";
}

// ---------- Kamera (GC0308 der AtomS3R-CAM) ----------
// Laut M5Stack muss GPIO 18 vor dem Kamerastart 500 ms auf LOW liegen (schaltet auch die Betriebs-LED ein).
const int KAMERA_POWER_PIN = 18;

CameraPins atoms3rCamPins() {
    CameraPins p = CAMERA_MODEL_AI_THINKER; // nur als Ausgangsbasis (setzt u.a. RESET auf "unbenutzt")
    p.PWDN_GPIO_NUM = -1;
    p.XCLK_GPIO_NUM = 21;                   // 20 MHz Kameratakt
    p.SIOD_GPIO_NUM = 12;                   // Kamera-I2C (SCCB) SDA
    p.SIOC_GPIO_NUM = 9;                    // Kamera-I2C (SCCB) SCL
    p.Y2_GPIO_NUM = 3;                      // D0
    p.Y3_GPIO_NUM = 42;                     // D1
    p.Y4_GPIO_NUM = 46;                     // D2
    p.Y5_GPIO_NUM = 48;                     // D3
    p.Y6_GPIO_NUM = 4;                      // D4
    p.Y7_GPIO_NUM = 17;                     // D5
    p.Y8_GPIO_NUM = 11;                     // D6
    p.Y9_GPIO_NUM = 13;                     // D7
    p.VSYNC_GPIO_NUM = 10;
    p.HREF_GPIO_NUM = 14;
    p.PCLK_GPIO_NUM = 40;
    return p;
}

ESP32QRCodeReader scanner(atoms3rCamPins()); // Standard: QVGA (320 x 240)

// ---------- otpauth://-Adresse zerlegen ----------

// Dekodiert %XX-Folgen (UTF-8-Bytes bleiben erhalten, die Uhr schreibt Umlaute spaeter um).
String prozentDekodieren(const String &eingabe) {
    String ergebnis = "";
    for (size_t i = 0; i < eingabe.length(); i++) {
        char c = eingabe[i];
        if (c == '%' && i + 2 < eingabe.length()) {
            char hex[3] = {eingabe[i + 1], eingabe[i + 2], 0};
            char *rest = nullptr;
            long wert = strtol(hex, &rest, 16);
            if (*rest == 0) {
                ergebnis += (char)wert;
                i += 2;
                continue;
            }
        }
        ergebnis += c;
    }
    return ergebnis;
}

// Liest einen Parameter aus dem Query-Teil (hinter dem "?"), URL-dekodiert.
String queryParameter(const String &uri, const String &name) {
    int fragezeichen = uri.indexOf('?');
    if (fragezeichen < 0) {
        return "";
    }
    String abfrage = "&" + uri.substring(fragezeichen + 1); // vorangestelltes & vereinfacht die Suche
    String muster = "&" + name + "=";
    int start = abfrage.indexOf(muster);
    if (start < 0) {
        return "";
    }
    start += muster.length();
    int ende = abfrage.indexOf('&', start);
    String wert = (ende < 0) ? abfrage.substring(start) : abfrage.substring(start, ende);
    return prozentDekodieren(wert);
}

// Zerlegt eine otpauth://totp/-Adresse (Aufrufer prueft den Anfang). Liefert das Secret
// (leer = keins gefunden); *labelAusgabe bekommt "Aussteller:Konto".
String base32AusUri(const String &uri, String *labelAusgabe) {
    String secret = queryParameter(uri, "secret");

    const int PRAEFIX_LAENGE = 15; // Laenge von "otpauth://totp/"
    int labelEnde = uri.indexOf('?', PRAEFIX_LAENGE);
    String roh = (labelEnde < 0) ? uri.substring(PRAEFIX_LAENGE) : uri.substring(PRAEFIX_LAENGE, labelEnde);
    roh = prozentDekodieren(roh);

    String aussteller = queryParameter(uri, "issuer");
    if (roh.length() == 0) {
        roh = aussteller;
    } else if (aussteller.length() > 0 && roh.indexOf(':') < 0) {
        roh = aussteller + ":" + roh; // Aussteller voranstellen, falls der Pfad ihn nicht schon nennt
    }

    if (labelAusgabe) {
        *labelAusgabe = (roh.length() > 0) ? roh : String("Unbekannt");
    }
    return secret;
}

// Die Uhr rechnet fest mit SHA1, 6 Stellen und 30 Sekunden. Weicht ein Konto davon ab, wuerde
// sie falsche Codes anzeigen - so ein Konto wird deshalb NICHT gesendet. Fehlende Angaben
// gelten nach der otpauth-Spezifikation als Standard (Aegis schreibt alle drei ausdruecklich hinein).
// Rueckgabe true = Standard; sonst steht in *abweichung zeilenweise, was nicht passt.
bool istStandardTotp(const String &uri, String *abweichung) {
    String algorithmus = queryParameter(uri, "algorithm");
    algorithmus.toUpperCase();
    String stellen = queryParameter(uri, "digits");
    String periode = queryParameter(uri, "period");

    String liste = "";
    if (algorithmus.length() > 0 && algorithmus != "SHA1") {
        liste += algorithmus + "\n";
    }
    if (stellen.length() > 0 && stellen != "6") {
        liste += stellen + " Stellen\n";
    }
    if (periode.length() > 0 && periode != "30") {
        liste += periode + " s Periode\n";
    }

    if (abweichung) {
        *abweichung = liste;
    }
    return liste.length() == 0;
}

// ---------- Senden und Antwort ----------

// Statusmeldung fuer den Bildschirm der Anzeige (und den Seriellen Monitor). Nie ein Secret hier hinein!
void infoAnAnzeige(const String &text) {
    Serial.println(text);
    funkSenden("INFO|" + text);
    delay(60); // dem Empfaenger Zeit geben, bevor das naechste Paket folgt
}

// Protokoll: verschluesselter Text "TOTP|<Label>|<Secret>", Antwort der Anzeige: "OK", "DUP" oder "ERR".
void sendeUeberFunk(String label, const String &secret) {
    label.replace("|", "-"); // "|" ist das Trennzeichen im Protokoll
    antwortDa = false;

    String nutzdaten = "TOTP|" + label + "|" + secret;
    if (nutzdaten.length() > FUNK_MAX_KLARTEXT) {
        Serial.println("Konto zu lang fuer ein Funkpaket - nicht gesendet.");
        return;
    }
    if (!funkSenden(nutzdaten)) {
        Serial.println("Senden fehlgeschlagen.");
        return;
    }

    String antwort = funkAntwortLesen(3000);
    if (antwort == "OK") {
        Serial.println("Konto gesendet und gespeichert.");
    } else if (antwort == "DUP") {
        Serial.println("Konto gibt es schon.");
    } else if (antwort == "ERR") {
        Serial.println("Fehler - Anzeige hat die Daten abgelehnt.");
    } else {
        Serial.println("Keine Antwort - Schluessel gleich? Anzeige im Menue 'Neues Konto hinzufuegen'?");
    }
}

void setup() {
    Serial.begin(115200);
    // Native USB: bis zu 5 s auf den Seriellen Monitor warten, damit die Startmeldungen
    // nicht verloren gehen, wenn er erst nach dem Start geoeffnet wird.
    unsigned long wartenStart = millis();
    while (!Serial && millis() - wartenStart < 5000) {
        delay(10);
    }
    Serial.println("Scanner startet ...");

    if (!funkSchluesselGesetzt(FUNK_SCHLUESSEL)) {
        Serial.println("FEHLER: In konfiguration_funk.h ist noch kein Schluessel eingetragen.");
        while (true) {
            delay(1000);
        }
    }
    if (!funkStarten()) {
        Serial.println("FEHLER: ESP-NOW konnte nicht gestartet werden.");
        while (true) {
            delay(1000);
        }
    }

    // Kamera einschalten: GPIO 18 LOW, dann 500 ms warten (Vorgabe von M5Stack)
    pinMode(KAMERA_POWER_PIN, OUTPUT);
    digitalWrite(KAMERA_POWER_PIN, LOW);
    delay(500);

    Serial.println("Kamera wird gestartet ...");
    // Rueckgabewert als Zahl verglichen: 0 = alles in Ordnung (Namen der Fehlercodes je nach Bibliotheksversion verschieden)
    int fehler = (int)scanner.setup();
    if (fehler != 0) {
        Serial.printf("FEHLER: Kamera-Setup fehlgeschlagen (Code %d) - PSRAM aktiv (OPI)? Pins?\n", fehler);
        while (true) {
            delay(1000);
        }
    }
    scanner.beginOnCore(1);
    Serial.println("Bereit zum Scannen.");
}

void loop() {
    static String letzteAdresse = "";
    static unsigned long letzterScan = 0;

    // Lebenszeichen alle 10 s: so sieht man auch bei spaet geoeffnetem Monitor, dass der Sketch laeuft.
    static unsigned long letzterHerzschlag = 0;
    if (millis() - letzterHerzschlag > 10000) {
        letzterHerzschlag = millis();
        Serial.println("Scanner laeuft - warte auf QR-Code ...");
    }

    struct QRCodeData qrData;
    if (!scanner.receiveQrCode(&qrData, 100)) {
        return;
    }
    if (!qrData.valid) {
        return; // erkannt, aber nicht lesbar - einfach weiter versuchen
    }

    String gelesen = String((const char *)qrData.payload);

    // Haelt man den Code laenger ins Bild, wird er mehrfach erkannt: nur beim ersten Mal reagieren.
    if (gelesen == letzteAdresse && millis() - letzterScan < 8000) {
        letzterScan = millis();
        return;
    }
    letzteAdresse = gelesen;
    letzterScan = millis();

    // Bewusst NICHT die ganze Adresse ausgeben: sie enthaelt das Secret.
    Serial.printf("QR-Code erkannt (%u Zeichen)\n", (unsigned)gelesen.length());

    if (gelesen.startsWith("otpauth://totp/")) {
        String label;
        String secret = base32AusUri(gelesen, &label);
        String abweichung;

        if (secret.length() == 0) {
            infoAnAnzeige("Kein Secret im QR-Code gefunden.");
        } else if (!istStandardTotp(gelesen, &abweichung)) {
            abweichung.replace("\n", ", ");
            infoAnAnzeige("Nicht unterstuetzt: " + abweichung + "nicht gesendet.");
        } else {
            if (label.length() > 40) {
                label = label.substring(0, 40);
            }
            infoAnAnzeige("QR erkannt: " + label);
            sendeUeberFunk(label, secret);
        }
    } else if (gelesen.startsWith("otpauth://hotp/")) {
        infoAnAnzeige("HOTP wird nicht unterstuetzt.");
    } else if (gelesen.startsWith("otpauth-migration://")) {
        infoAnAnzeige("Sammelexport wird nicht unterstuetzt - einzelne Konten scannen.");
    } else if (gelesen.startsWith("otpauth://")) {
        infoAnAnzeige("Dieser Kontotyp wird nicht unterstuetzt.");
    } else {
        infoAnAnzeige("Kein TOTP-QR-Code.");
    }
}
