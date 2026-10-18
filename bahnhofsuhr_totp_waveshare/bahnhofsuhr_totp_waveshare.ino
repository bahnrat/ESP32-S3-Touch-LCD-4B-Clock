/*
 * Bahnhofsuhr + TOTP  -  Waveshare ESP32-S3-Touch-LCD-4B (480x480)
 * SPDX-License-Identifier: GPL-3.0-only   (Lizenztext: siehe LICENSE im Hauptordner)
 * Uhr, PIN, Hauptmenue, TOTP-Konten (anzeigen, umbenennen, loeschen),
 *           Zeit stellen, QR-Code (vCard), Herunterfahren,
 *           neue Konten per verschluesseltem Funk (ESP-NOW) von der AtomS3R-CAM.
 *
 * Arduino-IDE-Einstellungen:
 *   Board: ESP32S3 Dev Module | USB CDC On Boot: Disabled | PSRAM: OPI PSRAM
 *   Flash Size: 16MB | Flash Mode: QIO 80MHz | Port: COM3 (CH343)
 *   Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
 *     (alternativ eine 16-MB-Variante mit FATFS - der Sketch erkennt beides)
 * Bibliotheken: lvgl 9.3.0 (lv_conf.h im libraries-Ordner), GFX Library for Arduino,
 *               SensorLib (TouchDrvGT911), ArduinoJson 7, NimBLE-Arduino 2.x (nur fuer die Anrufanzeige)
 * Einstellungen (WLAN, Zeitzone, vCard, Start): konfiguration.h; Wetter: konfiguration_wetter.h
 * Funk (Schluessel, Kanal): konfiguration_funk.h und funk_protokoll.h (identisch zum Scanner)
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <FFat.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <esp_sleep.h>
#include <lvgl.h>
#include "TouchDrvGT911.hpp"
#include "Arduino_GFX_Library.h"
#include "konfiguration.h"
#include "konfiguration_wetter.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include "konfiguration_funk.h"
#include "funk_protokoll.h"
#include "konfiguration_ble.h"
#if __has_include("konfiguration_web.h")
#include "konfiguration_web.h"
#endif

// Ton ueber den eingebauten Lautsprecher: Vorgaben, falls sie in konfiguration_ble.h nicht stehen
// (so bleibt die alte Datei weiter gueltig). Alles Weitere dazu steht dort.
#ifndef BLE_TON_EINBAUEN
#define BLE_TON_EINBAUEN 1          // 1 = Ton-Funktion einbauen, 0 = weglassen (z.B. bei Compilerfehlern)
#endif
#ifndef BLE_TON_STANDARD_ANRUF
#define BLE_TON_STANDARD_ANRUF 1    // Ton bei Anruf nach dem ersten Start (danach gilt das Menue)
#endif
#ifndef BLE_TON_STANDARD_SMS
#define BLE_TON_STANDARD_SMS 1      // Ton bei SMS nach dem ersten Start
#endif
#ifndef BLE_TON_LAUTSTAERKE
#define BLE_TON_LAUTSTAERKE 40      // 0 bis 100
#endif
#ifndef BLE_TON_ANRUF_WIEDERHOLEN
#define BLE_TON_ANRUF_WIEDERHOLEN 1 // 1 = klingelt bis der Anruf endet, 0 = nur einmal
#endif
#ifndef BLE_TON_SMS_ANZAHL
#define BLE_TON_SMS_ANZAHL 3        // so oft wird der SMS-Ton hintereinander gespielt (1 bis 20)
#endif
// Anschluss des Audio-Chips ES8311 laut Waveshare-Schaltplan (I2C liegt auf demselben Bus wie der Touch)
#ifndef AUDIO_PIN_MCLK
#define AUDIO_PIN_MCLK 5
#endif
#ifndef AUDIO_PIN_BCLK
#define AUDIO_PIN_BCLK 16
#endif
#ifndef AUDIO_PIN_WS
#define AUDIO_PIN_WS 7
#endif
#ifndef AUDIO_PIN_DOUT
#define AUDIO_PIN_DOUT 6
#endif
#ifndef AUDIO_PA_EXPANDER_PIN
#define AUDIO_PA_EXPANDER_PIN 3     // Freigabe des Lautsprecher-Verstaerkers am Port-Expander (EXIO3)
#endif

#if BLE_ANRUFANZEIGE_EINBAUEN
#include <NimBLEDevice.h>
#if BLE_TON_EINBAUEN
#include <driver/i2s_std.h>
#endif
#endif

#if LV_COLOR_DEPTH != 16
#error "lv_conf.h: LV_COLOR_DEPTH muss 16 sein"
#endif
#if LV_USE_QRCODE == 0
#error "lv_conf.h: LV_USE_QRCODE muss 1 sein (QR-Code der Visitenkarte)"
#endif

// ============================================================
//  Hardware (aus dem Waveshare-Demo)
// ============================================================
const int I2C_SDA = 47;
const int I2C_SCL = 48;
const uint8_t RTC_ADRESSE = 0x51; // PCF85063

// Pixeltakt des Displays. Bei Streifen im Bild verkleinern (z.B. 10000000),
// bei sichtbarem Flimmern vergroessern (z.B. 14000000, maximal 16000000).
const uint32_t DISPLAY_PIXELTAKT = 10000000;
// true: LVGL zeichnet in einen Puffer im PSRAM (ruhigeres Bild, z.B. Sekundenzeiger).
// false: kleiner Puffer im internen RAM (Streifen werden einzeln aufs Display geschrieben).
const bool ZEICHENPUFFER_IM_PSRAM = true;

Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(7, 0, 2, 1, &Wire, 0x20);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    17 /* DE */, 3 /* VSYNC */, 46 /* HSYNC */, 9 /* PCLK */,
    10, 11, 12, 13, 14 /* B0-B4 */,
    21, 8, 18, 45, 38, 39 /* G0-G5 */,
    40, 41, 42, 2, 1 /* R0-R4 */,
    1 /* hsync_polarity */, 10 /* front porch */, 8 /* pulse width */, 50 /* back porch */,
    1 /* vsync_polarity */, 10 /* front porch */, 8 /* pulse width */, 20 /* back porch */,
    0 /* pclk_active_neg */, DISPLAY_PIXELTAKT, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */, 480 * 10 /* Bounce-Puffer in Pixeln */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480, 480, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    expander, GFX_NOT_DEFINED /* RST */,
    st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

TouchDrvGT911 GT911;
int16_t touchX[5], touchY[5];

// ============================================================
//  Zustand
// ============================================================
const uint8_t PIN_LAENGE = 6;
const uint8_t MAX_FEHLVERSUCHE = 5;
const uint32_t LABEL_MAX_LAENGE = 40; // nur ASCII
const unsigned long TIMEOUT_ZEIT = 120000; // 2 Minuten Inaktivitaet
const time_t ZEIT_PLAUSIBEL_AB = 1735689600; // 01.01.2025
const int ZEIT_JAHR_START = 2025;
const int ZEIT_JAHR_ANZAHL = 21;

enum GeraeteStatus {
    STATUS_HAUPTMENUE,
    STATUS_PIN_EINRICHTEN,
    STATUS_PIN_EINGABE,
    STATUS_TOTP_MENUE,
    STATUS_TOTP,
    STATUS_KONTO_HINZUFUEGEN,
    STATUS_KONTO_UMBENENNEN,
    STATUS_ZEIT_STELLEN,
    STATUS_KONTO_LOESCHEN,
    STATUS_UHR_LAEUFT,
    STATUS_QR,
    STATUS_WETTER,
    STATUS_WETTER_START,
    STATUS_BLE,
    STATUS_WEB
};
// (Muss oben stehen: die Arduino IDE erzeugt Prototypen am Dateianfang.)
enum KontoErgebnis { KONTO_OK, KONTO_UNGUELTIG, KONTO_DUPLIKAT };
enum ListenModus { LISTE_UMBENENNEN, LISTE_LOESCHEN };

// ============================================================
//  Einrichtung per Weboberflaeche: Vorgaben, falls konfiguration_web.h fehlt
// ============================================================
#ifndef WEB_EINRICHTUNG_EINBAUEN
#define WEB_EINRICHTUNG_EINBAUEN 1
#endif
#ifndef WEB_EINRICHTUNG_MINUTEN
#define WEB_EINRICHTUNG_MINUTEN 10
#endif
#ifndef WEB_HOTSPOT_NAME
#define WEB_HOTSPOT_NAME "Bahnhofsuhr-Setup"
#endif
#ifndef WEB_HOTSPOT_KANAL
#define WEB_HOTSPOT_KANAL 6
#endif
#define WE_VCARD_MAX 1001
#define WE_NETZE 5          // so viele WLAN-Netze lassen sich speichern (die konfiguration.h liefert die ersten 3)

// Einstellungen, die man per Weboberflaeche aendern kann. Start: Werte aus den konfiguration*.h-Dateien;
// was ueber die Weboberflaeche gespeichert wurde (Flash, Bereich "web"), hat Vorrang.
struct WebEinstellungen {
    char ssid[WE_NETZE][33];
    char pass[WE_NETZE][65];
    bool zeitBeimStart;
    char tz[64];
    char ort[32];
    float breite;
    float laenge;
    bool wetterBeimStart;
    int wetterMinuten;
    bool wetterNurNetz;
    bool wetterAusTimeout;
    bool nachStartUhr;
    bool quadratisch;
    bool quadratRing;
    int smsAnzahl;
    char vcard[WE_VCARD_MAX];
};
static WebEinstellungen we;

static void weLaden() {
    for (int i = 0; i < WE_NETZE; i++) {
        bool ausDatei = (i < 3);
        strlcpy(we.ssid[i], (ausDatei && WLAN_SSIDS[i]) ? WLAN_SSIDS[i] : "", sizeof(we.ssid[i]));
        strlcpy(we.pass[i], (ausDatei && WLAN_PASSWOERTER[i]) ? WLAN_PASSWOERTER[i] : "", sizeof(we.pass[i]));
    }
    we.zeitBeimStart = ZEIT_BEIM_START_PER_WLAN;
    strlcpy(we.tz, TZ_STRING, sizeof(we.tz));
    strlcpy(we.ort, WETTER_ORT, sizeof(we.ort));
    we.breite = (float)WETTER_BREITENGRAD;
    we.laenge = (float)WETTER_LAENGENGRAD;
    we.wetterBeimStart = WETTER_BEIM_START_HOLEN;
    we.wetterMinuten = (int)WETTER_AUTO_MINUTEN;
    we.wetterNurNetz = WETTER_AUTO_NUR_MIT_NETZTEIL;
    we.wetterAusTimeout = WETTER_BILDSCHIRM_AUS_NACH_TIMEOUT;
    we.nachStartUhr = NACH_START_UHR_ZEIGEN;
    we.quadratisch = UHR_ZIFFERBLATT_QUADRATISCH;
    we.quadratRing = UHR_QUADRAT_RING;
    we.smsAnzahl = BLE_TON_SMS_ANZAHL < 1 ? 1 : (BLE_TON_SMS_ANZAHL > 20 ? 20 : BLE_TON_SMS_ANZAHL);
    strlcpy(we.vcard, VCARD_TEXT, sizeof(we.vcard));
#if WEB_EINRICHTUNG_EINBAUEN
    Preferences p;
    if (!p.begin("web", false)) return;
    for (int i = 0; i < WE_NETZE; i++) {
        char sk[3] = {'s', (char)('0' + i), 0}, wk[3] = {'w', (char)('0' + i), 0};
        if (p.isKey(sk)) {
            p.getString(sk, we.ssid[i], sizeof(we.ssid[i]));
            we.pass[i][0] = 0;
            if (p.isKey(wk)) p.getString(wk, we.pass[i], sizeof(we.pass[i]));
        }
    }
    we.zeitBeimStart = p.getBool("zs", we.zeitBeimStart);
    if (p.isKey("tz")) p.getString("tz", we.tz, sizeof(we.tz));
    if (p.isKey("ort")) p.getString("ort", we.ort, sizeof(we.ort));
    we.breite = (float)p.getDouble("br", we.breite);
    we.laenge = (float)p.getDouble("la", we.laenge);
    we.wetterBeimStart = p.getBool("wbs", we.wetterBeimStart);
    we.wetterMinuten = p.getInt("wm", we.wetterMinuten);
    we.wetterNurNetz = p.getBool("wn", we.wetterNurNetz);
    we.wetterAusTimeout = p.getBool("wa", we.wetterAusTimeout);
    we.nachStartUhr = p.getBool("nu", we.nachStartUhr);
    we.quadratisch = p.getBool("qd", we.quadratisch);
    we.quadratRing = p.getBool("qr", we.quadratRing);
    we.smsAnzahl = p.getInt("sa", we.smsAnzahl);
    if (we.smsAnzahl < 1) we.smsAnzahl = 1;
    if (we.smsAnzahl > 20) we.smsAnzahl = 20;
    if (p.isKey("vc")) p.getString("vc", we.vcard, sizeof(we.vcard));
    p.end();
#endif
}

GeraeteStatus aktuellerStatus = STATUS_HAUPTMENUE;
bool uhrWarAktivVorMenue = false; // gilt fuer den Startbildschirm (Uhr oder Wetter)
bool bildschirmIstAus = false;    // Display (Hintergrundbeleuchtung) ausgeschaltet
uint8_t startModus = 0;           // 0 = Uhr, 1 = Wetter (wird im Flash gemerkt)
void bildschirmEinschalten();
void helligkeitPwmPause(bool aus);
bool timeoutBereitsBehandelt = false;
bool rtcVorhanden = false;
bool rtcZeitGueltig = false;

Preferences sicherheitSpeicher;

// ============================================================
//  RTC (PCF85063, haelt UTC)
// ============================================================
static uint8_t bcdNachDez(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dezNachBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static time_t utcAusTm(const struct tm &t) {
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long tage = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)(tage * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
}

// false = RTC nicht erreichbar oder Oszillator war stehengeblieben (Zeit unbrauchbar)
bool rtcLesen(time_t *utc) {
    Wire.beginTransmission(RTC_ADRESSE);
    Wire.write((uint8_t)0x04);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)RTC_ADRESSE, 7) != 7) return false;
    uint8_t s = Wire.read();
    uint8_t mi = Wire.read();
    uint8_t h = Wire.read();
    uint8_t d = Wire.read();
    Wire.read();
    uint8_t mo = Wire.read();
    uint8_t y = Wire.read();
    bool ungueltig = (s & 0x80) != 0;
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = bcdNachDez(s & 0x7F);
    t.tm_min = bcdNachDez(mi & 0x7F);
    t.tm_hour = bcdNachDez(h & 0x3F);
    t.tm_mday = bcdNachDez(d & 0x3F);
    t.tm_mon = bcdNachDez(mo & 0x1F) - 1;
    t.tm_year = 100 + bcdNachDez(y);
    *utc = utcAusTm(t);
    return !ungueltig;
}

bool rtcSchreiben(time_t utc) {
    struct tm t;
    gmtime_r(&utc, &t);
    Wire.beginTransmission(RTC_ADRESSE);
    Wire.write((uint8_t)0x04);
    Wire.write(dezNachBcd(t.tm_sec) & 0x7F); // Bit 7 = 0 loescht das OS-Flag
    Wire.write(dezNachBcd(t.tm_min));
    Wire.write(dezNachBcd(t.tm_hour));
    Wire.write(dezNachBcd(t.tm_mday));
    Wire.write(dezNachBcd(t.tm_wday));
    Wire.write(dezNachBcd(t.tm_mon + 1));
    Wire.write(dezNachBcd((t.tm_year + 1900) % 100));
    return Wire.endTransmission() == 0;
}

uint32_t rtcUnixzeit() {
    time_t t = 0;
    rtcLesen(&t);
    return (uint32_t)t;
}

bool lokaleZeit(struct tm *ortszeit) {
    time_t utc = 0;
    bool gueltig = rtcLesen(&utc);
    localtime_r(&utc, ortszeit);
    return gueltig;
}

void zeitAbgleichMerken(time_t utc) {
    Preferences zeitSpeicher;
    zeitSpeicher.begin("zeit", false);
    zeitSpeicher.putULong("abgleich", (uint32_t)utc);
    zeitSpeicher.end();
}

uint32_t zeitLetzterAbgleich() {
    Preferences zeitSpeicher;
    zeitSpeicher.begin("zeit", true);
    uint32_t t = zeitSpeicher.getULong("abgleich", 0);
    zeitSpeicher.end();
    return t;
}

void rtcSetzen(time_t utc) {
    if (rtcSchreiben(utc)) {
        rtcZeitGueltig = true;
        zeitAbgleichMerken(utc);
        Serial.printf("RTC gestellt (UTC-Epoche %lu).\n", (unsigned long)utc);
    } else {
        Serial.println("FEHLER: RTC schreiben fehlgeschlagen.");
    }
}

// ============================================================
//  Zeit per WLAN
// ============================================================
static const char *wlanStatusText(wl_status_t st) {
    switch (st) {
        case WL_NO_SSID_AVAIL: return "Netz nicht gefunden (SSID falsch, 5-GHz-Netz oder zu weit weg)";
        case WL_CONNECT_FAILED: return "Verbindung abgelehnt (Passwort falsch oder WPA3-only)";
        case WL_CONNECTION_LOST: return "Verbindung verloren";
        case WL_DISCONNECTED: return "nicht verbunden (Timeout)";
        case WL_IDLE_STATUS: return "idle (Timeout)";
        default: return "unbekannt";
    }
}

// ============================================================
//  Wetter (Open-Meteo, nur aktuelle Werte)
// ============================================================
struct WetterDaten {
    bool gueltig;
    float temperatur;
    float gefuehlt;
    float wind;       // km/h
    int feuchte;      // %
    int code;         // WMO-Wettercode
    time_t abgerufenUtc;
};
WetterDaten wetter = {false, 0, 0, 0, 0, 0, 0};

const char *wetterCodeText(int code) {
    switch (code) {
        case 0: return "Klar";
        case 1: return "Ueberwiegend klar";
        case 2: return "Teils bewoelkt";
        case 3: return "Bedeckt";
        case 45: case 48: return "Nebel";
        case 51: case 53: case 55: return "Nieselregen";
        case 56: case 57: return "Gefrierender Nieselregen";
        case 61: return "Leichter Regen";
        case 63: return "Regen";
        case 65: return "Starker Regen";
        case 66: case 67: return "Gefrierender Regen";
        case 71: return "Leichter Schneefall";
        case 73: return "Schneefall";
        case 75: return "Starker Schneefall";
        case 77: return "Schneegriesel";
        case 80: case 81: case 82: return "Regenschauer";
        case 85: case 86: return "Schneeschauer";
        case 95: return "Gewitter";
        case 96: case 99: return "Gewitter mit Hagel";
        default: return "Unbekannt";
    }
}

// Setzt voraus, dass das WLAN bereits verbunden ist. Aendert die Daten nur bei Erfolg.
bool wetterHolenVerbunden() {
    char url[360];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
             "&wind_speed_unit=kmh&timezone=auto",
             (double)we.breite, (double)we.laenge);

    WiFiClientSecure client;
    client.setInsecure(); // Wetterdaten sind unkritisch; keine Zugangsdaten werden gesendet
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, url)) {
        Serial.println("Wetter: HTTP-Start fehlgeschlagen.");
        return false;
    }
    int status = http.GET();
    if (status != 200) {
        Serial.printf("Wetter: HTTP-Status %d\n", status);
        http.end();
        return false;
    }
    String antwort = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, antwort)) {
        Serial.println("Wetter: Antwort nicht lesbar.");
        return false;
    }
    JsonObject aktuell = doc["current"];
    if (aktuell.isNull() || !aktuell["temperature_2m"].is<float>()) {
        Serial.println("Wetter: unerwartete Antwort.");
        return false;
    }
    wetter.temperatur = aktuell["temperature_2m"].as<float>();
    wetter.gefuehlt = aktuell["apparent_temperature"] | wetter.temperatur;
    wetter.feuchte = aktuell["relative_humidity_2m"] | 0;
    wetter.code = aktuell["weather_code"] | -1;
    wetter.wind = aktuell["wind_speed_10m"] | 0.0f;
    time_t utc = 0;
    if (!rtcLesen(&utc)) time(&utc);
    wetter.abgerufenUtc = utc;
    wetter.gueltig = true;
    Serial.printf("Wetter: %.1f C, Code %d\n", wetter.temperatur, wetter.code);
    return true;
}

// ============================================================
//  WLAN-Sitzung: verbinden, Zeit holen, Wetter holen, WLAN aus
// ============================================================
static bool wlanVerbinden() {
    bool verbunden = false;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    for (int i = 0; i < WE_NETZE && !verbunden; i++) {
        if (we.ssid[i][0] == 0) continue;
        Serial.printf("WLAN '%s' (Passwort mit %d Zeichen) ...\n", we.ssid[i], (int)strlen(we.pass[i]));
        WiFi.disconnect(true);
        delay(200);
        WiFi.begin(we.ssid[i], we.pass[i][0] ? we.pass[i] : NULL);
        for (int w = 0; w < 150 && WiFi.status() != WL_CONNECTED; w++) delay(100);
        verbunden = (WiFi.status() == WL_CONNECTED);
        if (!verbunden) Serial.printf("  fehlgeschlagen: %s (Code %d)\n", wlanStatusText(WiFi.status()), (int)WiFi.status());
    }
    if (!verbunden) {
        Serial.println("Sichtbare 2,4-GHz-Netze:");
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            Serial.printf("  '%s'  Kanal %d  %d dBm  %s\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i),
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "offen" : "verschluesselt");
        }
        if (n <= 0) Serial.println("  keine gefunden");
    }
    return verbunden;
}

static bool zeitHolenVerbunden() {
    configTzTime(we.tz, "de.pool.ntp.org", "pool.ntp.org");
    time_t jetzt = 0;
    for (int w = 0; w < 100; w++) {
        time(&jetzt);
        if (jetzt >= ZEIT_PLAUSIBEL_AB) break;
        delay(100);
    }
    if (jetzt >= ZEIT_PLAUSIBEL_AB) {
        rtcSetzen(jetzt);
        return rtcZeitGueltig;
    }
    Serial.println("Keine NTP-Antwort.");
    return false;
}

// Eine WLAN-Sitzung. Rueckgabe: true, wenn alles Gewuenschte geklappt hat.
bool wlanAbgleich(bool zeitHolen, bool wetterHolen) {
    bool erfolg = false;
    if (wlanVerbinden()) {
        erfolg = true;
        if (zeitHolen && !zeitHolenVerbunden()) erfolg = false;   // zuerst die Zeit
        if (wetterHolen && !wetterHolenVerbunden()) erfolg = false; // dann das Wetter
    } else {
        Serial.println("Kein WLAN verbunden.");
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setenv("TZ", we.tz, 1); // die NTP-Funktionen aendern die Zeitzone
    tzset();
    return erfolg;
}

bool zeitPerWlanHolen() {
    return wlanAbgleich(true, false);
}

// ============================================================
//  Sicherheit / PIN
// ============================================================
String sha256Hex(const String &eingabe) {
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)eingabe.c_str(), eingabe.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    String hex = "";
    char puffer[3];
    for (int i = 0; i < 32; i++) {
        sprintf(puffer, "%02x", hash[i]);
        hex += puffer;
    }
    return hex;
}

String saltLadenOderErzeugen() {
    sicherheitSpeicher.begin("sicherheit", false);
    String salt = sicherheitSpeicher.getString("pin_salt", "");
    if (salt.length() == 0) {
        for (int i = 0; i < 16; i++) {
            salt += String(esp_random() % 16, HEX);
        }
        sicherheitSpeicher.putString("pin_salt", salt);
    }
    sicherheitSpeicher.end();
    return salt;
}

void pinSetzen(const String &neuePin) {
    String salt = saltLadenOderErzeugen();
    String hash = sha256Hex(salt + neuePin);
    sicherheitSpeicher.begin("sicherheit", false);
    sicherheitSpeicher.putString("pin_hash", hash);
    sicherheitSpeicher.putUInt("fehlversuche", 0);
    sicherheitSpeicher.putULong("gesperrt_bis", 0);
    sicherheitSpeicher.end();
}

bool pinIstEingerichtet() {
    sicherheitSpeicher.begin("sicherheit", true);
    bool vorhanden = sicherheitSpeicher.isKey("pin_hash");
    sicherheitSpeicher.end();
    return vorhanden;
}

// Basiert bewusst auf der RTC, nicht auf millis(): ein Reset umgeht die Sperre nicht.
bool istGesperrt(uint32_t *verbleibendeSekunden) {
    sicherheitSpeicher.begin("sicherheit", true);
    uint32_t gesperrtBis = sicherheitSpeicher.getULong("gesperrt_bis", 0);
    sicherheitSpeicher.end();
    uint32_t jetzt = rtcUnixzeit();
    if (gesperrtBis > jetzt) {
        if (verbleibendeSekunden) *verbleibendeSekunden = gesperrtBis - jetzt;
        return true;
    }
    return false;
}

// ============================================================
//  Konten (LittleFS oder FFat, eine JSON-Datei) und TOTP
// ============================================================
const char *KONTEN_DATEI = "/konten.json";
fs::FS *dateisystem = nullptr;

void dateisystemInitialisieren() {
    if (LittleFS.begin(false)) { dateisystem = &LittleFS; Serial.println("Dateisystem: LittleFS"); return; }
    if (FFat.begin(false))     { dateisystem = &FFat;     Serial.println("Dateisystem: FFat");     return; }
    // Erster Start: formatieren
    if (LittleFS.begin(true))  { dateisystem = &LittleFS; Serial.println("Dateisystem: LittleFS (neu formatiert)"); return; }
    if (FFat.begin(true))      { dateisystem = &FFat;     Serial.println("Dateisystem: FFat (neu formatiert)");     return; }
    Serial.println("FEHLER: kein Dateisystem. Partition Scheme mit SPIFFS oder FATFS waehlen!");
}

JsonDocument kontenLaden() {
    JsonDocument doc;
    if (dateisystem == nullptr) { doc.to<JsonArray>(); return doc; }
    File datei = dateisystem->open(KONTEN_DATEI, "r");
    if (!datei) { doc.to<JsonArray>(); return doc; }
    DeserializationError fehler = deserializeJson(doc, datei);
    datei.close();
    if (fehler) {
        Serial.println("Warnung: Kontodatei fehlt/beschaedigt - starte mit leerer Liste.");
        doc.clear();
        doc.to<JsonArray>();
    }
    return doc;
}

// Kleiner Zwischenspeicher, damit die Codeanzeige nicht mehrmals pro Sekunde die Datei liest
static bool cacheAnzahlGueltig = false;
static uint32_t cacheAnzahl = 0;
static int32_t cacheIndex = -1;
static String cacheLabel, cacheSecret;

void kontenCacheLeeren() {
    cacheAnzahlGueltig = false;
    cacheIndex = -1;
}

void kontenSchreiben(JsonDocument &doc) {
    kontenCacheLeeren();
    if (dateisystem == nullptr) return;
    File datei = dateisystem->open(KONTEN_DATEI, "w");
    if (!datei) {
        Serial.println("FEHLER: Kontodatei konnte nicht zum Schreiben geoeffnet werden!");
        return;
    }
    serializeJson(doc, datei);
    datei.close();
}

uint32_t totpKontenAnzahl() {
    if (!cacheAnzahlGueltig) {
        JsonDocument doc = kontenLaden();
        cacheAnzahl = doc.as<JsonArray>().size();
        cacheAnzahlGueltig = true;
    }
    return cacheAnzahl;
}

void totpKontoLaden(uint32_t index, String *label, String *secret) {
    if ((int32_t)index == cacheIndex) {
        *label = cacheLabel;
        *secret = cacheSecret;
        return;
    }
    JsonDocument doc = kontenLaden();
    JsonArray konten = doc.as<JsonArray>();
    if (index >= konten.size()) {
        *label = "Unbekannt";
        *secret = "";
        return;
    }
    *label = konten[index]["label"].as<String>();
    *secret = konten[index]["secret"].as<String>();
    cacheIndex = (int32_t)index;
    cacheLabel = *label;
    cacheSecret = *secret;
}

void kontoSpeichern(const String &label, const String &secretBase32) {
    JsonDocument doc = kontenLaden();
    JsonArray konten = doc.as<JsonArray>();
    JsonObject neu = konten.add<JsonObject>();
    neu["label"] = label;
    neu["secret"] = secretBase32;
    kontenSchreiben(doc);
    Serial.printf("Konto gespeichert: %s\n", label.c_str());
}

void kontoLoeschen(uint32_t index) {
    JsonDocument doc = kontenLaden();
    JsonArray konten = doc.as<JsonArray>();
    if (index >= konten.size()) return;
    konten.remove(index);
    kontenSchreiben(doc);
    Serial.println("Konto geloescht.");
}

void kontoUmbenennen(uint32_t index, const String &neuerName) {
    JsonDocument doc = kontenLaden();
    JsonArray konten = doc.as<JsonArray>();
    if (index >= konten.size()) return;
    konten[index]["label"] = neuerName;
    kontenSchreiben(doc);
    Serial.println("Konto umbenannt.");
}

// Die LVGL-Schriften kennen nur ASCII: Umlaute umschreiben, Rest durch "?" ersetzen.
String labelNachAscii(String label) {
    label.replace("ä", "ae"); label.replace("ö", "oe"); label.replace("ü", "ue");
    label.replace("Ä", "Ae"); label.replace("Ö", "Oe"); label.replace("Ü", "Ue");
    label.replace("ß", "ss");
    String ergebnis = "";
    for (size_t i = 0; i < label.length(); i++) {
        unsigned char c = (unsigned char)label[i];
        if (c >= 32 && c < 127) {
            ergebnis += (char)c;
        } else if (c >= 0xC0 || c < 0x80) {
            ergebnis += '?';
        }
    }
    return ergebnis;
}

bool secretZeichenGueltig(const String &secret) {
    if (secret.length() < 8 || secret.length() > 128) return false;
    for (size_t i = 0; i < secret.length(); i++) {
        char c = secret[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7') || c == '=';
        if (!ok) return false;
    }
    return true;
}

String duplikatKontoName = ""; // Name des bereits vorhandenen Kontos mit gleichem Secret
bool kontoSecretVorhanden(const String &secret) {
    JsonDocument doc = kontenLaden();
    for (JsonVariant konto : doc.as<JsonArray>()) {
        if (konto["secret"].as<String>() == secret) {
            duplikatKontoName = konto["label"].as<String>();
            return true;
        }
    }
    return false;
}

// Gemeinsame Pruefung fuer jeden Empfangsweg (wird in Etappe 3 vom Funkempfang genutzt)
KontoErgebnis kontoPruefenUndSpeichern(String label, String secret) {
    label = labelNachAscii(label);
    label.replace("|", "-");
    label.trim();
    if (label.length() > LABEL_MAX_LAENGE) label = label.substring(0, LABEL_MAX_LAENGE);
    secret.trim();
    secret.toUpperCase();
    if (label.length() == 0 || !secretZeichenGueltig(secret)) return KONTO_UNGUELTIG;
    if (kontoSecretVorhanden(secret)) return KONTO_DUPLIKAT;
    kontoSpeichern(label, secret);
    return KONTO_OK;
}

// Base32 (RFC 4648)
int base32Dekodieren(const String &eingabe, uint8_t *ausgabe, int maxLaenge) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int bitPuffer = 0, bitsImPuffer = 0, ausgabeLaenge = 0;
    for (size_t i = 0; i < eingabe.length(); i++) {
        char c = toupper(eingabe[i]);
        if (c == '=') break;
        const char *pos = strchr(alphabet, c);
        if (pos == nullptr) continue;
        int wert = pos - alphabet;
        bitPuffer = (bitPuffer << 5) | wert;
        bitsImPuffer += 5;
        if (bitsImPuffer >= 8) {
            if (ausgabeLaenge >= maxLaenge) break;
            ausgabe[ausgabeLaenge++] = (bitPuffer >> (bitsImPuffer - 8)) & 0xFF;
            bitsImPuffer -= 8;
        }
    }
    return ausgabeLaenge;
}

// RFC 6238 / 4226: HMAC-SHA1, 6 Stellen, 30 s
uint32_t totpBerechnen(const uint8_t *secretBytes, int secretLaenge, uint64_t zeitschritt) {
    uint8_t nachricht[8];
    for (int i = 7; i >= 0; i--) {
        nachricht[i] = zeitschritt & 0xFF;
        zeitschritt >>= 8;
    }
    uint8_t hmacErgebnis[20];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mdInfo, 1);
    mbedtls_md_hmac_starts(&ctx, secretBytes, secretLaenge);
    mbedtls_md_hmac_update(&ctx, nachricht, 8);
    mbedtls_md_hmac_finish(&ctx, hmacErgebnis);
    mbedtls_md_free(&ctx);
    int offset = hmacErgebnis[19] & 0x0F;
    uint32_t binaer = ((uint32_t)(hmacErgebnis[offset] & 0x7F) << 24)
                    | ((uint32_t)hmacErgebnis[offset + 1] << 16)
                    | ((uint32_t)hmacErgebnis[offset + 2] << 8)
                    | (uint32_t)hmacErgebnis[offset + 3];
    return binaer % 1000000UL;
}

String totpCodeFuerKonto(const String &secretBase32) {
    uint8_t secretBytes[64];
    int secretLaenge = base32Dekodieren(secretBase32, secretBytes, sizeof(secretBytes));
    if (secretLaenge <= 0) return "------";
    uint64_t zeitschritt = rtcUnixzeit() / 30;
    uint32_t code = totpBerechnen(secretBytes, secretLaenge, zeitschritt);
    char puffer[8];
    snprintf(puffer, sizeof(puffer), "%06u", (unsigned)code);
    return String(puffer);
}

// ============================================================
//  LVGL: Anzeige und Touch
// ============================================================
uint32_t anzeigeBreite, anzeigeHoehe;
lv_display_t *anzeige;
lv_color_t *zeichenpuffer;

static uint32_t millisCallback() { return millis(); }

static void anzeigeFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(disp);
}

static void rundenCallback(lv_event_t *e) {
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 = (a->x1 >> 1) << 1;
    a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1;
    a->y2 = ((a->y2 >> 1) << 1) + 1;
}

static void touchLesen(lv_indev_t *indev, lv_indev_data_t *data) {
    uint8_t n = GT911.getPoint(touchX, touchY, GT911.getSupportTouchPoint());
    // Ist das Display aus: die erste Beruehrung schaltet es nur ein und wird sonst verschluckt
    static bool verschlucken = false;
    if (n > 0 && bildschirmIstAus) {
        bildschirmEinschalten();
        verschlucken = true;
    }
    if (verschlucken) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (n == 0) verschlucken = false;
        return;
    }
    if (n > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touchX[0];
        data->point.y = touchY[0];
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Hilfen
static void dekoObjekt(lv_obj_t *o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
}

static void labelSetzenWennAnders(lv_obj_t *l, const char *txt) {
    const char *alt = lv_label_get_text(l);
    if (alt == NULL || strcmp(alt, txt) != 0) lv_label_set_text(l, txt);
}

static lv_obj_t *zurueckKnopf(lv_obj_t *eltern, const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(eltern);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ============================================================
//  Screens (Zeiger)
// ============================================================
lv_obj_t *hauptmenue_screen = NULL;
lv_obj_t *uhr_screen = NULL;
lv_obj_t *qr_screen = NULL;
lv_obj_t *pin_screen = NULL;
lv_obj_t *pin_anzeige_label, *pin_status_label;
lv_obj_t *pin_einrichten_screen = NULL;
lv_obj_t *pin_einrichten_titel_label, *pin_einrichten_anzeige_label, *pin_einrichten_status_label;
lv_obj_t *totp_menue_screen = NULL;
lv_obj_t *totp_screen = NULL;
lv_obj_t *totp_uebersicht_screen = NULL;
lv_obj_t *totp_label_konto, *totp_label_code, *totp_label_countdown, *totp_balken_countdown;
lv_timer_t *totp_timer = NULL;
uint32_t totpAktuellerIndex = 0;
bool totpAnzeigeNeu = true;
lv_obj_t *konto_hinzufuegen_screen = NULL;
lv_obj_t *konto_hinzufuegen_status_label = NULL;
lv_obj_t *liste_screen[2] = {NULL, NULL}; // [LISTE_UMBENENNEN], [LISTE_LOESCHEN]
lv_obj_t *editor_screen = NULL;
lv_obj_t *editor_textarea = NULL;
uint32_t umbenennenPendingIndex = 0;
uint32_t loeschenPendingIndex = 0;
lv_obj_t *zeit_screen = NULL;
lv_obj_t *zeit_info_label, *zeit_status_label;
lv_obj_t *zeit_roller_tag, *zeit_roller_monat, *zeit_roller_jahr, *zeit_roller_stunde, *zeit_roller_minute;

// Vorwaertsdeklarationen
void erstelleHauptmenue();
void starteUhr();
void beendeUhr();
void zeigeHauptmenue();
void zeigeWebStart();
void zeigeTotpMenue();
void zeigeTotpCodes();
void zeigePinEingabe();
void zeigeKontoListe(ListenModus modus);
void zeigeZeitStellen();
void zeigeQrCode();
void zeigeWetter();
void zeigeWetterStart();
void zeigeBle();
void starteStartbildschirm();
void bildschirmAusschalten();
void starteKontoHinzufuegen();
void zeigeHerunterfahrenBestaetigung();
void totpAnzeigeAktualisieren();
void funkStoppen();

// ============================================================
//  PIN-Eingabe
// ============================================================
char pinPuffer[PIN_LAENGE + 1] = {0};
uint8_t pinPufferLaenge = 0;
static const char *pin_tastenfeld_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "Abbr.", "0", "<-", ""
};
static const char *pin_einrichten_tastenfeld_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    "Reset", "0", "<-", ""
};
char pinEinrichtenErstePin[PIN_LAENGE + 1] = {0};
char pinEinrichtenPuffer[PIN_LAENGE + 1] = {0};
uint8_t pinEinrichtenPhase = 0;
uint8_t pinEinrichtenPufferLaenge = 0;

static void pinMaske(char *ziel, size_t groesse, uint8_t eingegeben) {
    ziel[0] = '\0';
    for (uint8_t i = 0; i < PIN_LAENGE && strlen(ziel) + 3 < groesse; i++) {
        strcat(ziel, (i < eingegeben) ? "* " : "_ ");
    }
}

void pinAnzeigeAktualisieren() {
    char maske[32];
    pinMaske(maske, sizeof(maske), pinPufferLaenge);
    lv_label_set_text(pin_anzeige_label, maske);
}

void pinPruefen() {
    uint32_t restzeit = 0;
    if (istGesperrt(&restzeit)) {
        lv_label_set_text_fmt(pin_status_label, "Gesperrt, noch %u s", (unsigned)restzeit);
        pinPufferLaenge = 0;
        memset(pinPuffer, 0, sizeof(pinPuffer));
        pinAnzeigeAktualisieren();
        return;
    }

    sicherheitSpeicher.begin("sicherheit", true);
    String gespeicherterHash = sicherheitSpeicher.getString("pin_hash", "");
    String salt = sicherheitSpeicher.getString("pin_salt", "");
    uint32_t fehlversuche = sicherheitSpeicher.getUInt("fehlversuche", 0);
    sicherheitSpeicher.end();

    String eingegebenerHash = sha256Hex(salt + String(pinPuffer));
    pinPufferLaenge = 0;
    memset(pinPuffer, 0, sizeof(pinPuffer));

    if (gespeicherterHash.length() > 0 && eingegebenerHash == gespeicherterHash) {
        sicherheitSpeicher.begin("sicherheit", false);
        sicherheitSpeicher.putUInt("fehlversuche", 0);
        sicherheitSpeicher.end();
        lv_label_set_text(pin_status_label, "");
        Serial.println("PIN korrekt.");
        zeigeTotpMenue();
    } else {
        fehlversuche++;
        sicherheitSpeicher.begin("sicherheit", false);
        sicherheitSpeicher.putUInt("fehlversuche", fehlversuche);
        if (fehlversuche >= MAX_FEHLVERSUCHE) {
            uint32_t exponent = fehlversuche < 10 ? fehlversuche : 10;
            uint32_t sperrSekunden = min((uint32_t)300, (uint32_t)(1UL << exponent));
            sicherheitSpeicher.putULong("gesperrt_bis", rtcUnixzeit() + sperrSekunden);
            lv_label_set_text_fmt(pin_status_label, "Zu viele Versuche - gesperrt fuer %u s", (unsigned)sperrSekunden);
        } else {
            lv_label_set_text_fmt(pin_status_label, "Falsche PIN (%u/%u)", (unsigned)fehlversuche, (unsigned)MAX_FEHLVERSUCHE);
        }
        sicherheitSpeicher.end();
        pinAnzeigeAktualisieren();
    }
}

static void pinTastenfeldHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *obj = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    const char *txt = lv_buttonmatrix_get_button_text(obj, id);
    if (txt == NULL) return;

    if (strcmp(txt, "<-") == 0) {
        if (pinPufferLaenge > 0) {
            pinPufferLaenge--;
            pinPuffer[pinPufferLaenge] = '\0';
        }
    } else if (strcmp(txt, "Abbr.") == 0) {
        pinPufferLaenge = 0;
        memset(pinPuffer, 0, sizeof(pinPuffer));
        zeigeHauptmenue();
        return;
    } else if (pinPufferLaenge < PIN_LAENGE) {
        pinPuffer[pinPufferLaenge++] = txt[0];
        pinPuffer[pinPufferLaenge] = '\0';
    }
    pinAnzeigeAktualisieren();
    if (pinPufferLaenge == PIN_LAENGE) pinPruefen();
}

static lv_obj_t *pinTastenfeldErstellen(lv_obj_t *eltern, const char **map, lv_event_cb_t cb) {
    lv_obj_t *t = lv_buttonmatrix_create(eltern);
    lv_buttonmatrix_set_map(t, map);
    lv_obj_set_size(t, 400, 300);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_add_event_cb(t, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return t;
}

void zeigePinEingabe() {
    // Immer neu aufbauen: der Puffer ist dann garantiert leer
    lv_obj_t *alt = pin_screen;
    pin_screen = lv_obj_create(NULL);

    lv_obj_t *titel = lv_label_create(pin_screen);
    lv_label_set_text(titel, "PIN eingeben");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 20);

    pin_anzeige_label = lv_label_create(pin_screen);
    lv_obj_set_style_text_font(pin_anzeige_label, &lv_font_montserrat_32, 0);
    lv_obj_align(pin_anzeige_label, LV_ALIGN_TOP_MID, 0, 70);

    pin_status_label = lv_label_create(pin_screen);
    lv_obj_set_style_text_color(pin_status_label, lv_color_hex(0xFF5050), 0);
    lv_obj_set_style_text_align(pin_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pin_status_label, 440);
    lv_obj_align(pin_status_label, LV_ALIGN_TOP_MID, 0, 120);
    lv_label_set_text(pin_status_label, "");

    pinTastenfeldErstellen(pin_screen, pin_tastenfeld_map, pinTastenfeldHandler);

    pinPufferLaenge = 0;
    memset(pinPuffer, 0, sizeof(pinPuffer));
    pinAnzeigeAktualisieren();

    aktuellerStatus = STATUS_PIN_EINGABE;
    lv_screen_load(pin_screen);
    if (alt) lv_obj_delete_async(alt);
}

// ---------- PIN-Ersteinrichtung ----------
void pinEinrichtenAnzeigeAktualisieren() {
    char maske[32];
    pinMaske(maske, sizeof(maske), pinEinrichtenPufferLaenge);
    lv_label_set_text(pin_einrichten_anzeige_label, maske);
}

void pinEinrichtenZuruecksetzen() {
    pinEinrichtenPhase = 0;
    pinEinrichtenPufferLaenge = 0;
    memset(pinEinrichtenPuffer, 0, sizeof(pinEinrichtenPuffer));
    memset(pinEinrichtenErstePin, 0, sizeof(pinEinrichtenErstePin));
    lv_label_set_text(pin_einrichten_titel_label, "PIN festlegen");
    pinEinrichtenAnzeigeAktualisieren();
}

static void pinEinrichtenHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *obj = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    const char *txt = lv_buttonmatrix_get_button_text(obj, id);
    if (txt == NULL) return;

    if (strcmp(txt, "<-") == 0) {
        if (pinEinrichtenPufferLaenge > 0) {
            pinEinrichtenPufferLaenge--;
            pinEinrichtenPuffer[pinEinrichtenPufferLaenge] = '\0';
        }
    } else if (strcmp(txt, "Reset") == 0) {
        pinEinrichtenZuruecksetzen();
        lv_label_set_text(pin_einrichten_status_label, "");
        return;
    } else if (pinEinrichtenPufferLaenge < PIN_LAENGE) {
        pinEinrichtenPuffer[pinEinrichtenPufferLaenge++] = txt[0];
        pinEinrichtenPuffer[pinEinrichtenPufferLaenge] = '\0';
    }
    pinEinrichtenAnzeigeAktualisieren();
    if (pinEinrichtenPufferLaenge < PIN_LAENGE) return;

    if (pinEinrichtenPhase == 0) {
        strncpy(pinEinrichtenErstePin, pinEinrichtenPuffer, PIN_LAENGE + 1);
        pinEinrichtenPhase = 1;
        pinEinrichtenPufferLaenge = 0;
        memset(pinEinrichtenPuffer, 0, sizeof(pinEinrichtenPuffer));
        lv_label_set_text(pin_einrichten_titel_label, "PIN wiederholen");
        lv_label_set_text(pin_einrichten_status_label, "");
        pinEinrichtenAnzeigeAktualisieren();
    } else if (strncmp(pinEinrichtenPuffer, pinEinrichtenErstePin, PIN_LAENGE) == 0) {
        pinSetzen(String(pinEinrichtenErstePin));
        memset(pinEinrichtenPuffer, 0, sizeof(pinEinrichtenPuffer));
        memset(pinEinrichtenErstePin, 0, sizeof(pinEinrichtenErstePin));
        pinEinrichtenPufferLaenge = 0;
        Serial.println("PIN erfolgreich eingerichtet.");
        if (we.nachStartUhr) starteStartbildschirm(); else zeigeHauptmenue();
    } else {
        pinEinrichtenZuruecksetzen();
        lv_label_set_text(pin_einrichten_status_label, "PINs stimmen nicht ueberein - bitte erneut");
    }
}

void zeigePinEinrichten() {
    pin_einrichten_screen = lv_obj_create(NULL);

    pin_einrichten_titel_label = lv_label_create(pin_einrichten_screen);
    lv_label_set_text(pin_einrichten_titel_label, "PIN festlegen");
    lv_obj_set_style_text_font(pin_einrichten_titel_label, &lv_font_montserrat_28, 0);
    lv_obj_align(pin_einrichten_titel_label, LV_ALIGN_TOP_MID, 0, 20);

    pin_einrichten_anzeige_label = lv_label_create(pin_einrichten_screen);
    lv_obj_set_style_text_font(pin_einrichten_anzeige_label, &lv_font_montserrat_32, 0);
    lv_obj_align(pin_einrichten_anzeige_label, LV_ALIGN_TOP_MID, 0, 70);

    pin_einrichten_status_label = lv_label_create(pin_einrichten_screen);
    lv_obj_set_style_text_color(pin_einrichten_status_label, lv_color_hex(0xFF5050), 0);
    lv_obj_set_style_text_align(pin_einrichten_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pin_einrichten_status_label, 440);
    lv_obj_align(pin_einrichten_status_label, LV_ALIGN_TOP_MID, 0, 120);
    lv_label_set_text(pin_einrichten_status_label, "");

    pinTastenfeldErstellen(pin_einrichten_screen, pin_einrichten_tastenfeld_map, pinEinrichtenHandler);
    pinEinrichtenZuruecksetzen();

    aktuellerStatus = STATUS_PIN_EINRICHTEN;
    lv_screen_load(pin_einrichten_screen);
}

// ============================================================
//  Uhr (Bildschirmschoner)
// ============================================================
const int UHR_MITTE_X = 240;
const int UHR_MITTE_Y = 240;
const float UHR_GESICHT_RADIUS = 215.0f;
const float UHR_SKALIERUNG = UHR_GESICHT_RADIUS / 100.0f;

const float UHR_STUNDE_SPITZE = 60.0f * UHR_SKALIERUNG;
const float UHR_STUNDE_SCHWANZ = 18.0f * UHR_SKALIERUNG; // Verlaengerung hinter der Achse
const float UHR_MINUTE_SPITZE = 94.0f * UHR_SKALIERUNG;
const float UHR_MINUTE_SCHWANZ = 18.0f * UHR_SKALIERUNG; // Verlaengerung hinter der Achse
const float UHR_SEKUNDE_SPITZE = 90.2f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_SCHWANZ = 22.0f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_SCHAFT_UNTEN_ENDE = 42.0f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_SCHAFT_OBEN_START = 58.0f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_RING_ABSTAND = 50.0f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_RING_RADIUS = 8.5f * UHR_SKALIERUNG;
const float UHR_SEKUNDE_RING_DICKE = 6.5f * UHR_SKALIERUNG;

lv_obj_t *uhr_stunden_zeiger, *uhr_minuten_zeiger;
lv_obj_t *uhr_sekunden_zeiger; // Sekundenzeiger: einfache rote Linie
lv_obj_t *uhr_lochzeiger;      // Lochzeiger: ein einziges Objekt, das Schaft und Ring selbst zeichnet
lv_timer_t *uhr_timer = NULL;

lv_point_precise_t uhr_stunden_punkte[2];
lv_point_precise_t uhr_minuten_punkte[2];
lv_point_precise_t uhr_sekunden_punkte[2];
lv_point_precise_t uhr_sekunden_unten_punkte[2];
lv_point_precise_t uhr_sekunden_oben_punkte[2];
lv_point_t uhr_lochzeiger_ring_mitte;
lv_point_precise_t strich_punkte[60][2];

struct tm uhrLetzteZeit;
time_t uhrLetzteUtc = 0;
unsigned long uhrBasisMillis = 0;
bool uhrBasisInitialisiert = false;

static void punktAufKreis(float radius, float winkelGrad, lv_point_precise_t *ziel) {
    float rad = (winkelGrad - 90.0f) * (float)M_PI / 180.0f;
    ziel->x = (lv_value_precise_t)(UHR_MITTE_X + (int)lroundf(radius * cosf(rad)));
    ziel->y = (lv_value_precise_t)(UHR_MITTE_Y + (int)lroundf(radius * sinf(rad)));
}

static void zeigerZeichnen(lv_obj_t *linie, lv_point_precise_t *punkte, float winkel, float spitze, float schwanz) {
    punktAufKreis(schwanz, winkel + 180.0f, &punkte[0]);
    punktAufKreis(spitze, winkel, &punkte[1]);
    lv_line_set_points(linie, punkte, 2);
}

// Zeichnet den Lochzeiger: Schaft unten, Schaft oben und Ring aus EINEM Zeichenvorgang.
// Alle Teile nutzen dieselben ganzzahligen Punkte, deshalb sitzt der Ring exakt auf der Achse.
static void lochzeigerZeichnen(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_color_t rot = lv_color_hex(0xad3e00);

    lv_draw_line_dsc_t linie;
    lv_draw_line_dsc_init(&linie);
    linie.color = rot;
    linie.width = 4;
    linie.opa = LV_OPA_COVER;
    linie.round_start = 0;
    linie.round_end = 0;

    linie.p1 = uhr_sekunden_unten_punkte[0];
    linie.p2 = uhr_sekunden_unten_punkte[1];
    lv_draw_line(layer, &linie);
    linie.p1 = uhr_sekunden_oben_punkte[0];
    linie.p2 = uhr_sekunden_oben_punkte[1];
    lv_draw_line(layer, &linie);

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = rot;
    ring.opa = LV_OPA_COVER;
    ring.center = uhr_lochzeiger_ring_mitte;
    ring.radius = (uint16_t)lroundf(UHR_SEKUNDE_RING_RADIUS + UHR_SEKUNDE_RING_DICKE / 2.0f);
    ring.width = (uint16_t)lroundf(UHR_SEKUNDE_RING_DICKE);
    ring.start_angle = 0;
    ring.end_angle = 360;
    lv_draw_arc(layer, &ring);
}

static void uhrZeigerAktualisieren(const struct tm &t, unsigned long msInnerhalb) {
    float stundenWinkel = (float)(t.tm_hour % 12) * 30.0f + (float)t.tm_min * 0.5f;
    float minutenWinkel = (float)t.tm_min * 6.0f;

    const float stopTime = 1.5f;
    float fact = 60.0f / (60.0f - stopTime);
    float virt = fact * (1000.0f * (float)t.tm_sec + (float)msInnerhalb);
    if (virt > 60000.0f) virt = 60000.0f;
    int virtSek = (int)(virt / 1000.0f);
    float virtMs = fmodf(virt, 1000.0f);
    float sekWinkel = 6.0f * (float)virtSek;
    sekWinkel += 3.0f * (1.0f + cosf((float)M_PI + (float)M_PI * (0.001f * virtMs)));

    zeigerZeichnen(uhr_stunden_zeiger, uhr_stunden_punkte, stundenWinkel, UHR_STUNDE_SPITZE, UHR_STUNDE_SCHWANZ);
    zeigerZeichnen(uhr_minuten_zeiger, uhr_minuten_punkte, minutenWinkel, UHR_MINUTE_SPITZE, UHR_MINUTE_SCHWANZ);

    if (!UHR_SEKUNDENZEIGER_ALS_LOCHZEIGER) {
        zeigerZeichnen(uhr_sekunden_zeiger, uhr_sekunden_punkte, sekWinkel, UHR_SEKUNDE_SPITZE, UHR_SEKUNDE_SCHWANZ);
    } else {
        punktAufKreis(UHR_SEKUNDE_SCHWANZ, sekWinkel + 180.0f, &uhr_sekunden_unten_punkte[0]);
        punktAufKreis(UHR_SEKUNDE_SCHAFT_UNTEN_ENDE, sekWinkel, &uhr_sekunden_unten_punkte[1]);
        punktAufKreis(UHR_SEKUNDE_SCHAFT_OBEN_START, sekWinkel, &uhr_sekunden_oben_punkte[0]);
        punktAufKreis(UHR_SEKUNDE_SPITZE, sekWinkel, &uhr_sekunden_oben_punkte[1]);
        lv_point_precise_t m;
        punktAufKreis(UHR_SEKUNDE_RING_ABSTAND, sekWinkel, &m);
        uhr_lochzeiger_ring_mitte.x = (int32_t)m.x;
        uhr_lochzeiger_ring_mitte.y = (int32_t)m.y;

        // Das Objekt deckt genau den Kasten um den Zeiger ab: nur dieser Bereich wird neu gezeichnet
        int32_t rand = (int32_t)lroundf(UHR_SEKUNDE_RING_RADIUS + UHR_SEKUNDE_RING_DICKE / 2.0f) + 3;
        int32_t x1 = (int32_t)m.x - rand, x2 = (int32_t)m.x + rand;
        int32_t y1 = (int32_t)m.y - rand, y2 = (int32_t)m.y + rand;
        for (int k = 0; k < 2; k++) {
            int32_t ux = (int32_t)uhr_sekunden_unten_punkte[k].x, uy = (int32_t)uhr_sekunden_unten_punkte[k].y;
            int32_t ox = (int32_t)uhr_sekunden_oben_punkte[k].x, oy = (int32_t)uhr_sekunden_oben_punkte[k].y;
            if (ux - 3 < x1) x1 = ux - 3; if (ux + 3 > x2) x2 = ux + 3;
            if (uy - 3 < y1) y1 = uy - 3; if (uy + 3 > y2) y2 = uy + 3;
            if (ox - 3 < x1) x1 = ox - 3; if (ox + 3 > x2) x2 = ox + 3;
            if (oy - 3 < y1) y1 = oy - 3; if (oy + 3 > y2) y2 = oy + 3;
        }
        lv_obj_set_pos(uhr_lochzeiger, x1, y1);
        lv_obj_set_size(uhr_lochzeiger, x2 - x1 + 1, y2 - y1 + 1);
    }
}

static void uhrTimerCallback(lv_timer_t *timer) {
    time_t utc = uhrLetzteUtc;
    rtcLesen(&utc);
    if (!uhrBasisInitialisiert || utc != uhrLetzteUtc) {
        uhrLetzteUtc = utc;
        localtime_r(&utc, &uhrLetzteZeit);
        uhrBasisMillis = millis();
        uhrBasisInitialisiert = true;
    }
    unsigned long ms = millis() - uhrBasisMillis;
    if (ms > 999) ms = 999;
    uhrZeigerAktualisieren(uhrLetzteZeit, ms);
}

static void uhrAntippen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    Serial.println("Uhr angetippt - Hauptmenue.");
    zeigeHauptmenue();
}

static void erstelleUhrScreen() {
    uhr_screen = lv_obj_create(NULL);
    dekoObjekt(uhr_screen);
    lv_obj_add_flag(uhr_screen, LV_OBJ_FLAG_CLICKABLE);
    // Quadratisches Zifferblatt: das ganze Display ist weiss, die Uhr bleibt unveraendert
    lv_obj_set_style_bg_color(uhr_screen, we.quadratisch ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(uhr_screen, LV_OPA_COVER, 0);

    lv_obj_t *dial = lv_obj_create(uhr_screen);
    dekoObjekt(dial);
    lv_obj_set_size(dial, 460, 460);
    lv_obj_center(dial);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dial, (!we.quadratisch || we.quadratRing) ? 4 : 0, 0);
    lv_obj_set_style_border_color(dial, lv_color_black(), 0);

    for (int i = 0; i < 60; i++) {
        float winkel = i * 6.0f;
        float innen;
        int breite;
        if (i % 15 == 0) {
            innen = UHR_GESICHT_RADIUS - 30.0f * UHR_SKALIERUNG;
            breite = 18;
        } else if (i % 5 == 0) {
            innen = UHR_GESICHT_RADIUS - 24.0f * UHR_SKALIERUNG;
            breite = 18;
        } else {
            innen = UHR_GESICHT_RADIUS - 8.0f * UHR_SKALIERUNG;
            breite = 8;
        }
        punktAufKreis(UHR_GESICHT_RADIUS, winkel, &strich_punkte[i][0]);
        punktAufKreis(innen, winkel, &strich_punkte[i][1]);
        lv_obj_t *strich = lv_line_create(uhr_screen);
        lv_obj_remove_flag(strich, LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(strich, strich_punkte[i], 2);
        lv_obj_set_style_line_width(strich, breite, 0);
        lv_obj_set_style_line_color(strich, lv_color_hex(0x444444), 0);
    }

    uhr_stunden_zeiger = lv_line_create(uhr_screen);
    lv_obj_remove_flag(uhr_stunden_zeiger, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(uhr_stunden_zeiger, 22, 0);
    lv_obj_set_style_line_color(uhr_stunden_zeiger, lv_color_hex(0x212121), 0);

    uhr_minuten_zeiger = lv_line_create(uhr_screen);
    lv_obj_remove_flag(uhr_minuten_zeiger, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(uhr_minuten_zeiger, 18, 0);
    lv_obj_set_style_line_color(uhr_minuten_zeiger, lv_color_hex(0x232323), 0);

    const lv_color_t rot = lv_color_hex(0xad3e00);
    if (UHR_SEKUNDENZEIGER_ALS_LOCHZEIGER) {
        uhr_lochzeiger = lv_obj_create(uhr_screen);
        dekoObjekt(uhr_lochzeiger);
        lv_obj_set_style_bg_opa(uhr_lochzeiger, LV_OPA_TRANSP, 0);
        lv_obj_set_size(uhr_lochzeiger, 10, 10);
        lv_obj_add_event_cb(uhr_lochzeiger, lochzeigerZeichnen, LV_EVENT_DRAW_MAIN, NULL);
    } else {
        uhr_sekunden_zeiger = lv_line_create(uhr_screen);
        lv_obj_remove_flag(uhr_sekunden_zeiger, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(uhr_sekunden_zeiger, 4, 0);
        lv_obj_set_style_line_color(uhr_sekunden_zeiger, rot, 0);
    }

    int a = (int)(2.0f * 7.0f * UHR_SKALIERUNG);
    lv_obj_t *mitte = lv_obj_create(uhr_screen);
    dekoObjekt(mitte);
    lv_obj_set_size(mitte, a, a);
    lv_obj_center(mitte);
    lv_obj_set_style_radius(mitte, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mitte, rot, 0);
    lv_obj_set_style_bg_opa(mitte, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(uhr_screen, uhrAntippen, LV_EVENT_CLICKED, NULL);

    uhr_timer = lv_timer_create(uhrTimerCallback, 40, NULL); // 25 Hz
    lv_timer_pause(uhr_timer);
}

void starteUhr() {
    aktuellerStatus = STATUS_UHR_LAEUFT;
    uhrWarAktivVorMenue = true;
    uhrBasisInitialisiert = false;
    lv_screen_load(uhr_screen);
    lv_timer_resume(uhr_timer);
    Serial.println("Analoge Uhr gestartet.");
}

// Wird bei jedem Verlassen der Uhr aufgerufen
static void uhrAnhalten() {
    if (uhr_timer != NULL) lv_timer_pause(uhr_timer);
}

void beendeUhr() {
    Serial.println("Uhr explizit beendet.");
    uhrWarAktivVorMenue = false;
    uhrAnhalten();
    zeigeHauptmenue();
}

// ============================================================
//  Hauptmenue
// ============================================================
lv_obj_t *startmodus_label = NULL;
static void startmodus_label_aktualisieren() {
    if (startmodus_label) lv_label_set_text(startmodus_label, startModus ? "Startbild: Wetter" : "Startbild: Uhr");
}

static void menueHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
        case 1: zeigePinEingabe(); break;
        case 2: starteStartbildschirm(); break;
        case 3: beendeUhr(); break;
        case 4: zeigeQrCode(); break;
        case 5: zeigeHerunterfahrenBestaetigung(); break;
        case 6: zeigeWetter(); break;
        case 8: zeigeBle(); break;
        case 9: zeigeWebStart(); break;
        case 7:
            startModus = startModus ? 0 : 1;
            startModusSpeichern();
            startmodus_label_aktualisieren();
            break;
    }
}

// ============================================================
//  Akku (AXP2101 am I2C-Bus, Adresse 0x34)
// ============================================================
const uint8_t AXP_ADRESSE = 0x34;
lv_obj_t *akku_label = NULL;

static bool axpLesen(uint8_t reg, uint8_t *wert) {
    Wire.beginTransmission(AXP_ADRESSE);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)AXP_ADRESSE, 1) != 1) return false;
    *wert = Wire.read();
    return true;
}

static bool axpSchreiben(uint8_t reg, uint8_t wert) {
    Wire.beginTransmission(AXP_ADRESSE);
    Wire.write(reg);
    Wire.write(wert);
    return Wire.endTransmission() == 0;
}

// Aktualisiert die Akkuanzeige im Hauptmenue
void akkuAnzeigeAktualisieren() {
    if (akku_label == NULL) return;
    uint8_t status1 = 0, status2 = 0, prozent = 0;
    if (!axpLesen(0x00, &status1) || !axpLesen(0x01, &status2)) {
        lv_label_set_text(akku_label, "");
        return;
    }
    bool akkuDa = status1 & 0x08;       // Bit 3: Akku angeschlossen
    bool laedt = ((status2 >> 5) & 0x03) == 0x01; // Bits 6:5 = 01: Akku wird geladen
    if (!akkuDa) {
        lv_label_set_text(akku_label, "Kein Akku");
        return;
    }
    if (!axpLesen(0xA4, &prozent)) return;
    if (prozent > 100) prozent = 100;
    const char *symbol = LV_SYMBOL_BATTERY_EMPTY;
    if (prozent >= 85) symbol = LV_SYMBOL_BATTERY_FULL;
    else if (prozent >= 60) symbol = LV_SYMBOL_BATTERY_3;
    else if (prozent >= 35) symbol = LV_SYMBOL_BATTERY_2;
    else if (prozent >= 10) symbol = LV_SYMBOL_BATTERY_1;
    char text[40];
    snprintf(text, sizeof(text), "%s %u %%%s", symbol, (unsigned)prozent, laedt ? " " LV_SYMBOL_CHARGE : "");
    lv_label_set_text(akku_label, text);
}

// true, wenn eine externe Spannung (USB/Netzteil) anliegt; bei Lesefehler: true
bool netzteilAngeschlossen() {
    uint8_t status1 = 0;
    if (!axpLesen(0x00, &status1)) return true;
    return status1 & 0x20; // Bit 5: VBUS gut
}

static void akkuTimerCallback(lv_timer_t *timer) {
    if (lv_screen_active() == hauptmenue_screen) akkuAnzeigeAktualisieren();
}

// Schaltet das Geraet ueber den AXP2101 wirklich aus (Register 0x10, Bit 0)
void axpAbschalten() {
    uint8_t wert = 0;
    if (axpLesen(0x10, &wert)) axpSchreiben(0x10, wert | 0x01);
}

// Hintergrundbeleuchtung: laut Waveshare haengt sie am AXP2101 (Spannungsausgang DLDO1,
// Register 0x90 Bit 7). Zusaetzlich wird ein schwarzer Bildschirm geladen, damit das Display
// auch dunkel ist, falls die Beleuchtung doch nicht abschaltet.
lv_obj_t *aus_screen = NULL;

void bildschirmAusschalten() {
    if (aus_screen == NULL) {
        aus_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(aus_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(aus_screen, LV_OPA_COVER, 0);
    }
    lv_screen_load(aus_screen);
    lv_refr_now(NULL);
    uint8_t v = 0;
    if (axpLesen(0x90, &v)) axpSchreiben(0x90, v & (uint8_t)~0x80);
    helligkeitPwmPause(true);
    bildschirmIstAus = true;
    Serial.println("Display aus (Wetter-Startbild, Timeout).");
}

void bildschirmEinschalten() {
    uint8_t v = 0;
    if (axpLesen(0x90, &v)) axpSchreiben(0x90, v | 0x80);
    helligkeitPwmPause(false);
    bildschirmIstAus = false;
    zeigeWetterStart(); // schaltet das Wetter-Startbild wieder auf
    lv_display_trigger_activity(NULL);
    Serial.println("Display ein.");
}

void startModusLaden() {
    Preferences p;
    p.begin("einst", true);
    startModus = p.getUChar("start", STARTBILDSCHIRM_WETTER_STANDARD ? 1 : 0);
    p.end();
}

void startModusSpeichern() {
    Preferences p;
    p.begin("einst", false);
    p.putUChar("start", startModus);
    p.end();
}

void starteStartbildschirm() {
    if (startModus == 1) zeigeWetterStart();
    else starteUhr();
}

// ============================================================
//  Bildschirmhelligkeit
//  Die Hintergrundbeleuchtung wird laut Pinbelegung des Boards per PWM an GPIO4 gesteuert
//  (LED-Treiber AP3032). Der Regler im Menue aendert das Tastverhaeltnis dieses Signals.
// ============================================================
#ifndef HELLIGKEIT_REGLER_EINBAUEN
#define HELLIGKEIT_REGLER_EINBAUEN 1
#endif
#ifndef HELLIGKEIT_PIN
#define HELLIGKEIT_PIN 4                  // GPIO der Beleuchtung (PWM)
#endif
#ifndef HELLIGKEIT_PWM_INVERTIERT
#define HELLIGKEIT_PWM_INVERTIERT 1       // beim Waveshare 4B: Beleuchtung ist "aktiv niedrig" (0 = normal)
#endif
#ifndef HELLIGKEIT_PWM_HZ
#define HELLIGKEIT_PWM_HZ 2000
#endif
#if HELLIGKEIT_REGLER_EINBAUEN
static uint8_t helligkeitProzent = 100;
static bool helligkeitBereit = false;
static lv_obj_t *helligkeit_slider = NULL;

static void helligkeitPwm(uint32_t duty) {          // duty 0 bis 1023
    if (!helligkeitBereit) return;
#if HELLIGKEIT_PWM_INVERTIERT
    duty = 1023 - duty;
#endif
    ledcWrite(HELLIGKEIT_PIN, duty);
}

static uint32_t helligkeitDuty(int prozent) {
    if (prozent < 5) prozent = 5;
    if (prozent > 100) prozent = 100;
    float x = prozent / 100.0f;                      // quadratisch: wirkt gleichmaessiger
    uint32_t d = (uint32_t)(x * x * 1023.0f);
    return d < 12 ? 12 : d;
}

static void helligkeitAnwenden(int prozent) { helligkeitPwm(helligkeitDuty(prozent)); }

void helligkeitPwmPause(bool aus) {                  // Display aus/ein (Wetter-Timeout)
    if (aus) helligkeitPwm(0);
    else helligkeitAnwenden(helligkeitProzent);
}

static void helligkeitInit() {
    Preferences p;
    p.begin("einst", false);
    helligkeitProzent = (uint8_t)p.getInt("hell", 100);
    p.end();
    if (helligkeitProzent < 5) helligkeitProzent = 5;
    if (helligkeitProzent > 100) helligkeitProzent = 100;
    helligkeitBereit = ledcAttach(HELLIGKEIT_PIN, HELLIGKEIT_PWM_HZ, 10);
    if (!helligkeitBereit) { Serial.println("Helligkeit: PWM nicht verfuegbar."); return; }
    helligkeitAnwenden(helligkeitProzent);
}

static void helligkeitHandler(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        helligkeitProzent = (uint8_t)lv_slider_get_value(sl);
        helligkeitAnwenden(helligkeitProzent);
    } else if (c == LV_EVENT_RELEASED) {
        Preferences p;
        p.begin("einst", false);
        p.putInt("hell", helligkeitProzent);
        p.end();
    }
}
#else
void helligkeitPwmPause(bool) {}
#endif

void erstelleHauptmenue() {
    hauptmenue_screen = lv_obj_create(NULL);

    akku_label = lv_label_create(hauptmenue_screen);
    lv_obj_align(akku_label, LV_ALIGN_TOP_RIGHT, -20, 8);
    akkuAnzeigeAktualisieren();
    lv_timer_create(akkuTimerCallback, 5000, NULL);

    // Startbild-Anzeige oben links (Uhr oder Wetter)
    startmodus_label = lv_label_create(hauptmenue_screen);
    lv_obj_set_style_text_font(startmodus_label, &lv_font_montserrat_20, 0);
    lv_obj_align(startmodus_label, LV_ALIGN_TOP_LEFT, 20, 8);
    startmodus_label_aktualisieren();

    lv_obj_t *liste = lv_list_create(hauptmenue_screen);
    lv_obj_set_size(liste, 440, 300);
    lv_obj_align(liste, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_font(liste, &lv_font_montserrat_24, 0);

    lv_obj_t *b;
    b = lv_list_add_button(liste, LV_SYMBOL_KEYBOARD, "TOTP-Generator");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)1);
    b = lv_list_add_button(liste, LV_SYMBOL_PLAY, "Startbild starten");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)2);
    b = lv_list_add_button(liste, LV_SYMBOL_STOP, "Startbild beenden");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)3);
    b = lv_list_add_button(liste, LV_SYMBOL_DIRECTORY, "QR-Code anzeigen");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)4);
    b = lv_list_add_button(liste, LV_SYMBOL_WIFI, "Wetter");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)6);
#if BLE_ANRUFANZEIGE_EINBAUEN
    b = lv_list_add_button(liste, LV_SYMBOL_BLUETOOTH, "Anrufanzeige");
    lv_obj_add_event_cb(b, menueHandler, LV_EVENT_CLICKED, (void *)8);
#endif

#if HELLIGKEIT_REGLER_EINBAUEN
    // Helligkeit: Symbol und Regler
    lv_obj_t *hsym = lv_label_create(hauptmenue_screen);
    lv_label_set_text(hsym, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(hsym, &lv_font_montserrat_24, 0);
    lv_obj_align(hsym, LV_ALIGN_BOTTOM_LEFT, 28, -92);
    helligkeit_slider = lv_slider_create(hauptmenue_screen);
    lv_obj_set_size(helligkeit_slider, 340, 14);
    lv_obj_align(helligkeit_slider, LV_ALIGN_BOTTOM_LEFT, 80, -100);
    lv_slider_set_range(helligkeit_slider, 5, 100);
    lv_slider_set_value(helligkeit_slider, helligkeitProzent, LV_ANIM_OFF);
    lv_obj_add_event_cb(helligkeit_slider, helligkeitHandler, LV_EVENT_ALL, NULL);
#endif

    // Untere Symbolleiste: Startbild Uhr/Wetter, Einrichtung per Handy, Herunterfahren
    const char *sym[3] = {LV_SYMBOL_SHUFFLE, LV_SYMBOL_SETTINGS, LV_SYMBOL_POWER};
    intptr_t ids[3] = {7, 9, 5};
    int anzahl = 0;
    const char *sym2[3];
    intptr_t ids2[3];
    for (int i = 0; i < 3; i++) {
#if !WEB_EINRICHTUNG_EINBAUEN
        if (ids[i] == 9) continue;
#endif
        sym2[anzahl] = sym[i];
        ids2[anzahl] = ids[i];
        anzahl++;
    }
    const int breite = 130, abstand = (480 - anzahl * breite) / (anzahl + 1);
    for (int i = 0; i < anzahl; i++) {
        lv_obj_t *knopf = lv_button_create(hauptmenue_screen);
        lv_obj_set_size(knopf, breite, 60);
        lv_obj_align(knopf, LV_ALIGN_BOTTOM_LEFT, abstand + i * (breite + abstand), -12);
        lv_obj_t *t = lv_label_create(knopf);
        lv_label_set_text(t, sym2[i]);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
        lv_obj_center(t);
        lv_obj_add_event_cb(knopf, menueHandler, LV_EVENT_CLICKED, (void *)ids2[i]);
    }
}

void zeigeHauptmenue() {
    funkStoppen();
    uhrAnhalten();
    if (totp_timer != NULL) {
        lv_timer_delete(totp_timer);
        totp_timer = NULL;
    }
    aktuellerStatus = STATUS_HAUPTMENUE;
    akkuAnzeigeAktualisieren();
    lv_screen_load(hauptmenue_screen);
}

// ============================================================
//  QR-Code (vCard)
// ============================================================
static void qrAntippen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeHauptmenue();
}

void zeigeQrCode() {
    if (qr_screen == NULL) {
        qr_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(qr_screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_screen, LV_OPA_COVER, 0);

        lv_obj_t *qr = lv_qrcode_create(qr_screen);
        lv_qrcode_set_size(qr, 440);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_obj_center(qr);
        if (lv_qrcode_update(qr, we.vcard, strlen(we.vcard)) != LV_RESULT_OK) {
            Serial.println("FEHLER: vCard passt nicht in einen QR-Code (zu lang?)");
        }
        lv_obj_add_event_cb(qr_screen, qrAntippen, LV_EVENT_CLICKED, NULL);
    }
    aktuellerStatus = STATUS_QR;
    lv_screen_load(qr_screen);
}

// ============================================================
//  TOTP-Codeansicht
// ============================================================
void totpAnzeigeAktualisieren() {
    static char letzterKonto[96] = "";
    static char letzterCode[16] = "";
    static char letzterRest[48] = "";
    static int32_t letzterBalken = -1;
    if (totpAnzeigeNeu) {
        letzterKonto[0] = letzterCode[0] = letzterRest[0] = '\0';
        letzterBalken = -1;
        totpAnzeigeNeu = false;
    }

    char kontoText[96], codeText[16], restText[48];
    int32_t balken = -1;

    if (!rtcZeitGueltig) {
        snprintf(kontoText, sizeof(kontoText), "Zeit ungueltig!");
        snprintf(codeText, sizeof(codeText), "------");
        snprintf(restText, sizeof(restText), "Bitte im TOTP-Menue die Zeit stellen");
    } else {
        uint32_t anzahl = totpKontenAnzahl();
        if (anzahl == 0) {
            snprintf(kontoText, sizeof(kontoText), "Keine Konten gespeichert");
            snprintf(codeText, sizeof(codeText), "------");
            restText[0] = '\0';
        } else {
            if (totpAktuellerIndex >= anzahl) totpAktuellerIndex = 0;
            String label, secret;
            totpKontoLaden(totpAktuellerIndex, &label, &secret);
            String code = totpCodeFuerKonto(secret);
            uint32_t rest = 30 - (rtcUnixzeit() % 30);
            snprintf(kontoText, sizeof(kontoText), "%s  (%u/%u)", label.c_str(), (unsigned)(totpAktuellerIndex + 1), (unsigned)anzahl);
            snprintf(codeText, sizeof(codeText), "%s", code.c_str());
            snprintf(restText, sizeof(restText), "%u s", (unsigned)rest);
            balken = (int32_t)rest;
        }
    }

    if (strcmp(kontoText, letzterKonto) != 0) { lv_label_set_text(totp_label_konto, kontoText); strcpy(letzterKonto, kontoText); }
    if (strcmp(codeText, letzterCode) != 0) { lv_label_set_text(totp_label_code, codeText); strcpy(letzterCode, codeText); }
    if (strcmp(restText, letzterRest) != 0) { lv_label_set_text(totp_label_countdown, restText); strcpy(letzterRest, restText); }
    if (balken < 0) {
        lv_obj_add_flag(totp_balken_countdown, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(totp_balken_countdown, LV_OBJ_FLAG_HIDDEN);
        if (balken != letzterBalken) { lv_bar_set_value(totp_balken_countdown, balken, LV_ANIM_OFF); }
    }
    letzterBalken = balken;
}

static void totpTimerCallback(lv_timer_t *timer) { totpAnzeigeAktualisieren(); }

static void totpTimerStoppen() {
    if (totp_timer != NULL) {
        lv_timer_delete(totp_timer);
        totp_timer = NULL;
    }
}

// Kontenuebersicht: zeigt nur die Namen, KEINE Codes
static void totpUebersichtZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeTotpMenue();
}

// Detailansicht: Code erst nach Auswahl eines Kontos
static void totpDetailZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    totpTimerStoppen();
    zeigeTotpCodes(); // zurueck zur Uebersicht
}

static void totpDetailZeigen() {
    if (totp_screen == NULL) {
        totp_screen = lv_obj_create(NULL);

        zurueckKnopf(totp_screen, LV_SYMBOL_LEFT " Konten", totpDetailZurueck);

        // Kontoname: eigene Zeile unter dem Zurueck-Knopf, volle Breite, umbrechend (kein Scrollen)
        totp_label_konto = lv_label_create(totp_screen);
        lv_obj_set_width(totp_label_konto, 440);
        lv_label_set_long_mode(totp_label_konto, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(totp_label_konto, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(totp_label_konto, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(totp_label_konto, LV_ALIGN_TOP_MID, 0, 90);

        totp_label_code = lv_label_create(totp_screen);
        lv_obj_set_style_text_font(totp_label_code, &lv_font_montserrat_48, 0);
        lv_obj_align(totp_label_code, LV_ALIGN_CENTER, 0, 30);

        totp_balken_countdown = lv_bar_create(totp_screen);
        lv_bar_set_range(totp_balken_countdown, 0, 30);
        lv_obj_set_size(totp_balken_countdown, 300, 20);
        lv_obj_align(totp_balken_countdown, LV_ALIGN_BOTTOM_MID, 0, -80);

        totp_label_countdown = lv_label_create(totp_screen);
        lv_obj_set_width(totp_label_countdown, 440);
        lv_obj_set_style_text_align(totp_label_countdown, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(totp_label_countdown, totp_balken_countdown, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
    aktuellerStatus = STATUS_TOTP;
    totpAnzeigeNeu = true;
    totpAnzeigeAktualisieren();
    lv_screen_load(totp_screen);
    if (totp_timer == NULL) totp_timer = lv_timer_create(totpTimerCallback, 250, NULL);
}

static void totpKontoGewaehlt(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    totpAktuellerIndex = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    totpDetailZeigen();
}

// Kontenuebersicht (wird bei jedem Aufruf neu aufgebaut, damit Aenderungen sichtbar sind)
void zeigeTotpCodes() {
    totpTimerStoppen();
    lv_obj_t *alt = totp_uebersicht_screen;
    lv_obj_t *neu = lv_obj_create(NULL);
    totp_uebersicht_screen = neu;

    zurueckKnopf(neu, LV_SYMBOL_LEFT " Menue", totpUebersichtZurueck);

    lv_obj_t *titel = lv_label_create(neu);
    lv_obj_align(titel, LV_ALIGN_TOP_RIGHT, -20, 22);

    lv_obj_t *liste = lv_list_create(neu);
    lv_obj_set_size(liste, 440, 380);
    lv_obj_align(liste, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_font(liste, &lv_font_montserrat_24, 0);

    JsonDocument doc = kontenLaden();
    JsonArray konten = doc.as<JsonArray>();
    char titelText[24];
    snprintf(titelText, sizeof(titelText), "Konten: %u", (unsigned)konten.size());
    lv_label_set_text(titel, titelText);
    if (konten.size() == 0) {
        lv_list_add_text(liste, "Keine Konten gespeichert");
    } else {
        uint32_t i = 0;
        for (JsonVariant konto : konten) {
            String label = konto["label"].as<String>();
            // Label ist "Aussteller:Konto" - in der Uebersicht nur den Aussteller zeigen
            int doppelpunkt = label.indexOf(':');
            if (doppelpunkt > 0) {
                String aussteller = label.substring(0, doppelpunkt);
                aussteller.trim();
                if (aussteller.length() > 0) label = aussteller;
            }
            lv_obj_t *eintrag = lv_list_add_button(liste, LV_SYMBOL_RIGHT, label.c_str());
            // lange Namen: Text in der Zeile abschneiden statt umbrechen
            lv_obj_t *txt = lv_obj_get_child(eintrag, 1);
            if (txt) lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_add_event_cb(eintrag, totpKontoGewaehlt, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            i++;
        }
    }

    aktuellerStatus = STATUS_TOTP_MENUE; // in der Uebersicht laeuft kein Timer; Timeout wie im Menue
    lv_screen_load(neu);
    if (alt) lv_obj_delete_async(alt);
}

// ============================================================
//  TOTP-Untermenue
// ============================================================
static void totpMenueHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch (id) {
        case 1: zeigeTotpCodes(); break;
        case 2: starteKontoHinzufuegen(); break;
        case 3: zeigeKontoListe(LISTE_UMBENENNEN); break;
        case 4: zeigeKontoListe(LISTE_LOESCHEN); break;
        case 5: zeigeZeitStellen(); break;
        case 6: zeigeHauptmenue(); break; // verlaesst den PIN-geschuetzten Bereich
    }
}

void zeigeTotpMenue() {
    funkStoppen();
    if (totp_menue_screen == NULL) {
        totp_menue_screen = lv_obj_create(NULL);
        lv_obj_t *liste = lv_list_create(totp_menue_screen);
        lv_obj_set_size(liste, 440, 440);
        lv_obj_center(liste);
        lv_obj_set_style_text_font(liste, &lv_font_montserrat_24, 0);

        lv_obj_t *b;
        b = lv_list_add_button(liste, LV_SYMBOL_EYE_OPEN, "Konten anzeigen");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)1);
        b = lv_list_add_button(liste, LV_SYMBOL_PLUS, "Neues Konto hinzufuegen");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)2);
        b = lv_list_add_button(liste, LV_SYMBOL_EDIT, "Konto umbenennen");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)3);
        b = lv_list_add_button(liste, LV_SYMBOL_TRASH, "Konto loeschen");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)4);
        b = lv_list_add_button(liste, LV_SYMBOL_SETTINGS, "Zeit stellen");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)5);
        b = lv_list_add_button(liste, LV_SYMBOL_LEFT, "Zurueck zum Hauptmenue");
        lv_obj_add_event_cb(b, totpMenueHandler, LV_EVENT_CLICKED, (void *)6);
    }
    aktuellerStatus = STATUS_TOTP_MENUE;
    lv_screen_load(totp_menue_screen);
}

// ============================================================
//  Neues Konto per Funk (ESP-NOW) von der AtomS3R-CAM
// ============================================================
// Die Kamera schickt AES-128-GCM-verschluesselte Pakete ("INFO|..." und "TOTP|Label|Secret").
// Der Funk ist NUR waehrend dieses Menuepunkts eingeschaltet (maximal 60 s).
// Antwort an die Kamera (verschluesselt): "OK", "DUP" (Konto gab es schon) oder "ERR".
static const uint8_t FUNK_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const unsigned long FUNK_TIMEOUT_MS = 60000;
const int FUNK_PUFFER_ANZAHL = 4; // Die Kamera sendet INFO und TOTP kurz hintereinander
static uint8_t funkPuffer[FUNK_PUFFER_ANZAHL][FUNK_MAX_PAKET];
static volatile size_t funkPaketLaenge[FUNK_PUFFER_ANZAHL];
static int funkSchreiben = 0;
static int funkLesen = 0;
static volatile int funkBelegt = 0;
static portMUX_TYPE funkMux = portMUX_INITIALIZER_UNLOCKED;
bool funkAktiv = false;
unsigned long funkStartZeit = 0;
unsigned long funkErfolgZeit = 0;

// Laeuft im WLAN-Task: nur kopieren, ausgewertet wird in loop()
static void funkEmpfangen(const esp_now_recv_info_t *info, const uint8_t *daten, int laenge) {
    if (laenge <= 0 || laenge > (int)FUNK_MAX_PAKET) return;
    portENTER_CRITICAL(&funkMux);
    if (funkBelegt < FUNK_PUFFER_ANZAHL) {
        memcpy(funkPuffer[funkSchreiben], daten, laenge);
        funkPaketLaenge[funkSchreiben] = laenge;
        funkSchreiben = (funkSchreiben + 1) % FUNK_PUFFER_ANZAHL;
        funkBelegt++;
    }
    portEXIT_CRITICAL(&funkMux);
}

static void funkAntwort(const char *text) {
    uint8_t paket[FUNK_MAX_PAKET];
    size_t laenge = funkVerschluesseln(FUNK_SCHLUESSEL, String(text), paket);
    if (laenge > 0) esp_now_send(FUNK_BROADCAST, paket, laenge);
}

static void funkStatus(const char *text) {
    if (konto_hinzufuegen_status_label) lv_label_set_text(konto_hinzufuegen_status_label, text);
}

static void funkStarten() {
    funkErfolgZeit = 0;
    if (!funkSchluesselGesetzt(FUNK_SCHLUESSEL)) {
        funkStatus("Kein Funkschluessel eingetragen\n(konfiguration_funk.h)\n\nTippen = zurueck");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(FUNK_KANAL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        funkStatus("Funk konnte nicht gestartet werden\n\nTippen = zurueck");
        WiFi.mode(WIFI_OFF);
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, FUNK_BROADCAST, 6);
    peer.channel = FUNK_KANAL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    portENTER_CRITICAL(&funkMux);
    funkSchreiben = funkLesen = 0;
    funkBelegt = 0;
    portEXIT_CRITICAL(&funkMux);
    esp_now_register_recv_cb(funkEmpfangen);
    funkAktiv = true;
    funkStartZeit = millis();
    Serial.printf("Funkempfang gestartet (Kanal %d).\n", FUNK_KANAL);
}

void funkStoppen() {
    if (!funkAktiv) return;
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    funkAktiv = false;
    Serial.println("Funkempfang gestoppt.");
}

// Wird aus loop() aufgerufen, solange der Bildschirm "Neues Konto" aktiv ist
static void funkVerarbeiten() {
    if (!funkAktiv) return;
    while (funkBelegt > 0) {
        uint8_t paket[FUNK_MAX_PAKET];
        size_t laenge;
        portENTER_CRITICAL(&funkMux);
        laenge = funkPaketLaenge[funkLesen];
        memcpy(paket, funkPuffer[funkLesen], laenge);
        funkLesen = (funkLesen + 1) % FUNK_PUFFER_ANZAHL;
        funkBelegt--;
        portEXIT_CRITICAL(&funkMux);

        if (funkErfolgZeit != 0) continue; // nach Erfolg nichts mehr annehmen

        String klartext;
        if (!funkEntschluesseln(FUNK_SCHLUESSEL, paket, laenge, &klartext)) {
            Serial.println("Funkpaket verworfen (nicht entschluesselbar).");
            continue;
        }

        if (klartext.startsWith("INFO|")) {
            String text = labelNachAscii(klartext.substring(5));
            if (text.length() > 90) text = text.substring(0, 90);
            funkStatus(text.c_str());
        } else if (klartext.startsWith("TOTP|")) {
            int trenner = klartext.lastIndexOf('|');
            if (trenner <= 5) {
                funkAntwort("ERR");
                funkStatus("Ungueltige Daten - bitte erneut scannen");
                continue;
            }
            String label = klartext.substring(5, trenner);
            String secret = klartext.substring(trenner + 1);
            KontoErgebnis ergebnis = kontoPruefenUndSpeichern(label, secret);
            secret = ""; // Secret nicht laenger als noetig im RAM
            if (ergebnis == KONTO_OK) {
                funkAntwort("OK");
                funkStatus("Konto gespeichert!");
                funkErfolgZeit = millis(); // Funk bleibt kurz an, damit die Antwort sicher rausgeht
            } else if (ergebnis == KONTO_DUPLIKAT) {
                funkAntwort("DUP");
                String hinweis = "Gleicher Schluessel wie vorhandenes Konto: " + duplikatKontoName;
                funkStatus(hinweis.c_str());
            } else {
                funkAntwort("ERR");
                funkStatus("Ungueltiges Konto - bitte erneut scannen");
            }
        }
    }
}

static void kontoHinzufuegenTippen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    Serial.println("Warten auf neues Konto abgebrochen.");
    zeigeTotpMenue(); // beendet auch den Funk
}

void starteKontoHinzufuegen() {
    if (konto_hinzufuegen_screen == NULL) {
        konto_hinzufuegen_screen = lv_obj_create(NULL);
        lv_obj_t *titel = lv_label_create(konto_hinzufuegen_screen);
        lv_label_set_text(titel, "Konto per Kamera scannen");
        lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
        lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 40);

        konto_hinzufuegen_status_label = lv_label_create(konto_hinzufuegen_screen);
        lv_obj_set_width(konto_hinzufuegen_status_label, 420);
        lv_obj_set_style_text_align(konto_hinzufuegen_status_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(konto_hinzufuegen_status_label);
        lv_obj_add_event_cb(konto_hinzufuegen_screen, kontoHinzufuegenTippen, LV_EVENT_CLICKED, NULL);
    }
    funkStatus("Warte auf die Kamera ...\n\n(Tippen zum Abbrechen)");
    aktuellerStatus = STATUS_KONTO_HINZUFUEGEN;
    lv_screen_load(konto_hinzufuegen_screen);
    funkStarten();
}

// ============================================================
//  Konto umbenennen / loeschen (Listen)
// ============================================================
static void listeZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeTotpMenue();
}

// --- Editor (umbenennen) ---
static void editorSchliessen(bool speichern) {
    if (speichern) {
        String neuerName = String(lv_textarea_get_text(editor_textarea));
        neuerName.trim();
        if (neuerName.length() > 0) kontoUmbenennen(umbenennenPendingIndex, neuerName);
    }
    lv_obj_t *alterEditor = editor_screen;
    editor_screen = NULL;
    editor_textarea = NULL;
    zeigeKontoListe(LISTE_UMBENENNEN); // Liste neu aufbauen und anzeigen
    if (alterEditor) lv_obj_delete_async(alterEditor);
}

static void editorHandler(lv_event_t *e) {
    int aktion = (int)(intptr_t)lv_event_get_user_data(e); // 1 = Speichern, 2 = Abbrechen
    editorSchliessen(aktion == 1);
}

static void zeigeEditor(uint32_t index) {
    String label, secret;
    totpKontoLaden(index, &label, &secret);
    secret = ""; // wird hier nicht gebraucht

    editor_screen = lv_obj_create(NULL);

    lv_obj_t *titel = lv_label_create(editor_screen);
    lv_label_set_text(titel, "Konto umbenennen");
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 8);

    editor_textarea = lv_textarea_create(editor_screen);
    lv_textarea_set_one_line(editor_textarea, true);
    lv_textarea_set_max_length(editor_textarea, LABEL_MAX_LAENGE);
    lv_textarea_set_text(editor_textarea, label.c_str());
    lv_obj_set_width(editor_textarea, 440);
    lv_obj_align(editor_textarea, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *abbrBtn = lv_button_create(editor_screen);
    lv_obj_set_size(abbrBtn, 200, 50);
    lv_obj_align(abbrBtn, LV_ALIGN_TOP_LEFT, 20, 100);
    lv_obj_t *abbrLabel = lv_label_create(abbrBtn);
    lv_label_set_text(abbrLabel, "Abbrechen");
    lv_obj_center(abbrLabel);
    lv_obj_add_event_cb(abbrBtn, editorHandler, LV_EVENT_CLICKED, (void *)2);

    lv_obj_t *speichernBtn = lv_button_create(editor_screen);
    lv_obj_set_size(speichernBtn, 200, 50);
    lv_obj_align(speichernBtn, LV_ALIGN_TOP_RIGHT, -20, 100);
    lv_obj_t *speichernLabel = lv_label_create(speichernBtn);
    lv_label_set_text(speichernLabel, "Speichern");
    lv_obj_center(speichernLabel);
    lv_obj_add_event_cb(speichernBtn, editorHandler, LV_EVENT_CLICKED, (void *)1);

    lv_obj_t *tastatur = lv_keyboard_create(editor_screen);
    lv_obj_set_size(tastatur, 480, 300);
    lv_obj_align(tastatur, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(tastatur, editor_textarea);
    lv_obj_add_event_cb(tastatur, editorHandler, LV_EVENT_READY, (void *)1);
    lv_obj_add_event_cb(tastatur, editorHandler, LV_EVENT_CANCEL, (void *)2);

    lv_screen_load(editor_screen);
}

// --- Loeschen: Sicherheitsabfrage ---
static void loeschenAbfrageHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *msgbox = (lv_obj_t *)lv_event_get_user_data(e);
    bool ja = (lv_event_get_target(e) == (void *)lv_obj_get_user_data(msgbox));
    lv_msgbox_close_async(msgbox);
    if (ja) {
        kontoLoeschen(loeschenPendingIndex);
        zeigeKontoListe(LISTE_LOESCHEN);
    }
}

static void loeschenEintragHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    loeschenPendingIndex = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    String label, secret;
    totpKontoLaden(loeschenPendingIndex, &label, &secret);
    secret = "";

    String nachricht = "Konto '" + label + "' wirklich loeschen?";
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Konto loeschen?");
    lv_msgbox_add_text(mbox, nachricht.c_str());
    lv_obj_t *jaBtn = lv_msgbox_add_footer_button(mbox, "Ja, loeschen");
    lv_obj_t *neinBtn = lv_msgbox_add_footer_button(mbox, "Abbrechen");
    lv_obj_set_user_data(mbox, jaBtn);
    lv_obj_add_event_cb(jaBtn, loeschenAbfrageHandler, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(neinBtn, loeschenAbfrageHandler, LV_EVENT_CLICKED, mbox);
    lv_obj_set_width(mbox, 420);
    lv_obj_center(mbox);
}

static void umbenennenEintragHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    umbenennenPendingIndex = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    zeigeEditor(umbenennenPendingIndex);
}

void zeigeKontoListe(ListenModus modus) {
    lv_obj_t *alt = liste_screen[modus];
    lv_obj_t *neu = lv_obj_create(NULL);
    liste_screen[modus] = neu;

    zurueckKnopf(neu, LV_SYMBOL_LEFT " Zurueck", listeZurueck);

    lv_obj_t *titel = lv_label_create(neu);
    lv_label_set_text(titel, modus == LISTE_LOESCHEN ? "Konto loeschen" : "Konto umbenennen");
    lv_obj_align(titel, LV_ALIGN_TOP_RIGHT, -20, 22);

    lv_obj_t *liste = lv_list_create(neu);
    lv_obj_set_size(liste, 440, 380);
    lv_obj_align(liste, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_font(liste, &lv_font_montserrat_24, 0);

    JsonDocument doc = kontenLaden(); // einmal laden, nicht pro Eintrag
    JsonArray konten = doc.as<JsonArray>();
    if (konten.size() == 0) {
        lv_list_add_text(liste, "Keine Konten gespeichert");
    } else {
        uint32_t i = 0;
        for (JsonVariant konto : konten) {
            String label = konto["label"].as<String>();
            lv_obj_t *eintrag = lv_list_add_button(liste,
                                                   modus == LISTE_LOESCHEN ? LV_SYMBOL_TRASH : LV_SYMBOL_EDIT,
                                                   label.c_str());
            lv_obj_add_event_cb(eintrag,
                                modus == LISTE_LOESCHEN ? loeschenEintragHandler : umbenennenEintragHandler,
                                LV_EVENT_CLICKED, (void *)(intptr_t)i);
            i++;
        }
    }

    aktuellerStatus = (modus == LISTE_LOESCHEN) ? STATUS_KONTO_LOESCHEN : STATUS_KONTO_UMBENENNEN;
    lv_screen_load(neu);
    if (alt) lv_obj_delete_async(alt);
}

// ============================================================
//  Zeit stellen
// ============================================================
static String rollerOptionen(int von, int bis, int stellen) {
    String optionen = "";
    for (int i = von; i <= bis; i++) {
        char puffer[8];
        snprintf(puffer, sizeof(puffer), "%0*d", stellen, i);
        optionen += puffer;
        if (i < bis) optionen += "\n";
    }
    return optionen;
}

static void zeitInfoAktualisieren() {
    struct tm lokal;
    bool gueltig = lokaleZeit(&lokal);
    uint32_t letzter = zeitLetzterAbgleich();

    char abgleichText[40];
    if (letzter == 0) {
        snprintf(abgleichText, sizeof(abgleichText), "noch nie");
    } else {
        uint32_t jetztUtc = rtcUnixzeit();
        uint32_t tage = (jetztUtc > letzter) ? (jetztUtc - letzter) / 86400UL : 0;
        if (tage == 0) snprintf(abgleichText, sizeof(abgleichText), "heute");
        else snprintf(abgleichText, sizeof(abgleichText), "vor %lu Tagen", (unsigned long)tage);
    }
    lv_label_set_text_fmt(zeit_info_label, "Aktuell: %02d.%02d.%04d  %02d:%02d:%02d%s\nLetzter Abgleich: %s",
                          lokal.tm_mday, lokal.tm_mon + 1, lokal.tm_year + 1900,
                          lokal.tm_hour, lokal.tm_min, lokal.tm_sec,
                          gueltig ? "" : "  (UNGUELTIG)", abgleichText);
}

static void zeitRollerAufJetztSetzen() {
    struct tm lokal;
    lokaleZeit(&lokal);
    int jahrIndex = lokal.tm_year + 1900 - ZEIT_JAHR_START;
    if (jahrIndex < 0) jahrIndex = 0;
    if (jahrIndex >= ZEIT_JAHR_ANZAHL) jahrIndex = ZEIT_JAHR_ANZAHL - 1;
    lv_roller_set_selected(zeit_roller_tag, lokal.tm_mday - 1, LV_ANIM_OFF);
    lv_roller_set_selected(zeit_roller_monat, lokal.tm_mon, LV_ANIM_OFF);
    lv_roller_set_selected(zeit_roller_jahr, jahrIndex, LV_ANIM_OFF);
    lv_roller_set_selected(zeit_roller_stunde, lokal.tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(zeit_roller_minute, lokal.tm_min, LV_ANIM_OFF);
}

static void zeitZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeTotpMenue();
}

static void zeitUebernehmen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int tagGewaehlt = (int)lv_roller_get_selected(zeit_roller_tag) + 1;
    struct tm lt;
    memset(&lt, 0, sizeof(lt));
    lt.tm_mday = tagGewaehlt;
    lt.tm_mon = (int)lv_roller_get_selected(zeit_roller_monat);
    lt.tm_year = ZEIT_JAHR_START + (int)lv_roller_get_selected(zeit_roller_jahr) - 1900;
    lt.tm_hour = (int)lv_roller_get_selected(zeit_roller_stunde);
    lt.tm_min = (int)lv_roller_get_selected(zeit_roller_minute);
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    time_t utc = mktime(&lt);
    if (lt.tm_mday != tagGewaehlt) {
        lv_label_set_text(zeit_status_label, "Ungueltiges Datum");
        return;
    }
    rtcSetzen(utc);
    lv_label_set_text(zeit_status_label, "Zeit gestellt");
    zeitInfoAktualisieren();
}

static void zeitWlan(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_label_set_text(zeit_status_label, "WLAN-Abgleich laeuft...");
    lv_refr_now(NULL);
    bool ok = zeitPerWlanHolen();
    lv_label_set_text(zeit_status_label, ok ? "Zeit per WLAN abgeglichen" : "Abgleich fehlgeschlagen (kein WLAN/NTP)");
    zeitInfoAktualisieren();
    zeitRollerAufJetztSetzen();
}

void zeigeZeitStellen() {
    lv_obj_t *alt = zeit_screen;
    zeit_screen = lv_obj_create(NULL);

    lv_obj_t *titel = lv_label_create(zeit_screen);
    lv_label_set_text(titel, "Zeit stellen (Ortszeit)");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_24, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 8);

    zeit_info_label = lv_label_create(zeit_screen);
    lv_obj_set_width(zeit_info_label, 460);
    lv_obj_set_style_text_align(zeit_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(zeit_info_label, LV_ALIGN_TOP_MID, 0, 45);

    lv_obj_t **roller[5] = {&zeit_roller_tag, &zeit_roller_monat, &zeit_roller_jahr, &zeit_roller_stunde, &zeit_roller_minute};
    String optionen[5] = {
        rollerOptionen(1, 31, 2), rollerOptionen(1, 12, 2),
        rollerOptionen(ZEIT_JAHR_START, ZEIT_JAHR_START + ZEIT_JAHR_ANZAHL - 1, 4),
        rollerOptionen(0, 23, 2), rollerOptionen(0, 59, 2)};
    const int x[5] = {20, 100, 180, 315, 395};
    const int breite[5] = {70, 70, 105, 70, 70};
    for (int i = 0; i < 5; i++) {
        *roller[i] = lv_roller_create(zeit_screen);
        lv_roller_set_options(*roller[i], optionen[i].c_str(), LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(*roller[i], 3);
        lv_obj_set_pos(*roller[i], x[i], 110);
        lv_obj_set_width(*roller[i], breite[i]);
    }

    lv_obj_t *hinweis = lv_label_create(zeit_screen);
    lv_obj_set_width(hinweis, 460);
    lv_obj_set_style_text_font(hinweis, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(hinweis, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hinweis, "Minute einstellen, dann genau beim Sekundenwechsel\nder Vergleichsuhr auf Uebernehmen druecken");
    lv_obj_align(hinweis, LV_ALIGN_TOP_MID, 0, 235);

    zeit_status_label = lv_label_create(zeit_screen);
    lv_obj_set_width(zeit_status_label, 460);
    lv_obj_set_style_text_align(zeit_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(zeit_status_label, "");
    lv_obj_align(zeit_status_label, LV_ALIGN_TOP_MID, 0, 300);

    const char *beschriftung[3] = {"Uebernehmen", "WLAN", "Zurueck"};
    lv_event_cb_t handler[3] = {zeitUebernehmen, zeitWlan, zeitZurueck};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(zeit_screen);
        lv_obj_set_size(btn, 140, 60);
        lv_obj_set_pos(btn, 15 + i * 155, 390);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, beschriftung[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(btn, handler[i], LV_EVENT_CLICKED, NULL);
    }

    zeitRollerAufJetztSetzen();
    zeitInfoAktualisieren();

    aktuellerStatus = STATUS_ZEIT_STELLEN;
    lv_screen_load(zeit_screen);
    if (alt) lv_obj_delete_async(alt);
}

// ============================================================
//  Wetter-Anzeige
// ============================================================
lv_obj_t *wetter_screen = NULL;
lv_obj_t *wetter_label_temp, *wetter_label_lage, *wetter_label_details, *wetter_label_stand;
lv_obj_t *wetter_zurueck_knopf, *wetter_aktual_knopf;

static void wetterAnzeigeAktualisieren() {
    char text[96];
    if (!wetter.gueltig) {
        lv_label_set_text(wetter_label_temp, "-- Grad");
        lv_label_set_text(wetter_label_lage, "Noch keine Wetterdaten");
        lv_label_set_text(wetter_label_details, "");
        lv_label_set_text(wetter_label_stand, aktuellerStatus == STATUS_WETTER_START ? "Antippen fuer das Menue" : "Mit \"Aktualisieren\" per WLAN holen");
        return;
    }
    snprintf(text, sizeof(text), "%.1f Grad", (double)wetter.temperatur);
    lv_label_set_text(wetter_label_temp, text);
    lv_label_set_text(wetter_label_lage, wetterCodeText(wetter.code));
    snprintf(text, sizeof(text), "Gefuehlt: %.1f Grad\nWind: %.0f km/h\nLuftfeuchte: %d %%",
             (double)wetter.gefuehlt, (double)wetter.wind, wetter.feuchte);
    lv_label_set_text(wetter_label_details, text);
    struct tm t;
    localtime_r(&wetter.abgerufenUtc, &t);
    snprintf(text, sizeof(text), "Stand: %02d.%02d. %02d:%02d Uhr", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
    lv_label_set_text(wetter_label_stand, text);
}

static void wetterAntippen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (aktuellerStatus == STATUS_WETTER_START) zeigeHauptmenue(); // Antippen wie bei der Uhr
}

static void wetterZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeHauptmenue();
}

static void wetterAktualisieren(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_label_set_text(wetter_label_stand, "Hole Wetter per WLAN ...");
    lv_refr_now(NULL);
    bool ok = wlanAbgleich(false, true);
    wetterAnzeigeAktualisieren();
    if (!ok) lv_label_set_text(wetter_label_stand, "Abruf fehlgeschlagen (WLAN/Internet?)");
}

static void wetterScreenErstellen() {
    if (wetter_screen == NULL) {
        wetter_screen = lv_obj_create(NULL);
        wetter_zurueck_knopf = zurueckKnopf(wetter_screen, LV_SYMBOL_LEFT " Menue", wetterZurueck);
        lv_obj_add_event_cb(wetter_screen, wetterAntippen, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ort = lv_label_create(wetter_screen);
        lv_label_set_text(ort, we.ort);
        lv_obj_align(ort, LV_ALIGN_TOP_RIGHT, -20, 22);

        wetter_label_temp = lv_label_create(wetter_screen);
        lv_obj_set_style_text_font(wetter_label_temp, &lv_font_montserrat_48, 0);
        lv_obj_align(wetter_label_temp, LV_ALIGN_CENTER, 0, -100);

        wetter_label_lage = lv_label_create(wetter_screen);
        lv_obj_set_width(wetter_label_lage, 440);
        lv_obj_set_style_text_font(wetter_label_lage, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(wetter_label_lage, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(wetter_label_lage, LV_ALIGN_CENTER, 0, -45);

        wetter_label_details = lv_label_create(wetter_screen);
        lv_obj_set_width(wetter_label_details, 440);
        lv_obj_set_style_text_font(wetter_label_details, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(wetter_label_details, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(wetter_label_details, LV_ALIGN_CENTER, 0, 35);

        wetter_label_stand = lv_label_create(wetter_screen);
        lv_obj_set_width(wetter_label_stand, 440);
        lv_obj_set_style_text_align(wetter_label_stand, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(wetter_label_stand, LV_ALIGN_BOTTOM_MID, 0, -100);

        lv_obj_t *knopf = wetter_aktual_knopf = lv_button_create(wetter_screen);
        lv_obj_set_size(knopf, 280, 56);
        lv_obj_align(knopf, LV_ALIGN_BOTTOM_MID, 0, -25);
        lv_obj_t *knopfText = lv_label_create(knopf);
        lv_label_set_text(knopfText, LV_SYMBOL_REFRESH " Aktualisieren");
        lv_obj_center(knopfText);
        lv_obj_add_event_cb(knopf, wetterAktualisieren, LV_EVENT_CLICKED, NULL);
    }
}

void zeigeWetter() {
    wetterScreenErstellen();
    lv_obj_remove_flag(wetter_zurueck_knopf, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wetter_aktual_knopf, LV_OBJ_FLAG_HIDDEN);
    aktuellerStatus = STATUS_WETTER;
    wetterAnzeigeAktualisieren();
    lv_screen_load(wetter_screen);
}

// Wetter als Startbildschirm: ohne Knoepfe, Antippen oeffnet das Hauptmenue
void zeigeWetterStart() {
    wetterScreenErstellen();
    lv_obj_add_flag(wetter_zurueck_knopf, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wetter_aktual_knopf, LV_OBJ_FLAG_HIDDEN);
    uhrWarAktivVorMenue = true;
    aktuellerStatus = STATUS_WETTER_START;
    wetterAnzeigeAktualisieren();
    lv_screen_load(wetter_screen);
    Serial.println("Wetter-Startbild.");
}

#if BLE_ANRUFANZEIGE_EINBAUEN
// ============================================================
//  Bluetooth-Anrufanzeige (BLE, verschluesselt, nur Anzeige)
//  Das Handy schreibt "RING|SIM|Nummer|Name", "IDLE" bzw. "SMS|Absender|Text" in eine Eigenschaft,
//  die nur nach Kopplung (6-stelliger Code, der hier angezeigt wird) beschreibbar ist.
//  Die BLE-Rueckrufe laufen in einem eigenen Task: sie setzen nur Merker, angezeigt wird in loop().
//  Zwei Arten von Handys: Android (mit der mitgelieferten App) und iPhone (ohne App, ueber ANCS).
// ============================================================

// Vorgaben, falls sie in konfiguration_ble.h nicht stehen (so bleibt die alte Datei weiter gueltig)
#ifndef BLE_SMS_ANZEIGEN
#define BLE_SMS_ANZEIGEN 1        // 1 = eingehende SMS anzeigen, 0 = ignorieren
#endif
#ifndef BLE_SMS_SEKUNDEN
#define BLE_SMS_SEKUNDEN 60       // so lange bleibt eine SMS auf dem Display (Antippen schliesst sofort)
#endif
#define BLE_NACHRICHT_MAX 210     // laengste Nachricht vom Handy in Byte (inkl. Endmarke)

static NimBLEServer *bleServer = NULL;
static NimBLEAdvertising *bleWerbung = NULL;
static bool bleInitialisiert = false;
static bool bleSoll = false;      // Bluetooth ist gerade bereit (Werbung erlaubt)
static bool bleNetzOk = true;     // Netzteil-Bedingung erfuellt (alle 5 s geprueft)
static bool bleAnrufAktiv = BLE_ANRUFANZEIGE_STANDARD;
static bool bleTonAnruf = BLE_TON_STANDARD_ANRUF;   // Ton bei Anruf (Menue-Schalter)
static bool bleTonSms = BLE_TON_STANDARD_SMS;       // Ton bei SMS (Menue-Schalter)
static volatile int bleTonLautstaerke = BLE_TON_LAUTSTAERKE;   // 0 bis 100 (Regler im Ton-Menue)
static volatile bool bleKopplungOffen = false;
static unsigned long bleKopplungEnde = 0;
static portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
static char bleNachricht[BLE_NACHRICHT_MAX];
static volatile bool bleNachrichtNeu = false;
static volatile int32_t blePasskey = -1;
static volatile bool blePasskeyNeu = false;
static volatile bool bleAblehnen = false;
static volatile uint16_t bleHandle = 0xFFFF;
static int32_t blePasskeyAnzeige = -1;
static bool bleIphone = BLE_IPHONE_STANDARD;          // true = iPhone-Modus (ANCS), false = Android-App
static volatile uint32_t bleVerbindungsZaehler = 0;   // zaehlt jede neue Verbindung
static void bleWerbungEinrichten();
static int ancsGapRueckruf(struct ble_gap_event *ev, void *arg);

class BleServerRueckrufe : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
        bleHandle = info.getConnHandle();
        bleVerbindungsZaehler = bleVerbindungsZaehler + 1;
        Serial.println("Bluetooth: Handy verbunden.");
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int grund) override {
        bleHandle = 0xFFFF;
        bleVerbindungsZaehler = bleVerbindungsZaehler + 1;
        Serial.printf("Bluetooth: Verbindung beendet (Grund %d).\n", grund);
    }
    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        Serial.printf("Bluetooth: Sicherheit fertig: verschluesselt=%d, bestaetigt=%d, gekoppelt=%d.\n",
                      info.isEncrypted() ? 1 : 0, info.isAuthenticated() ? 1 : 0, info.isBonded() ? 1 : 0);
    }
    uint32_t onPassKeyDisplay() override {
        uint32_t code = esp_random() % 1000000UL;
        if (!bleKopplungOffen) {      // niemand hat die Kopplung freigegeben: abbrechen
            bleAblehnen = true;
            return code;
        }
        blePasskey = (int32_t)code;
        blePasskeyNeu = true;
        return code;
    }
};

class BleSchreibRueckruf : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        NimBLEAttValue v = c->getValue();
        size_t n = v.length();
        Serial.printf("Bluetooth: %u Byte empfangen.\n", (unsigned)n);
        if (n >= sizeof(bleNachricht)) n = sizeof(bleNachricht) - 1;
        portENTER_CRITICAL(&bleMux);
        memcpy(bleNachricht, v.data(), n);
        bleNachricht[n] = 0;
        bleNachrichtNeu = true;
        portEXIT_CRITICAL(&bleMux);
    }
};

static BleServerRueckrufe bleServerRueckrufe;
static BleSchreibRueckruf bleSchreibRueckruf;

static void bleEinstellungLaden() {
    Preferences p;
    p.begin("einst", true);
    bleAnrufAktiv = p.getBool("ble", BLE_ANRUFANZEIGE_STANDARD);
    bleIphone = p.getBool("ble_ios", BLE_IPHONE_STANDARD);
    bleTonAnruf = p.getBool("tonA", BLE_TON_STANDARD_ANRUF);
    bleTonSms = p.getBool("tonS", BLE_TON_STANDARD_SMS);
    int v = p.getInt("tonV", BLE_TON_LAUTSTAERKE);
    bleTonLautstaerke = v < 0 ? 0 : (v > 100 ? 100 : v);
    p.end();
}

static void bleEinstellungSpeichern() {
    Preferences p;
    p.begin("einst", false);
    p.putBool("ble", bleAnrufAktiv);
    p.putBool("ble_ios", bleIphone);
    p.putBool("tonA", bleTonAnruf);
    p.putBool("tonS", bleTonSms);
    p.putInt("tonV", bleTonLautstaerke);
    p.end();
}

static void bleStarten() {
    if (bleInitialisiert) return;
    NimBLEDevice::init(BLE_GERAETENAME);
    NimBLEDevice::setMTU(247);                                // damit auch laengere Texte in ein Paket passen
    NimBLEDevice::setSecurityAuth(true, true, true);          // Kopplung merken, mit Code, sichere Verbindung
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // Code wird hier angezeigt
    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(&bleServerRueckrufe);
    bleServer->advertiseOnDisconnect(false);                  // Werbung steuert bleVerwalten()
    NimBLEService *dienst = bleServer->createService(BLE_SERVICE_UUID);
    NimBLECharacteristic *anruf = dienst->createCharacteristic(
        BLE_ANRUF_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
    anruf->setCallbacks(&bleSchreibRueckruf);
    dienst->start();
    bleWerbung = NimBLEDevice::getAdvertising();
    bleWerbung->enableScanResponse(true);        // Name kommt in die Scan-Antwort (sonst ist das Paket zu lang)
    bleWerbung->setName(BLE_GERAETENAME);
    bleWerbung->addServiceUUID(BLE_SERVICE_UUID);
    NimBLEDevice::setCustomGapHandler(ancsGapRueckruf);   // Meldungen des iPhones (ANCS) mithoeren
    if (bleIphone) bleWerbungEinrichten();
    bleInitialisiert = true;
    Serial.printf("Bluetooth: gestartet (Name \"%s\").\n", BLE_GERAETENAME);
}

// Nur ASCII anzeigen: Umlaute umschreiben, Sonstiges wird "?"
static void asciiBereinigen(const char *q, char *z, size_t groesse) {
    size_t o = 0;
    for (size_t i = 0; q[i];) {
        uint8_t c = (uint8_t)q[i];
        const char *ers = NULL;
        char einzel[2] = {0, 0};
        if (c < 32 || c == 127) { i++; continue; }
        if (c < 127) {
            einzel[0] = (char)c;
            ers = einzel;
            i++;
        } else if (c == 0xC3 && q[i + 1]) {
            switch ((uint8_t)q[i + 1]) {
                case 0xA4: ers = "ae"; break;
                case 0xB6: ers = "oe"; break;
                case 0xBC: ers = "ue"; break;
                case 0x84: ers = "Ae"; break;
                case 0x96: ers = "Oe"; break;
                case 0x9C: ers = "Ue"; break;
                case 0x9F: ers = "ss"; break;
                default: ers = "?"; break;
            }
            i += 2;
        } else {
            i++;
            while (((uint8_t)q[i] & 0xC0) == 0x80) i++;
            ers = "?";
        }
        while (*ers && o + 1 < groesse) z[o++] = *ers++;
    }
    z[o] = 0;
}

// ============================================================
//  Ton ueber den eingebauten Lautsprecher
//  Audio-Chip ES8311 (I2C-Steuerung, I2S-Daten) und Verstaerker NS4150, dessen Freigabe am
//  Port-Expander (EXIO3) haengt. I2C laeuft nur im Hauptprogramm (loop), der Ton selbst wird in
//  einem eigenen Task ueber I2S ausgegeben, damit die Anzeige nicht ruckelt.
//  Ablauf: tonStarten() schaltet den Verstaerker ein und weckt den Task; ist der Ton zu Ende,
//  schaltet tonVerwalten() (in loop) den Verstaerker wieder aus.
// ============================================================
#define TON_ART_SMS 1
#define TON_ART_ANRUF 2

#if BLE_TON_EINBAUEN
static const uint32_t TON_RATE = 16000;   // Abtastrate; Takt MCLK = 256 x 16000 = 4,096 MHz
// Paare aus Frequenz (Hz, 0 = Pause) und Dauer (ms)
static const uint16_t TON_MUSTER_SMS[] = {988, 110, 0, 30, 1319, 240};
static const uint16_t TON_MUSTER_ANRUF[] = {880, 300, 0, 80, 660, 300, 0, 80, 880, 300, 0, 80, 660, 300, 0, 1600};

static bool tonBereit = false;            // Chip und I2S laufen
static bool tonFehler = false;            // Start gescheitert: nicht dauernd neu versuchen
static bool tonPaAn = false;              // Verstaerker eingeschaltet
static uint8_t es8311Adresse = 0x18;      // 0x18 oder 0x19 (wird beim Start gesucht)
static i2s_chan_handle_t tonKanal = NULL;
static TaskHandle_t tonTaskHandle = NULL;
static volatile bool tonLaeuft = false;   // Task spielt gerade
static volatile bool tonStopp = false;    // Task soll ausblenden und enden
static const uint16_t *tonMuster = NULL;
static int tonAnzahl = 0;
static bool tonWiederholen = false;
static int tonDurchlaeufe = 1;            // wie oft das Muster gespielt wird (ohne Wiederholen bis Ende)
static float tonPhase = 0.0f;             // nur im Task benutzt
static float tonHuelle = 0.0f;            // nur im Task benutzt (weiches Ein-/Ausblenden)

static bool es8311Schreiben(uint8_t reg, uint8_t wert) {
    Wire.beginTransmission(es8311Adresse);
    Wire.write(reg);
    Wire.write(wert);
    return Wire.endTransmission() == 0;
}

static bool es8311Lesen(uint8_t reg, uint8_t *wert) {
    Wire.beginTransmission(es8311Adresse);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)es8311Adresse, 1) != 1) return false;
    *wert = (uint8_t)Wire.read();
    return true;
}

static void tonVerstaerker(bool an) {
    expander->digitalWrite(AUDIO_PA_EXPANDER_PIN, an ? HIGH : LOW);
    tonPaAn = an;
}

static float tonAmplitude() {
    float v = (float)bleTonLautstaerke / 100.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return 30000.0f * v * v;   // quadratisch: die Lautstaerke wirkt gleichmaessiger
}

// Erzeugt bis zu 256 Abtastwerte (links = rechts) und gibt sie ueber I2S aus
static void tonBlock(uint16_t hz, uint32_t frames) {
    int16_t buf[512];
    const float amp = tonAmplitude();
    const float zweiPi = 6.28318530718f;
    const float schritt = hz ? zweiPi * (float)hz / (float)TON_RATE : 0.0f;
    const float huelleSchritt = 1.0f / (0.008f * (float)TON_RATE);   // 8 ms Ein-/Ausblenden
    const float ziel = (hz > 0 && !tonStopp) ? 1.0f : 0.0f;
    if (frames > 256) frames = 256;
    for (uint32_t i = 0; i < frames; i++) {
        if (tonHuelle < ziel) {
            tonHuelle += huelleSchritt;
            if (tonHuelle > ziel) tonHuelle = ziel;
        } else if (tonHuelle > ziel) {
            tonHuelle -= huelleSchritt;
            if (tonHuelle < ziel) tonHuelle = ziel;
        }
        int16_t s = (int16_t)(sinf(tonPhase) * amp * tonHuelle);
        tonPhase += schritt;
        if (tonPhase > zweiPi) tonPhase -= zweiPi;
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }
    size_t geschrieben = 0;
    i2s_channel_write(tonKanal, buf, frames * 4, &geschrieben, 1000);
}

static void tonNote(uint16_t hz, uint16_t ms) {
    uint32_t rest = TON_RATE * (uint32_t)ms / 1000UL;
    while (rest > 0) {
        if (tonStopp && tonHuelle <= 0.0f) break;
        uint32_t n = rest > 256 ? 256 : rest;
        tonBlock(hz, n);
        rest -= n;
    }
}

static void tonAufgabe(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t *m = tonMuster;
        int n = tonAnzahl;
        bool wdh = tonWiederholen;
        int durchlaeufe = tonDurchlaeufe;
        tonHuelle = 0.0f;
        tonBlock(0, 256);   // kurze Stille, damit der Verstaerker einschwingen kann
        tonBlock(0, 256);
        int runde = 0;
        for (;;) {
            for (int i = 0; i + 1 < n && !tonStopp; i += 2) tonNote(m[i], m[i + 1]);
            runde++;
            if (tonStopp || (!wdh && runde >= durchlaeufe)) break;
            if (!wdh) tonNote(0, 350);   // Pause zwischen den Wiederholungen (der Anrufton hat sie im Muster)
        }
        while (tonHuelle > 0.0f) tonBlock(0, 128);
        for (int k = 0; k < 8; k++) tonBlock(0, 256);   // Puffer mit Stille fuellen (sonst wiederholt er den Rest)
        tonLaeuft = false;
    }
}

// Chip einrichten (Werte wie im Treiber von Espressif: Slave-Betrieb, MCLK = 256 x Abtastrate,
// 16 Bit, I2S-Format, nur Wiedergabe). Laeuft im Hauptprogramm, einmalig beim ersten Ton.
static bool tonInitialisieren() {
    if (tonBereit) return true;
    if (tonFehler) return false;
    tonFehler = true;   // bis alles geklappt hat

    // 1. I2S starten: liefert den Takt MCLK, den der Chip zum Einrichten braucht
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;
    if (i2s_new_channel(&cc, &tonKanal, NULL) != ESP_OK) {
        Serial.println("Ton: I2S-Kanal konnte nicht angelegt werden.");
        return false;
    }
    i2s_std_config_t sc;
    memset(&sc, 0, sizeof(sc));
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(TON_RATE);
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.clk_cfg = clk;
    sc.slot_cfg = slot;
    sc.gpio_cfg.mclk = (gpio_num_t)AUDIO_PIN_MCLK;
    sc.gpio_cfg.bclk = (gpio_num_t)AUDIO_PIN_BCLK;
    sc.gpio_cfg.ws = (gpio_num_t)AUDIO_PIN_WS;
    sc.gpio_cfg.dout = (gpio_num_t)AUDIO_PIN_DOUT;
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tonKanal, &sc) != ESP_OK || i2s_channel_enable(tonKanal) != ESP_OK) {
        Serial.println("Ton: I2S konnte nicht gestartet werden.");
        i2s_del_channel(tonKanal);
        tonKanal = NULL;
        return false;
    }
    delay(30);

    // 2. Chip suchen (Adresse 0x18 oder 0x19)
    bool gefunden = false;
    for (uint8_t a = 0x18; a <= 0x19 && !gefunden; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            es8311Adresse = a;
            gefunden = true;
        }
    }
    if (!gefunden) {
        Serial.println("Ton: Audio-Chip ES8311 antwortet nicht (Adresse 0x18/0x19).");
        return false;
    }
    uint8_t id1 = 0, id2 = 0;
    es8311Lesen(0xFD, &id1);
    es8311Lesen(0xFE, &id2);
    Serial.printf("Ton: ES8311 bei 0x%02X, Kennung %02X %02X (erwartet 83 11).\n", es8311Adresse, id1, id2);

    // 3. Einrichten
    bool ok = true;
    ok &= es8311Schreiben(0x00, 0x1F);   // Zuruecksetzen
    delay(20);
    ok &= es8311Schreiben(0x00, 0x00);
    ok &= es8311Schreiben(0x00, 0x80);   // einschalten, Slave-Betrieb (Bit 6 = 0)
    ok &= es8311Schreiben(0x01, 0x3F);   // alle Takte an, Takt von MCLK
    ok &= es8311Schreiben(0x02, 0x00);   // Teiler fuer 4,096 MHz / 16 kHz
    ok &= es8311Schreiben(0x05, 0x00);
    ok &= es8311Schreiben(0x03, 0x10);
    ok &= es8311Schreiben(0x04, 0x10);
    ok &= es8311Schreiben(0x07, 0x00);
    ok &= es8311Schreiben(0x08, 0xFF);
    ok &= es8311Schreiben(0x06, 0x03);
    ok &= es8311Schreiben(0x09, 0x0C);   // Format I2S, 16 Bit (DAC-Eingang), nicht stumm
    ok &= es8311Schreiben(0x0A, 0x0C);   // Format I2S, 16 Bit (ADC-Ausgang)
    ok &= es8311Schreiben(0x0B, 0x00);
    ok &= es8311Schreiben(0x0C, 0x00);
    ok &= es8311Schreiben(0x10, 0x1F);
    ok &= es8311Schreiben(0x11, 0x7F);
    ok &= es8311Schreiben(0x13, 0x10);
    ok &= es8311Schreiben(0x1B, 0x0A);
    ok &= es8311Schreiben(0x1C, 0x6A);
    ok &= es8311Schreiben(0x17, 0xBF);
    ok &= es8311Schreiben(0x0E, 0x02);   // analoge Teile einschalten
    ok &= es8311Schreiben(0x12, 0x00);   // DAC einschalten
    ok &= es8311Schreiben(0x14, 0x1A);
    ok &= es8311Schreiben(0x0D, 0x01);
    ok &= es8311Schreiben(0x15, 0x40);
    ok &= es8311Schreiben(0x37, 0x08);
    ok &= es8311Schreiben(0x45, 0x00);
    ok &= es8311Schreiben(0x31, 0x00);   // DAC nicht stumm
    ok &= es8311Schreiben(0x32, 0xBF);   // DAC-Lautstaerke 0 dB (die Lautstaerke regelt der Sketch)
    if (!ok) {
        Serial.println("Ton: Schreiben ins Register des Audio-Chips fehlgeschlagen.");
        return false;
    }

    // 4. Verstaerker aus lassen, Port-Expander-Pin als Ausgang
    expander->pinMode(AUDIO_PA_EXPANDER_PIN, OUTPUT);
    tonVerstaerker(false);

    // 5. Task
    if (xTaskCreatePinnedToCore(tonAufgabe, "ton", 6144, NULL, 3, &tonTaskHandle, 0) != pdPASS) {
        Serial.println("Ton: Task konnte nicht angelegt werden.");
        return false;
    }
    tonBereit = true;
    tonFehler = false;
    Serial.println("Ton: bereit.");
    return true;
}

// art = TON_ART_SMS oder TON_ART_ANRUF; einmal = true spielt den Anrufton nur einmal (Hoerprobe)
static void tonStarten(uint8_t art, bool einmal) {
    if (!tonInitialisieren()) return;
    if (tonLaeuft) {   // laufenden Ton beenden (ein Anruf soll nicht an einem SMS-Ton scheitern)
        tonStopp = true;
        unsigned long t = millis();
        while (tonLaeuft && millis() - t < 80) delay(2);
        if (tonLaeuft) return;
    }
    if (art == TON_ART_ANRUF) {
        tonMuster = TON_MUSTER_ANRUF;
        tonAnzahl = (int)(sizeof(TON_MUSTER_ANRUF) / sizeof(TON_MUSTER_ANRUF[0]));
        tonWiederholen = (BLE_TON_ANRUF_WIEDERHOLEN != 0) && !einmal;
        tonDurchlaeufe = 1;
    } else {
        tonMuster = TON_MUSTER_SMS;
        tonAnzahl = (int)(sizeof(TON_MUSTER_SMS) / sizeof(TON_MUSTER_SMS[0]));
        tonWiederholen = false;
        tonDurchlaeufe = einmal ? 1 : we.smsAnzahl;
    }
    tonStopp = false;
    if (!tonPaAn) tonVerstaerker(true);
    tonLaeuft = true;
    xTaskNotifyGive(tonTaskHandle);
}

static void tonStoppen() {
    if (tonBereit && tonLaeuft) tonStopp = true;
}

// In jedem loop()-Durchlauf: Verstaerker aus, sobald der Ton zu Ende ist
static void tonVerwalten() {
    if (tonBereit && tonPaAn && !tonLaeuft) tonVerstaerker(false);
}
#else   // Ton nicht eingebaut: leere Platzhalter
static void tonStarten(uint8_t, bool) {}
static void tonStoppen() {}
static void tonVerwalten() {}
#endif  // BLE_TON_EINBAUEN

// ---- Anrufbild ----
static lv_obj_t *anruf_screen = NULL;
static lv_obj_t *anruf_label_name, *anruf_label_nummer, *anruf_label_sim;
static bool anrufAnzeige = false;
static bool anrufVorherAus = false;
static lv_obj_t *anrufVorherScreen = NULL;
static unsigned long anrufEnde = 0;

// SMS-Bild (Aufbau weiter unten); steht hier, damit das Schliessen den Inhalt loeschen kann
static lv_obj_t *sms_screen = NULL;
static lv_obj_t *sms_label_absender = NULL, *sms_label_code = NULL, *sms_label_text = NULL;

static void anrufSchliessen() {
    if (!anrufAnzeige) return;
    anrufAnzeige = false;
    tonStoppen();   // Klingelton beenden (auch beim Antippen oder nach Ablauf der Anzeigedauer)
    if (anrufVorherAus) bildschirmAusschalten();
    else if (anrufVorherScreen) lv_screen_load(anrufVorherScreen);
    if (sms_screen != NULL) {   // SMS-Text nicht im Speicher stehen lassen
        lv_label_set_text(sms_label_absender, "");
        lv_label_set_text(sms_label_code, "");
        lv_label_set_text(sms_label_text, "");
    }
    lv_display_trigger_activity(NULL);
    Serial.println("Anruf-/SMS-Anzeige geschlossen.");
}

static void anrufAntippen(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) anrufSchliessen();
}

static void anrufScreenErstellen() {
    if (anruf_screen != NULL) return;
    anruf_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(anruf_screen, lv_palette_darken(LV_PALETTE_RED, 4), 0);
    lv_obj_set_style_bg_opa(anruf_screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(anruf_screen, anrufAntippen, LV_EVENT_CLICKED, NULL);

    lv_obj_t *titel = lv_label_create(anruf_screen);
    lv_label_set_text(titel, "ANRUF");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_32, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 40);

    anruf_label_name = lv_label_create(anruf_screen);
    lv_obj_set_width(anruf_label_name, 440);
    lv_obj_set_style_text_font(anruf_label_name, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(anruf_label_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(anruf_label_name, LV_ALIGN_CENTER, 0, -40);

    anruf_label_nummer = lv_label_create(anruf_screen);
    lv_obj_set_width(anruf_label_nummer, 440);
    lv_obj_set_style_text_font(anruf_label_nummer, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(anruf_label_nummer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(anruf_label_nummer, LV_ALIGN_CENTER, 0, 70);

    anruf_label_sim = lv_label_create(anruf_screen);
    lv_obj_set_width(anruf_label_sim, 440);
    lv_obj_set_style_text_font(anruf_label_sim, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(anruf_label_sim, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(anruf_label_sim, LV_ALIGN_BOTTOM_MID, 0, -95);

    lv_obj_t *hinweis = lv_label_create(anruf_screen);
    lv_label_set_text(hinweis, "Antippen zum Schliessen");
    lv_obj_align(hinweis, LV_ALIGN_BOTTOM_MID, 0, -35);
}

static void anrufZeigen(int sim, const char *nummer, const char *name) {
    anrufScreenErstellen();
    // Neuer Anruf? (Das Handy meldet manchmal ein zweites Mal mit dem Namen: dann kein neuer Ton)
    bool neuerAnruf = !(anrufAnzeige && lv_screen_active() == anruf_screen);
    if (!anrufAnzeige) {
        anrufVorherAus = bildschirmIstAus;
        anrufVorherScreen = lv_screen_active();
    }
    if (bildschirmIstAus) { // Display war aus: Beleuchtung wieder an (Bild bleibt, wird beim Schliessen wieder dunkel)
        uint8_t v = 0;
        if (axpLesen(0x90, &v)) axpSchreiben(0x90, v | 0x80);
        bildschirmIstAus = false;
    }
    const char *gross = name[0] ? name : (nummer[0] ? nummer : "Unbekannt");
    lv_label_set_text(anruf_label_name, gross);
    lv_label_set_text(anruf_label_nummer, (name[0] && BLE_NUMMER_ANZEIGEN) ? nummer : "");
    char simText[48] = "";
    if (sim == 1 || sim == 2) snprintf(simText, sizeof(simText), "Anruf auf %s", BLE_SIM_BESCHRIFTUNG[sim - 1]);
    lv_label_set_text(anruf_label_sim, simText);
    anrufAnzeige = true;
    anrufEnde = millis() + BLE_ANRUF_MAX_SEKUNDEN * 1000UL;
    lv_screen_load(anruf_screen);
    lv_display_trigger_activity(NULL);
    Serial.printf("Anruf eingehend (SIM %d).\n", sim);
    if (neuerAnruf && bleTonAnruf) tonStarten(TON_ART_ANRUF, false);
}

// ---- SMS-Bild ----
// Nutzt dieselbe Logik wie das Anrufbild: waehrend der Anzeige kein Timeout, beim Schliessen
// (Antippen oder nach BLE_SMS_SEKUNDEN) kommt das vorherige Bild zurueck. Der Text wird weder
// gespeichert noch in der seriellen Ausgabe protokolliert.
static void smsScreenErstellen() {
    if (sms_screen != NULL) return;
    sms_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(sms_screen, lv_palette_darken(LV_PALETTE_BLUE, 4), 0);
    lv_obj_set_style_bg_opa(sms_screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(sms_screen, anrufAntippen, LV_EVENT_CLICKED, NULL);

    lv_obj_t *titel = lv_label_create(sms_screen);
    lv_label_set_text(titel, "SMS");
    lv_obj_set_style_text_color(titel, lv_color_white(), 0);
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_32, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 30);

    sms_label_absender = lv_label_create(sms_screen);
    lv_obj_set_width(sms_label_absender, 440);
    lv_obj_set_style_text_color(sms_label_absender, lv_color_white(), 0);
    lv_obj_set_style_text_font(sms_label_absender, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(sms_label_absender, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sms_label_absender, LV_ALIGN_TOP_MID, 0, 80);

    sms_label_code = lv_label_create(sms_screen);
    lv_obj_set_width(sms_label_code, 440);
    lv_obj_set_style_text_color(sms_label_code, lv_color_white(), 0);
    lv_obj_set_style_text_font(sms_label_code, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(sms_label_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sms_label_code, LV_ALIGN_TOP_MID, 0, 130);

    sms_label_text = lv_label_create(sms_screen);
    lv_obj_set_width(sms_label_text, 440);
    lv_obj_set_style_text_color(sms_label_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(sms_label_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(sms_label_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sms_label_text, LV_ALIGN_TOP_MID, 0, 205);

    lv_obj_t *hinweis = lv_label_create(sms_screen);
    lv_obj_set_style_text_color(hinweis, lv_color_white(), 0);
    lv_label_set_text(hinweis, "Antippen zum Schliessen");
    lv_obj_align(hinweis, LV_ALIGN_BOTTOM_MID, 0, -25);
}

static void smsZeigen(const char *absender, const char *text) {
#if BLE_SMS_ANZEIGEN
    if (anrufAnzeige && lv_screen_active() == anruf_screen) {   // Anruf hat Vorrang
        Serial.println("SMS waehrend eines Anrufs eingegangen: nicht angezeigt.");
        return;
    }
    smsScreenErstellen();
    if (!anrufAnzeige) {
        anrufVorherAus = bildschirmIstAus;
        anrufVorherScreen = lv_screen_active();
    }
    if (bildschirmIstAus) { // Display war aus: Beleuchtung wieder an (wird beim Schliessen wieder dunkel)
        uint8_t v = 0;
        if (axpLesen(0x90, &v)) axpSchreiben(0x90, v | 0x80);
        bildschirmIstAus = false;
    }

    // Einmalcode gross darstellen: erster Block aus 5 bis 8 Ziffern, sonst erster mit 4 Ziffern
    // (so wird "gueltig bis 2026" nicht fuer den Code gehalten, wenn ein laengerer Block da ist)
    char code[12] = "";
    for (int stufe = 0; stufe < 2 && !code[0]; stufe++) {
        size_t minL = stufe == 0 ? 5 : 4, maxL = stufe == 0 ? 8 : 4;
        for (const char *p = text; *p;) {
            if (*p >= '0' && *p <= '9') {
                const char *anfang = p;
                while (*p >= '0' && *p <= '9') p++;
                size_t n = (size_t)(p - anfang);
                if (n >= minL && n <= maxL) {
                    memcpy(code, anfang, n);
                    code[n] = 0;
                    break;
                }
            } else {
                p++;
            }
        }
    }

    char von[28];
    strncpy(von, absender, sizeof(von) - 1);
    von[sizeof(von) - 1] = 0;
    lv_label_set_text(sms_label_absender, von[0] ? von : "Unbekannt");
    lv_label_set_text(sms_label_code, code);
    lv_label_set_text(sms_label_text, text);
    lv_obj_align(sms_label_text, LV_ALIGN_TOP_MID, 0, code[0] ? 205 : 140);

    anrufAnzeige = true;
    anrufEnde = millis() + BLE_SMS_SEKUNDEN * 1000UL;
    lv_screen_load(sms_screen);
    lv_display_trigger_activity(NULL);
    Serial.printf("SMS eingegangen (%u Zeichen).\n", (unsigned)strlen(text));
    if (bleTonSms) tonStarten(TON_ART_SMS, false);
#else
    (void)absender;
    (void)text;
#endif
}

// Nachricht vom Handy: "RING|<sim>|<nummer>|<name>", "IDLE" oder "SMS|<absender>|<text>"
static void bleNachrichtVerarbeiten(const char *roh) {
    char t[BLE_NACHRICHT_MAX];
    asciiBereinigen(roh, t, sizeof(t));
    if (strncasecmp(t, "RING|", 5) == 0) {
        char *sim = t + 5;
        char *nummer = strchr(sim, '|');
        char *name = NULL;
        if (nummer) {
            *nummer++ = 0;
            name = strchr(nummer, '|');
            if (name) *name++ = 0;
        }
        for (char *p = name; p && *p; p++) if (*p == '|') *p = ' ';
        anrufZeigen(atoi(sim), nummer ? nummer : "", name ? name : "");
    } else if (strcasecmp(t, "IDLE") == 0) {
        anrufSchliessen();
    } else if (strncasecmp(t, "SMS|", 4) == 0) {
        char *absender = t + 4;
        char *text = strchr(absender, '|');
        if (text) *text++ = 0;
        smsZeigen(absender, text ? text : "");
    } else {
        char kopf[13];
        strncpy(kopf, t, 12);
        kopf[12] = 0;
        Serial.printf("Bluetooth: Nachricht nicht erkannt (Anfang: \"%s\").\n", kopf);
    }
}

// ---- Einstellungsbild ----
static lv_obj_t *ble_screen = NULL;
static lv_obj_t *ble_schalter, *ble_iphone_schalter, *ble_status_label, *ble_code_label;

static bool bleVerschluesselt() {
    if (!bleInitialisiert || bleServer->getConnectedCount() == 0) return false;
    return bleServer->getPeerInfo(0).isEncrypted();
}

// ============================================================
//  iPhone-Modus: Apple Notification Center Service (ANCS)
//  Das iPhone bietet gekoppeltem Zubehoer seine Benachrichtigungen an. Das Waveshare fragt sie
//  ueber die bestehende Verbindung ab (es ist dabei Client, das iPhone der Server) und reagiert
//  nur auf die Kategorie "Eingehender Anruf". Auf dem iPhone ist dafuer keine App noetig.
//  Die BLE-Rueckrufe laufen im BLE-Task: sie legen nur Daten ab, ausgewertet wird in loop().
// ============================================================
#define ANCS_DIENST_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ANCS_QUELLE_UUID "9FBF120D-6301-42D9-8C58-25E699A21DBD" // Notification Source (meldet neue/entfernte Meldungen)
#define ANCS_STEUER_UUID "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9" // Control Point (Details anfordern)
#define ANCS_DATEN_UUID "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"  // Data Source (liefert die Details)

static volatile bool ancsAktiv = false;           // Handles gueltig, Meldungen werden angenommen
static volatile uint16_t ancsQuelleHandle = 0;
static volatile uint16_t ancsDatenHandle = 0;
static NimBLERemoteCharacteristic *ancsSteuer = NULL;
static uint32_t ancsVerbindung = 0xFFFFFFFFUL;    // fuer welche Verbindung (Zaehler) eingerichtet wird
static int ancsStatus = 0;                        // 0 = nicht eingerichtet, 1 = richtet ein, 2 = bereit,
                                                  // 3 = Dienst nicht gefunden, 4 = Fehler
static unsigned long ancsNaechsterVersuch = 0;

// Meldungen der "Notification Source" (vom BLE-Task gefuellt, in loop() geleert)
static const uint8_t ANCS_WS_GROESSE = 8;
static volatile uint8_t ancsWsLesen = 0, ancsWsSchreiben = 0;
static uint32_t ancsWsUid[ANCS_WS_GROESSE];
static uint8_t ancsWsArt[ANCS_WS_GROESSE];        // 0 = neu, 1 = geaendert, 2 = entfernt
static uint8_t ancsWsKategorie[ANCS_WS_GROESSE];  // 1 = eingehender Anruf
// Antwort der "Data Source" (kann auf mehrere Pakete verteilt sein)
static uint8_t ancsAntwort[300];
static volatile uint16_t ancsAntwortLaenge = 0;

static bool ancsHatAnruf = false;                 // ein Anruf dieser Verbindung ist gerade angezeigt
static uint32_t ancsAnrufUid = 0;
static bool ancsWartet = false;                   // Details zu ancsWartetUid sind angefordert
static uint32_t ancsWartetUid = 0;
static unsigned long ancsWartetSeit = 0;

static void ancsZuruecksetzen() {
    ancsAktiv = false;
    ancsSteuer = NULL;
    ancsStatus = 0;
    ancsWartet = false;
    ancsNaechsterVersuch = 0;
    portENTER_CRITICAL(&bleMux);
    ancsWsLesen = 0;
    ancsWsSchreiben = 0;
    ancsAntwortLaenge = 0;
    portEXIT_CRITICAL(&bleMux);
    if (ancsHatAnruf) {
        ancsHatAnruf = false;
        anrufSchliessen();
    }
}

// Laeuft im BLE-Task: hoert alle Benachrichtigungen des iPhones mit
static int ancsGapRueckruf(struct ble_gap_event *ev, void *) {
    if (ev->type != BLE_GAP_EVENT_NOTIFY_RX || !ancsAktiv) return 0;
    if (ev->notify_rx.conn_handle != bleHandle) return 0;
    uint16_t h = ev->notify_rx.attr_handle;
    uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
    uint8_t p[256];
    if (n > sizeof(p)) n = sizeof(p);
    if (os_mbuf_copydata(ev->notify_rx.om, 0, n, p) != 0) return 0;
    if (h == ancsQuelleHandle) {
        if (n < 8) return 0;
        uint32_t uid;
        memcpy(&uid, p + 4, 4);
        portENTER_CRITICAL(&bleMux);
        uint8_t naechste = (ancsWsSchreiben + 1) % ANCS_WS_GROESSE;
        if (naechste != ancsWsLesen) {
            ancsWsArt[ancsWsSchreiben] = p[0];
            ancsWsKategorie[ancsWsSchreiben] = p[2];
            ancsWsUid[ancsWsSchreiben] = uid;
            ancsWsSchreiben = naechste;
        }
        portEXIT_CRITICAL(&bleMux);
    } else if (h == ancsDatenHandle) {
        portENTER_CRITICAL(&bleMux);
        if ((size_t)ancsAntwortLaenge + n <= sizeof(ancsAntwort)) {
            memcpy(ancsAntwort + ancsAntwortLaenge, p, n);
            ancsAntwortLaenge = ancsAntwortLaenge + n;
        }
        portEXIT_CRITICAL(&bleMux);
    }
    return 0;
}

// Dienst des iPhones suchen und beide Meldungswege einschalten (blockiert kurz, laeuft in loop())
static void ancsEinrichten() {
    ancsStatus = 1;
    NimBLEConnInfo info = bleServer->getPeerInfoByHandle(bleHandle);
    NimBLEClient *c = bleServer->getClient(info);
    if (c == NULL) { ancsStatus = 4; return; }
    NimBLERemoteService *dienst = c->getService(ANCS_DIENST_UUID);
    if (dienst == NULL) {
        ancsStatus = 3;
        Serial.println("ANCS: Dienst am iPhone nicht gefunden (Systembenachrichtigungen teilen eingeschaltet?).");
        return;
    }
    NimBLERemoteCharacteristic *quelle = dienst->getCharacteristic(ANCS_QUELLE_UUID);
    NimBLERemoteCharacteristic *steuer = dienst->getCharacteristic(ANCS_STEUER_UUID);
    NimBLERemoteCharacteristic *daten = dienst->getCharacteristic(ANCS_DATEN_UUID);
    if (quelle == NULL || steuer == NULL || daten == NULL) {
        ancsStatus = 4;
        Serial.println("ANCS: Eigenschaften unvollstaendig.");
        return;
    }
    ancsQuelleHandle = quelle->getHandle();
    ancsDatenHandle = daten->getHandle();
    ancsSteuer = steuer;
    portENTER_CRITICAL(&bleMux);
    ancsWsLesen = 0;
    ancsWsSchreiben = 0;
    ancsAntwortLaenge = 0;
    portEXIT_CRITICAL(&bleMux);
    ancsAktiv = true;
    // Erst die Details, dann die Meldungen einschalten (so geht keine Antwort verloren)
    bool ok1 = daten->subscribe(true, nullptr, true);
    bool ok2 = quelle->subscribe(true, nullptr, true);
    if (!ok1 || !ok2) {
        ancsAktiv = false;
        ancsSteuer = NULL;
        ancsStatus = 4;
        Serial.println("ANCS: Einschalten der Meldungen fehlgeschlagen.");
        return;
    }
    ancsStatus = 2;
    Serial.println("ANCS: bereit, Anrufe werden gemeldet.");
}

// Details (Titel, Untertitel, Nachricht) zu einer Meldung anfordern
static void ancsAnfordern(uint32_t uid) {
    if (ancsSteuer == NULL) return;
    uint8_t k[14] = {0, 0, 0, 0, 0, 1, 64, 0, 2, 64, 0, 3, 64, 0};
    memcpy(k + 1, &uid, 4);
    portENTER_CRITICAL(&bleMux);
    ancsAntwortLaenge = 0;
    portEXIT_CRITICAL(&bleMux);
    ancsWartetUid = uid;
    ancsWartetSeit = millis();
    ancsWartet = true;
    if (!ancsSteuer->writeValue(k, sizeof(k), true)) {
        ancsWartet = false;
        Serial.println("ANCS: Anfrage der Anrufdaten fehlgeschlagen.");
    }
}

// iOS setzt unsichtbare Steuerzeichen um Rufnummern: entfernen, Leerzeichen/Striche vereinheitlichen
static void ancsTextKopie(const uint8_t *q, size_t n, char *z, size_t groesse) {
    char roh[200];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < sizeof(roh);) {
        uint8_t c = q[i];
        if (c == 0xE2 && i + 2 < n && q[i + 1] == 0x80) {
            uint8_t b = q[i + 2];
            if (b == 0x8E || b == 0x8F || (b >= 0xAA && b <= 0xAE)) { i += 3; continue; }   // Richtungsmarken
            if ((b >= 0x80 && b <= 0x8A) || b == 0xAF) { roh[o++] = ' '; i += 3; continue; } // Leerzeichen
            if (b >= 0x90 && b <= 0x95) { roh[o++] = '-'; i += 3; continue; }              // Striche
        }
        if (c == 0xE2 && i + 2 < n && q[i + 1] == 0x81 && q[i + 2] >= 0xA6 && q[i + 2] <= 0xA9) { i += 3; continue; }
        if (c == 0xC2 && i + 1 < n && q[i + 1] == 0xA0) { roh[o++] = ' '; i += 2; continue; }
        roh[o++] = (char)c;
        i++;
    }
    roh[o] = 0;
    asciiBereinigen(roh, z, groesse);
}

static bool ancsIstNummer(const char *s) {
    int ziffern = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') ziffern++;
        else if (!strchr("+-()/*#. ", *s)) return false;
    }
    return ziffern >= 3;
}

// Antwort der Data Source auswerten; zeigt den Anruf, sobald Titel/Untertitel/Nachricht da sind
static void ancsAntwortPruefen() {
    uint8_t d[sizeof(ancsAntwort)];
    uint16_t n;
    portENTER_CRITICAL(&bleMux);
    n = ancsAntwortLaenge;
    memcpy(d, ancsAntwort, n);
    portEXIT_CRITICAL(&bleMux);
    bool abgelaufen = (long)(millis() - ancsWartetSeit) >= 3000;
    char t[3][70];
    t[0][0] = t[1][0] = t[2][0] = 0;
    int gefunden = 0;
    if (n >= 5 && d[0] == 0) {
        uint32_t uid;
        memcpy(&uid, d + 1, 4);
        if (uid != ancsWartetUid) {      // Antwort zu einer anderen Meldung: verwerfen
            portENTER_CRITICAL(&bleMux);
            ancsAntwortLaenge = 0;
            portEXIT_CRITICAL(&bleMux);
            n = 0;
        } else {
            size_t pos = 5;
            while (pos + 3 <= n) {
                uint8_t id = d[pos];
                size_t len = d[pos + 1] | ((size_t)d[pos + 2] << 8);
                if (pos + 3 + len > n) break;   // noch nicht vollstaendig
                if (id >= 1 && id <= 3) {
                    ancsTextKopie(d + pos + 3, len, t[id - 1], sizeof(t[0]));
                    gefunden++;
                }
                pos += 3 + len;
            }
        }
    }
    if (gefunden < 3 && !abgelaufen) return;

    ancsWartet = false;
    const char *titel = t[0], *untertitel = t[1], *nachricht = t[2];
    Serial.printf("ANCS: Titel=\"%s\" Untertitel=\"%s\" Nachricht=\"%s\"\n", titel, untertitel, nachricht);
    const char *nummer = "";
    if (ancsIstNummer(untertitel)) nummer = untertitel;
    else if (ancsIstNummer(nachricht)) nummer = nachricht;
    else if (ancsIstNummer(titel)) nummer = titel;
    const char *name = ancsIstNummer(titel) ? "" : titel;
    ancsHatAnruf = true;
    ancsAnrufUid = ancsWartetUid;
    anrufZeigen(0, nummer, name);
}

static void ancsEreignisseAbarbeiten() {
    for (;;) {
        uint8_t art = 0, kategorie = 0;
        uint32_t uid = 0;
        bool da = false;
        portENTER_CRITICAL(&bleMux);
        if (ancsWsLesen != ancsWsSchreiben) {
            art = ancsWsArt[ancsWsLesen];
            kategorie = ancsWsKategorie[ancsWsLesen];
            uid = ancsWsUid[ancsWsLesen];
            ancsWsLesen = (ancsWsLesen + 1) % ANCS_WS_GROESSE;
            da = true;
        }
        portEXIT_CRITICAL(&bleMux);
        if (!da) break;

        if (art == 2) {                  // Meldung entfernt: Anruf angenommen, beendet oder abgewiesen
            if (ancsWartet && uid == ancsWartetUid) ancsWartet = false;
            if (ancsHatAnruf && uid == ancsAnrufUid) {
                ancsHatAnruf = false;
                anrufSchliessen();
            }
            continue;
        }
        if (kategorie != 1) continue;    // nur "Eingehender Anruf"
        if (ancsHatAnruf && uid == ancsAnrufUid) continue;
        if (ancsWartet && uid == ancsWartetUid) continue;
        ancsAnfordern(uid);
    }
    if (ancsWartet) ancsAntwortPruefen();
}

// Wird in jedem loop()-Durchlauf aus bleVerwalten() aufgerufen (nur im iPhone-Modus)
static void ancsVerwalten(unsigned long jetzt) {
    if (bleHandle == 0xFFFF) {
        if (ancsStatus != 0 || ancsAktiv || ancsHatAnruf) ancsZuruecksetzen();
        ancsVerbindung = 0xFFFFFFFFUL;
        return;
    }
    if (ancsVerbindung != bleVerbindungsZaehler) {   // neue Verbindung
        ancsZuruecksetzen();
        ancsVerbindung = bleVerbindungsZaehler;
    }
    if (ancsStatus == 2) {
        ancsEreignisseAbarbeiten();
        return;
    }
    if (!bleVerschluesselt()) return;                // ANCS gibt es erst nach der Kopplung
    if (ancsNaechsterVersuch != 0 && (long)(jetzt - ancsNaechsterVersuch) < 0) return;
    ancsEinrichten();
    ancsNaechsterVersuch = millis() + 8000UL;
    if (ancsNaechsterVersuch == 0) ancsNaechsterVersuch = 1;
}

// Sichtbarkeit: im iPhone-Modus bietet das Waveshare dem iPhone den Dienst an ("Service Solicitation"),
// nur dann zeigt iOS es in Einstellungen > Bluetooth an. Im Android-Modus wie bisher die eigene Kennung.
static void bleWerbungEinrichten() {
    if (bleWerbung == NULL) return;
    bleWerbung->stop();
    NimBLEAdvertisementData daten;
    daten.setFlags(BLE_HS_ADV_F_DISC_GEN);
    if (bleIphone) {
        static const uint8_t anfrage[18] = {0x11, BLE_HS_ADV_TYPE_SOL_UUIDS128,
                                            0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
                                            0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79}; // ANCS-Kennung, rueckwaerts
        daten.addData(anfrage, sizeof(anfrage));
    } else {
        daten.addServiceUUID(BLE_SERVICE_UUID);
    }
    NimBLEAdvertisementData antwort;
    antwort.setName(BLE_GERAETENAME);
    bleWerbung->setAdvertisementData(daten);
    bleWerbung->setScanResponseData(antwort);
}

// Nach dem Umschalten Android <-> iPhone: Verbindungen trennen, neu sichtbar machen
static void bleModusUebernehmen() {
    ancsZuruecksetzen();
    bleKopplungOffen = false;
    blePasskeyAnzeige = -1;
    if (!bleInitialisiert) return;
    if (bleWerbung) bleWerbung->stop();
    for (int i = (int)bleServer->getConnectedCount() - 1; i >= 0; i--) bleServer->disconnect(bleServer->getPeerInfo(i));
    bleWerbungEinrichten();
    Serial.println(bleIphone ? "Bluetooth: iPhone-Modus." : "Bluetooth: Android-Modus.");
}

static void bleBildAktualisieren() {
    if (ble_screen == NULL || lv_screen_active() != ble_screen) return;
    char t[140];
    if (!bleAnrufAktiv) {
        snprintf(t, sizeof(t), "Aus");
    } else if (!bleNetzOk) {
        snprintf(t, sizeof(t), "Wartet auf Netzteil (Akkubetrieb)");
    } else if (!bleInitialisiert) {
        snprintf(t, sizeof(t), "Starte ...");
    } else if (bleVerschluesselt()) {
        if (!bleIphone) {
            snprintf(t, sizeof(t), "Handy verbunden (verschluesselt)");
        } else if (ancsStatus == 2) {
            snprintf(t, sizeof(t), "iPhone verbunden\nAnrufe werden gemeldet");
        } else if (ancsStatus == 3) {
            snprintf(t, sizeof(t), "iPhone verbunden, aber keine Meldungen\nAm iPhone \"Systembenachrichtigungen teilen\" einschalten");
        } else if (ancsStatus == 4) {
            snprintf(t, sizeof(t), "iPhone verbunden, Abfrage fehlgeschlagen\n(wird wiederholt)");
        } else {
            snprintf(t, sizeof(t), "iPhone verbunden (verschluesselt)\nrichte Anrufmeldung ein ...");
        }
    } else if (bleKopplungOffen) {
        long diff = (long)(bleKopplungEnde - millis());
        long rest = diff > 0 ? diff / 1000L : 0;
        if (bleServer->getConnectedCount() > 0)
            snprintf(t, sizeof(t), "Kopplung offen (noch %ld s)\nAm Handy jetzt \"Koppeln\" waehlen", rest);
        else if (bleIphone)
            snprintf(t, sizeof(t), "Kopplung offen (noch %ld s)\nAm iPhone Einstellungen > Bluetooth:\n\"%s\" waehlen", rest, BLE_GERAETENAME);
        else
            snprintf(t, sizeof(t), "Kopplung offen (noch %ld s)\nAm Handy \"%s\" waehlen", rest, BLE_GERAETENAME);
    } else if (bleServer->getConnectedCount() > 0) {
        snprintf(t, sizeof(t), "Handy verbunden, nicht gekoppelt\n\"Kopplung starten\" antippen");
    } else {
        snprintf(t, sizeof(t), "Bereit - %d gekoppelte Geraete", NimBLEDevice::getNumBonds());
    }
    labelSetzenWennAnders(ble_status_label, t);
    if (bleKopplungOffen && blePasskeyAnzeige >= 0 && !bleVerschluesselt()) {
        snprintf(t, sizeof(t), "%06ld", (long)blePasskeyAnzeige);
    } else {
        t[0] = 0;
    }
    labelSetzenWennAnders(ble_code_label, t);
}

static void bleTimerCallback(lv_timer_t *) { bleBildAktualisieren(); }

static void bleZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bleKopplungOffen = false;
    blePasskeyAnzeige = -1;
    zeigeHauptmenue();
}

static void bleSchalterHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bleAnrufAktiv = lv_obj_has_state(ble_schalter, LV_STATE_CHECKED);
    bleEinstellungSpeichern();
    if (!bleAnrufAktiv) { bleKopplungOffen = false; blePasskeyAnzeige = -1; }
    bleBildAktualisieren();
}

static void bleIphoneSchalterHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool neu = lv_obj_has_state(ble_iphone_schalter, LV_STATE_CHECKED);
    if (neu == bleIphone) return;
    bleIphone = neu;
    bleEinstellungSpeichern();
    bleModusUebernehmen();
    bleBildAktualisieren();
}

#if BLE_TON_EINBAUEN
// ---- Ton-Einstellungen (Unterbild des Anrufanzeige-Menues) ----
// Zwei Schalter (Anruf, SMS), ein Lautstaerkeregler und zwei Knoepfe zum Anhoeren.
static lv_obj_t *ton_screen = NULL;
static lv_obj_t *ton_anruf_schalter = NULL, *ton_sms_schalter = NULL, *ton_regler = NULL, *ton_wert_label = NULL;

void zeigeBle();   // weiter unten

static void tonZurueck(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeBle();
}

// Schalter: beim Einschalten gibt es eine kurze Hoerprobe
static void tonSchalterHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *ziel = lv_event_get_target_obj(e);
    bleTonAnruf = lv_obj_has_state(ton_anruf_schalter, LV_STATE_CHECKED);
    bleTonSms = lv_obj_has_state(ton_sms_schalter, LV_STATE_CHECKED);
    bleEinstellungSpeichern();
    if (ziel == ton_sms_schalter && bleTonSms) tonStarten(TON_ART_SMS, true);
    else if (ziel == ton_anruf_schalter && bleTonAnruf) tonStarten(TON_ART_ANRUF, true);
}

// Regler: Zahl laeuft beim Ziehen mit, beim Loslassen wird gespeichert und der SMS-Ton zur Probe gespielt
static void tonReglerHandler(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        bleTonLautstaerke = (int)lv_slider_get_value(ton_regler);
        lv_label_set_text_fmt(ton_wert_label, "%d %%", (int)bleTonLautstaerke);
    } else if (c == LV_EVENT_RELEASED) {
        bleEinstellungSpeichern();
        tonStarten(TON_ART_SMS, true);
    }
}

static void tonTestAnrufKnopf(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) tonStarten(TON_ART_ANRUF, true);
}

static void tonTestSmsKnopf(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) tonStarten(TON_ART_SMS, true);
}

static void tonScreenErstellen() {
    if (ton_screen != NULL) return;
    ton_screen = lv_obj_create(NULL);
    zurueckKnopf(ton_screen, LV_SYMBOL_LEFT " Anrufanzeige", tonZurueck);

    lv_obj_t *titel = lv_label_create(ton_screen);
    lv_label_set_text(titel, "Ton");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_RIGHT, -20, 18);

    lv_obj_t *zeileA = lv_label_create(ton_screen);
    lv_label_set_text(zeileA, "Ton bei Anruf");
    lv_obj_set_style_text_font(zeileA, &lv_font_montserrat_24, 0);
    lv_obj_align(zeileA, LV_ALIGN_TOP_LEFT, 30, 100);
    ton_anruf_schalter = lv_switch_create(ton_screen);
    lv_obj_align(ton_anruf_schalter, LV_ALIGN_TOP_RIGHT, -30, 96);
    lv_obj_add_event_cb(ton_anruf_schalter, tonSchalterHandler, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *zeileS = lv_label_create(ton_screen);
    lv_label_set_text(zeileS, "Ton bei SMS");
    lv_obj_set_style_text_font(zeileS, &lv_font_montserrat_24, 0);
    lv_obj_align(zeileS, LV_ALIGN_TOP_LEFT, 30, 160);
    ton_sms_schalter = lv_switch_create(ton_screen);
    lv_obj_align(ton_sms_schalter, LV_ALIGN_TOP_RIGHT, -30, 156);
    lv_obj_add_event_cb(ton_sms_schalter, tonSchalterHandler, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *zeileL = lv_label_create(ton_screen);
    lv_label_set_text(zeileL, "Lautstaerke");
    lv_obj_set_style_text_font(zeileL, &lv_font_montserrat_24, 0);
    lv_obj_align(zeileL, LV_ALIGN_TOP_LEFT, 30, 225);
    ton_wert_label = lv_label_create(ton_screen);
    lv_obj_set_style_text_font(ton_wert_label, &lv_font_montserrat_24, 0);
    lv_obj_align(ton_wert_label, LV_ALIGN_TOP_RIGHT, -30, 225);
    ton_regler = lv_slider_create(ton_screen);
    lv_obj_set_width(ton_regler, 380);
    lv_obj_align(ton_regler, LV_ALIGN_TOP_MID, 0, 285);
    lv_slider_set_range(ton_regler, 0, 100);
    lv_obj_add_event_cb(ton_regler, tonReglerHandler, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ton_regler, tonReglerHandler, LV_EVENT_RELEASED, NULL);

    lv_obj_t *k1 = lv_button_create(ton_screen);
    lv_obj_set_size(k1, 320, 52);
    lv_obj_align(k1, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_t *t1 = lv_label_create(k1);
    lv_label_set_text(t1, LV_SYMBOL_PLAY " Anrufton anhoeren");
    lv_obj_center(t1);
    lv_obj_add_event_cb(k1, tonTestAnrufKnopf, LV_EVENT_CLICKED, NULL);

    lv_obj_t *k2 = lv_button_create(ton_screen);
    lv_obj_set_size(k2, 320, 52);
    lv_obj_align(k2, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_t *t2 = lv_label_create(k2);
    lv_label_set_text(t2, LV_SYMBOL_PLAY " SMS-Ton anhoeren");
    lv_obj_center(t2);
    lv_obj_add_event_cb(k2, tonTestSmsKnopf, LV_EVENT_CLICKED, NULL);
}

static void zeigeTonEinstellungen() {
    tonScreenErstellen();
    aktuellerStatus = STATUS_BLE;
    if (bleTonAnruf) lv_obj_add_state(ton_anruf_schalter, LV_STATE_CHECKED);
    else lv_obj_remove_state(ton_anruf_schalter, LV_STATE_CHECKED);
    if (bleTonSms) lv_obj_add_state(ton_sms_schalter, LV_STATE_CHECKED);
    else lv_obj_remove_state(ton_sms_schalter, LV_STATE_CHECKED);
    lv_slider_set_value(ton_regler, bleTonLautstaerke, LV_ANIM_OFF);
    lv_label_set_text_fmt(ton_wert_label, "%d %%", (int)bleTonLautstaerke);
    lv_screen_load(ton_screen);
}

static void bleTonOeffnen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    zeigeTonEinstellungen();
}
#endif  // BLE_TON_EINBAUEN

static void bleKopplungStarten(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!bleAnrufAktiv || !bleSoll) {
        labelSetzenWennAnders(ble_status_label, "Erst einschalten (und Netzteil anschliessen)");
        return;
    }
    blePasskeyAnzeige = -1;
    bleKopplungEnde = millis() + BLE_KOPPLUNG_SEKUNDEN * 1000UL;
    bleKopplungOffen = true;
    bleBildAktualisieren();
}

static void bleKopplungenLoeschen(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!bleInitialisiert) bleStarten();
    for (int i = (int)bleServer->getConnectedCount() - 1; i >= 0; i--) bleServer->disconnect(bleServer->getPeerInfo(i));
    NimBLEDevice::deleteAllBonds();
    bleKopplungOffen = false;
    blePasskeyAnzeige = -1;
    Serial.println("Bluetooth: alle Kopplungen geloescht.");
    bleBildAktualisieren();
}

static void bleScreenErstellen() {
    if (ble_screen != NULL) return;
    ble_screen = lv_obj_create(NULL);
    zurueckKnopf(ble_screen, LV_SYMBOL_LEFT " Menue", bleZurueck);

    lv_obj_t *titel = lv_label_create(ble_screen);
    lv_label_set_text(titel, "Anrufanzeige");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_RIGHT, -20, 18);

    lv_obj_t *zeile = lv_label_create(ble_screen);
    lv_label_set_text(zeile, "Bluetooth-Anzeige");
    lv_obj_set_style_text_font(zeile, &lv_font_montserrat_24, 0);
    lv_obj_align(zeile, LV_ALIGN_TOP_LEFT, 30, 100);
    ble_schalter = lv_switch_create(ble_screen);
    lv_obj_align(ble_schalter, LV_ALIGN_TOP_RIGHT, -30, 96);
    lv_obj_add_event_cb(ble_schalter, bleSchalterHandler, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *zeile2 = lv_label_create(ble_screen);
    lv_label_set_text(zeile2, "iPhone (ohne App)");
    lv_obj_set_style_text_font(zeile2, &lv_font_montserrat_24, 0);
    lv_obj_align(zeile2, LV_ALIGN_TOP_LEFT, 30, 150);
    ble_iphone_schalter = lv_switch_create(ble_screen);
    lv_obj_align(ble_iphone_schalter, LV_ALIGN_TOP_RIGHT, -30, 146);
    lv_obj_add_event_cb(ble_iphone_schalter, bleIphoneSchalterHandler, LV_EVENT_VALUE_CHANGED, NULL);

#if BLE_TON_EINBAUEN
    lv_obj_t *kt = lv_button_create(ble_screen);
    lv_obj_set_size(kt, 320, 42);
    lv_obj_align(kt, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_t *tt = lv_label_create(kt);
    lv_label_set_text(tt, LV_SYMBOL_VOLUME_MAX " Ton-Einstellungen");
    lv_obj_center(tt);
    lv_obj_add_event_cb(kt, bleTonOeffnen, LV_EVENT_CLICKED, NULL);
#endif

    ble_status_label = lv_label_create(ble_screen);
    lv_obj_set_width(ble_status_label, 440);
    lv_obj_set_style_text_align(ble_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ble_status_label, LV_ALIGN_TOP_MID, 0, BLE_TON_EINBAUEN ? 244 : 200);

    ble_code_label = lv_label_create(ble_screen);
    lv_obj_set_style_text_font(ble_code_label, &lv_font_montserrat_48, 0);
    lv_obj_align(ble_code_label, LV_ALIGN_TOP_MID, 0, BLE_TON_EINBAUEN ? 290 : 268);

    lv_obj_t *k1 = lv_button_create(ble_screen);
    lv_obj_set_size(k1, 320, 52);
    lv_obj_align(k1, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_t *t1 = lv_label_create(k1);
    lv_label_set_text(t1, LV_SYMBOL_BLUETOOTH " Kopplung starten");
    lv_obj_center(t1);
    lv_obj_add_event_cb(k1, bleKopplungStarten, LV_EVENT_CLICKED, NULL);

    lv_obj_t *k2 = lv_button_create(ble_screen);
    lv_obj_set_size(k2, 320, 52);
    lv_obj_align(k2, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_t *t2 = lv_label_create(k2);
    lv_label_set_text(t2, LV_SYMBOL_TRASH " Kopplungen loeschen");
    lv_obj_center(t2);
    lv_obj_add_event_cb(k2, bleKopplungenLoeschen, LV_EVENT_CLICKED, NULL);

    lv_timer_create(bleTimerCallback, 500, NULL);
}

void zeigeBle() {
    bleScreenErstellen();
    aktuellerStatus = STATUS_BLE;
    if (bleAnrufAktiv) lv_obj_add_state(ble_schalter, LV_STATE_CHECKED);
    else lv_obj_remove_state(ble_schalter, LV_STATE_CHECKED);
    if (bleIphone) lv_obj_add_state(ble_iphone_schalter, LV_STATE_CHECKED);
    else lv_obj_remove_state(ble_iphone_schalter, LV_STATE_CHECKED);
    lv_screen_load(ble_screen);
    bleBildAktualisieren();
}

// Wird in jedem loop()-Durchlauf aufgerufen
static void bleVerwalten() {
    unsigned long jetzt = millis();

    if (anrufAnzeige && (long)(jetzt - anrufEnde) >= 0) anrufSchliessen();
    tonVerwalten();

    if (bleNachrichtNeu) {
        char kopie[sizeof(bleNachricht)];
        portENTER_CRITICAL(&bleMux);
        memcpy(kopie, bleNachricht, sizeof(kopie));
        bleNachrichtNeu = false;
        portEXIT_CRITICAL(&bleMux);
        bleNachrichtVerarbeiten(kopie);
    }
    if (blePasskeyNeu) {
        blePasskeyAnzeige = blePasskey;
        blePasskeyNeu = false;
    }
    if (bleAblehnen) {
        bleAblehnen = false;
        if (bleInitialisiert && bleHandle != 0xFFFF) bleServer->disconnect(bleHandle);
    }

    // Ist die Kopplung offen und das Handy verbunden, fordert das Waveshare die Kopplung selbst an
    // (sonst wartet es, bis das Handy damit anfaengt). Pro Verbindung nur einmal.
    static uint16_t sicherheitFuer = 0xFFFF;
    if (bleHandle == 0xFFFF) {
        sicherheitFuer = 0xFFFF;
    } else if ((bleKopplungOffen || (bleIphone && NimBLEDevice::getNumBonds() > 0)) && bleInitialisiert &&
               bleHandle != sicherheitFuer && !bleVerschluesselt()) {
        sicherheitFuer = bleHandle;
        bool ok = NimBLEDevice::startSecurity(bleHandle);
        Serial.println(ok ? "Bluetooth: Kopplung angefordert." : "Bluetooth: Kopplung konnte nicht angefordert werden.");
    }

    if (bleIphone && bleInitialisiert) ancsVerwalten(jetzt);

    static unsigned long letzte = 0;
    if (letzte != 0 && jetzt - letzte < 1000) return;
    letzte = jetzt ? jetzt : 1;

    static unsigned long letzteNetzpruefung = 0;
    if (letzteNetzpruefung == 0 || jetzt - letzteNetzpruefung >= 5000) {
        letzteNetzpruefung = jetzt ? jetzt : 1;
        bleNetzOk = !BLE_NUR_MIT_NETZTEIL || netzteilAngeschlossen();
    }

    bool soll = bleAnrufAktiv && bleNetzOk;
    if (soll && !bleSoll) {
        bleStarten();
        bleSoll = true;
        Serial.println("Bluetooth: Anrufanzeige bereit.");
    } else if (!soll && bleSoll) {
        bleSoll = false;
        ancsZuruecksetzen();
        bleKopplungOffen = false;
        blePasskeyAnzeige = -1;
        if (bleWerbung) bleWerbung->stop();
        for (int i = (int)bleServer->getConnectedCount() - 1; i >= 0; i--) bleServer->disconnect(bleServer->getPeerInfo(i));
        Serial.println("Bluetooth: aus.");
    }
    if (bleSoll && bleServer->getConnectedCount() == 0 && !bleWerbung->isAdvertising()) {
        bool ok = bleWerbung->start();
        Serial.println(ok ? "Bluetooth: sichtbar (Werbung an)." : "Bluetooth: FEHLER beim Starten der Werbung.");
    }

    // Kopplung: nach Erfolg oder Ablauf schliessen; ausserhalb des Einstellungsbilds nie offen lassen
    if (bleKopplungOffen) {
        if (bleVerschluesselt()) {
            bleKopplungOffen = false;
            blePasskeyAnzeige = -1;
            Serial.println("Bluetooth: Handy gekoppelt.");
        } else if ((long)(jetzt - bleKopplungEnde) >= 0 || aktuellerStatus != STATUS_BLE) {
            bleKopplungOffen = false;
            blePasskeyAnzeige = -1;
        }
    }
}

#else  // Anrufanzeige nicht eingebaut: leere Platzhalter
static bool anrufAnzeige = false;
static void bleEinstellungLaden() {}
static void bleVerwalten() {}
void zeigeBle() {}
#endif  // BLE_ANRUFANZEIGE_EINBAUEN

// ============================================================
//  Herunterfahren
// ============================================================
bool herunterfahrenAngefordert = false;

void systemWirklichHerunterfahren() {
    Serial.println("Fahre System sicher herunter...");
    uhrAnhalten();
    if (totp_timer != NULL) { lv_timer_delete(totp_timer); totp_timer = NULL; }

    lv_obj_clean(lv_screen_active());
    lv_obj_t *hinweis = lv_label_create(lv_screen_active());
    lv_label_set_text(hinweis, "Das Geraet schaltet sich aus.");
    lv_obj_center(hinweis);
    lv_refr_now(NULL);
    delay(3000);

    gfx->fillScreen(0x0000); // Display dunkel
    delay(100);
    axpAbschalten();         // Strom wirklich abschalten (AXP2101)
    delay(1000);
    // Falls der Akkuchip nicht abschaltet (z.B. bei angestecktem USB): Tiefschlaf als Rueckfall
    esp_deep_sleep_start();
}

static void herunterfahrenHandler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *msgbox = (lv_obj_t *)lv_event_get_user_data(e);
    bool ja = (lv_event_get_target(e) == (void *)lv_obj_get_user_data(msgbox));
    lv_msgbox_close_async(msgbox);
    if (ja) herunterfahrenAngefordert = true; // loop() fuehrt es aus, nachdem die Abfrage weg ist
}

void zeigeHerunterfahrenBestaetigung() {
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "System herunterfahren?");
    lv_msgbox_add_text(mbox, "Das Geraet schaltet sich aus.\nZum Einschalten die Ein-/Aus-Taste druecken oder Strom anlegen.");
    lv_obj_t *jaBtn = lv_msgbox_add_footer_button(mbox, "Ja, herunterfahren");
    lv_obj_t *neinBtn = lv_msgbox_add_footer_button(mbox, "Abbrechen");
    lv_obj_set_user_data(mbox, jaBtn);
    lv_obj_add_event_cb(jaBtn, herunterfahrenHandler, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(neinBtn, herunterfahrenHandler, LV_EVENT_CLICKED, mbox);
    lv_obj_set_width(mbox, 420);
    lv_obj_center(mbox);
}

// ============================================================
//  Einrichtung per Weboberflaeche (WLAN nur auf Wunsch, einige Minuten)
// ============================================================
#include "web_einrichtung.h"

// ============================================================
//  Inaktivitaets-Timeout
// ============================================================
void zuStartzustandZurueckkehren() {
    if (uhrWarAktivVorMenue) starteStartbildschirm();
    else zeigeHauptmenue();
}

void aktualisiereTimeout() {
    Serial.println("2 Minuten Inaktivitaet erreicht!");
    if (aktuellerStatus == STATUS_PIN_EINGABE) {
        pinPufferLaenge = 0;
        memset(pinPuffer, 0, sizeof(pinPuffer));
    }
    if (totp_timer != NULL) { lv_timer_delete(totp_timer); totp_timer = NULL; }
    zuStartzustandZurueckkehren();
}

// ============================================================
//  Setup / Loop
// ============================================================
void speicherBericht(const char *wann) {
    Serial.printf("[Speicher %s] Heap frei: %u KB (groesster Block %u KB) | PSRAM frei: %u von %u KB | Sketch: %u KB\n",
                  wann,
                  (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024),
                  (unsigned)(ESP.getSketchSize() / 1024));
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("Bahnhofsuhr + TOTP Waveshare - Weitergabe");

    weLaden();
    setenv("TZ", we.tz, 1);
    tzset();

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
#if HELLIGKEIT_REGLER_EINBAUEN
    helligkeitInit();                  // gespeicherte Helligkeit setzen
#endif

    // Display-Reset ueber den Port-Expander (wie im Waveshare-Demo)
    expander->pinMode(5, OUTPUT);
    expander->pinMode(6, OUTPUT);
    expander->digitalWrite(6, LOW);
    delay(200);
    expander->digitalWrite(5, LOW);
    delay(200);
    expander->digitalWrite(5, HIGH);
    delay(200);

    if (!gfx->begin()) {
        Serial.println("FEHLER: Display (PSRAM auf OPI gestellt?)");
    }
    gfx->fillScreen(RGB565_BLACK);

    GT911.setPins(-1, -1);
    if (!GT911.begin(Wire, GT911_SLAVE_ADDRESS_L, I2C_SDA, I2C_SCL)) {
        Serial.println("FEHLER: Touch GT911 nicht gefunden.");
    } else {
        GT911.setMaxTouchPoint(1);
    }

    // RTC pruefen
    time_t utc = 0;
    bool rtcOsOk = rtcLesen(&utc);
    rtcVorhanden = (utc > 0);
    rtcZeitGueltig = rtcOsOk && utc >= ZEIT_PLAUSIBEL_AB;
    if (!rtcVorhanden) Serial.println("Keine RTC gefunden!");
    if (!rtcZeitGueltig) Serial.println("RTC-Zeit ungueltig - bitte Zeit stellen oder per WLAN holen.");

    dateisystemInitialisieren();

    // Zeit beim Start per WLAN holen (Meldung direkt aufs Display)
    if (we.zeitBeimStart) {
        gfx->setTextColor(RGB565_WHITE);
        gfx->setTextSize(3);
        gfx->setCursor(30, 220);
        gfx->print(we.wetterBeimStart ? "Hole Zeit und Wetter ..." : "Hole Zeit per WLAN ...");
        gfx->flush();
        bool ok = wlanAbgleich(true, we.wetterBeimStart);
        gfx->fillScreen(RGB565_BLACK);
        if (!ok && !rtcZeitGueltig) {
            gfx->setCursor(30, 200);
            gfx->print("ZEIT UNGUELTIG!");
            gfx->setCursor(30, 240);
            gfx->print("Bitte im TOTP-Menue stellen.");
            gfx->flush();
            delay(2500);
            gfx->fillScreen(RGB565_BLACK);
        }
    }

    lv_init();
    lv_tick_set_cb(millisCallback);

    anzeigeBreite = gfx->width();
    anzeigeHoehe = gfx->height();
    // Zeichenpuffer: Ein ganzes Bild im PSRAM. LVGL malt dann jeden geaenderten Bereich komplett
    // fertig und schickt ihn in EINEM Rutsch aufs Display - kein sichtbares Hinterherlaufen
    // einzelner Streifen (z.B. beim Sekundenzeiger). Faellt das aus, gibt es den kleinen RAM-Puffer.
    uint32_t pufferZeilen = anzeigeHoehe;
    uint32_t pufferBytes = anzeigeBreite * pufferZeilen * 2;
    zeichenpuffer = ZEICHENPUFFER_IM_PSRAM
        ? (lv_color_t *)heap_caps_malloc(pufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : NULL;
    if (!zeichenpuffer) {
        pufferZeilen = 40;
        pufferBytes = anzeigeBreite * pufferZeilen * 2;
        zeichenpuffer = (lv_color_t *)heap_caps_malloc(pufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!zeichenpuffer) zeichenpuffer = (lv_color_t *)heap_caps_malloc(pufferBytes, MALLOC_CAP_8BIT);
    Serial.printf("Zeichenpuffer: %u Zeilen (%s)\n", (unsigned)pufferZeilen, pufferZeilen == anzeigeHoehe ? "PSRAM, ganzes Bild" : "RAM, Streifen");
    if (!zeichenpuffer) {
        Serial.println("FEHLER: kein Zeichenpuffer.");
        return;
    }
    anzeige = lv_display_create(anzeigeBreite, anzeigeHoehe);
    lv_display_set_flush_cb(anzeige, anzeigeFlush);
    lv_display_set_buffers(anzeige, zeichenpuffer, NULL, pufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(anzeige, rundenCallback, LV_EVENT_INVALIDATE_AREA, NULL);

    // Dunkles Erscheinungsbild, Standardschrift 20 px (gut mit dem Finger bedienbar)
    lv_theme_t *theme = lv_theme_default_init(anzeige, lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_RED), true, &lv_font_montserrat_20);
    lv_display_set_theme(anzeige, theme);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchLesen);

    startModusLaden();
    bleEinstellungLaden();
    erstelleUhrScreen();
    erstelleHauptmenue();

    if (!pinIstEingerichtet()) {
        Serial.println("Noch keine PIN eingerichtet - zeige Einrichtungs-Dialog.");
        zeigePinEinrichten();
    } else if (we.nachStartUhr) {
        starteStartbildschirm();
    } else {
        zeigeHauptmenue();
    }

    speicherBericht("Start");
}

// ============================================================
//  Bildschirmfoto per USB (Befehl "S" ueber die serielle Schnittstelle)
//  Gegenstelle: bildschirmfoto.py auf dem PC. Braucht LV_USE_SNAPSHOT 1 in lv_conf.h.
// ============================================================
static void bildschirmfotoSenden() {
#if LV_USE_SNAPSHOT
    lv_draw_buf_t *bild = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (bild == NULL) {
        Serial.println("SCREENSHOT-FEHLER: Speicher");
        return;
    }
    uint32_t b = bild->header.w, h = bild->header.h, schritt = bild->header.stride;
    while (Serial.available()) Serial.read();     // doppelte Anforderungen ("S") verwerfen
    Serial.printf("SCREENSHOT %u %u\n", (unsigned)b, (unsigned)h);
    Serial.flush();
    delay(300);                       // dem PC Zeit zum Umschalten der Geschwindigkeit geben
    Serial.updateBaudRate(921600);
    delay(300);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(2000);      // USB: nicht nach 100 ms Daten verwerfen, wenn der PC gerade nicht liest
#endif
    // Zeilenweise senden; write() wartet selbst, bis im Sendepuffer wieder Platz ist
    // (bei USB dank des verlaengerten Timeouts oben).
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *zeile = bild->data + y * schritt;
        uint32_t laenge = b * 2, gesendet = 0;
        unsigned long t0 = millis();
        while (gesendet < laenge && millis() - t0 < 5000) {
            size_t n = Serial.write(zeile + gesendet, laenge - gesendet);
            if (n > 0) { gesendet += n; t0 = millis(); }
            else delay(1);
        }
        if (gesendet < laenge) break;      // PC liest nicht mehr: abbrechen statt haengen
    }
    Serial.flush();
    delay(300);
    Serial.updateBaudRate(115200);
    lv_draw_buf_destroy(bild);
#else
    Serial.println("SCREENSHOT-FEHLER: in lv_conf.h LV_USE_SNAPSHOT auf 1 setzen");
#endif
}

static void serielleBefehle() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c == 'S' || c == 's') bildschirmfotoSenden();
    }
}

// Holt das Wetter automatisch im eingestellten Abstand (nur bei Netzteil, wenn so eingestellt)
static void wetterAutomatischAktualisieren() {
    static bool init = false;
    static unsigned long letzterVersuch = 0;
    const unsigned long intervall = (unsigned long)we.wetterMinuten * 60000UL;
    if (we.wetterMinuten <= 0) return;
    if (!init) {
        init = true;
        letzterVersuch = millis();
        if (!wetter.gueltig) letzterVersuch -= intervall; // noch keine Daten: gleich holen
    }
    if (millis() - letzterVersuch < intervall) return;
    if (we.wetterNurNetz && !netzteilAngeschlossen()) return; // Akkubetrieb: spaeter wieder pruefen
    letzterVersuch = millis();
    Serial.println("Wetter: automatische Aktualisierung ...");
    wlanAbgleich(false, true);
    wetterAnzeigeAktualisieren();
}

void loop() {
    static unsigned long letzterSpeicherBericht = 0;
    if (millis() - letzterSpeicherBericht > 300000UL) {
        letzterSpeicherBericht = millis();
        speicherBericht("Betrieb");
    }

    lv_timer_handler();
    delay(5);
    serielleBefehle();

    if (herunterfahrenAngefordert) {
        lv_timer_handler(); // schliesst die Abfrage endgueltig
        systemWirklichHerunterfahren();
    }

    // Bluetooth-Anrufanzeige: waehrend eines Anrufs kein Timeout, kein Wetter-Abruf
    bleVerwalten();
    webVerwalten();                             // Einrichtung per Handy (nur wenn gestartet)
    if (aktuellerStatus == STATUS_WEB) return;  // waehrend der Einrichtung kein Timeout, kein Wetter
    if (anrufAnzeige) return;

    // Waehrend der Ersteinrichtung gibt es keinen sinnvollen Timeout-Zielzustand.
    if (aktuellerStatus == STATUS_PIN_EINRICHTEN) return;

    // Wetter als Startbild: kein normaler Timeout; nach 2 Minuten optional Display aus
    if (aktuellerStatus == STATUS_WETTER_START || bildschirmIstAus) {
        wetterAutomatischAktualisieren();
        if (we.wetterAusTimeout && !bildschirmIstAus &&
            lv_display_get_inactive_time(NULL) >= TIMEOUT_ZEIT) {
            bildschirmAusschalten();
        }
        return;
    }

    // Neues Konto: eigener, kuerzerer Timeout (60 s), damit der Funk nicht vergessen wird.
    if (aktuellerStatus == STATUS_KONTO_HINZUFUEGEN) {
        funkVerarbeiten();
        if (funkAktiv && funkErfolgZeit == 0 && millis() - funkStartZeit > FUNK_TIMEOUT_MS) {
            Serial.println("Warten auf neues Konto: Timeout.");
            zeigeTotpMenue();
        } else if (funkErfolgZeit != 0 && millis() - funkErfolgZeit > 2000) {
            zeigeTotpMenue();
        }
        return;
    }

    bool inaktiv = lv_display_get_inactive_time(NULL) >= TIMEOUT_ZEIT;
    if (inaktiv && !timeoutBereitsBehandelt) {
        timeoutBereitsBehandelt = true;
        if (aktuellerStatus != STATUS_UHR_LAEUFT) aktualisiereTimeout();
    } else if (!inaktiv) {
        timeoutBereitsBehandelt = false;
    }
}
