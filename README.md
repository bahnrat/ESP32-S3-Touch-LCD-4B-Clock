# Bahnhofsuhr + TOTP-Generator für das Waveshare ESP32-S3-Touch-LCD-4B

*In short (English):* A stand-alone station clock with a PIN-protected, offline TOTP (2FA) code generator for the
Waveshare ESP32-S3-Touch-LCD-4B (480×480 touch display). Optional extras: weather, QR-code business card,
encrypted ESP-NOW account import from an AtomS3R-CAM scanner, and a Bluetooth call/SMS display with sound
fed by an Android app. Documentation is in German. Licence: GPL-3.0.

Eine Bahnhofsuhr, die zugleich ein **PIN-geschützter TOTP-Generator ohne Internet** ist. Läuft auf dem
**Waveshare ESP32-S3-Touch-LCD-4B** (480×480, Touch).

## Was es kann

- **Bahnhofsuhr** mit schleichendem Sekundenzeiger (wahlweise Lochzeiger oder einfacher sekundenzeiger), quadratischem oder rundem Zifferblatt,
- **Schweizer Kellenzeiger** und Telenorma-Logo sind aus lizenzrechtlichen Gründen nicht enthalten.
- **TOTP-Codes** (Zwei-Faktor) offline; Konten anzeigen, umbenennen, löschen; Schutz durch **PIN**
- **Zeit stellen**, automatischer Abgleich per WLAN/NTP beim Start, Echtzeituhr (PCF85063)
- **QR-Code** mit einer vCard (Visitenkarte)
- **Wetter** (Open-Meteo, ohne Schlüssel) als alternativer Startbildschirm
- **Herunterfahren** und Akku-/Netzteil-Erkennung
- **Neue Konten per Funk**: eine AtomS3R-CAM scannt den QR-Code des Kontos und sendet ihn **verschlüsselt**
  (AES-128-GCM über ESP-NOW) an die Anzeige – siehe [docs/KAMERA.md](docs/KAMERA.md)
- **Bluetooth-Anrufanzeige** (verschlüsselt gekoppelt): eingehende **Anrufe** und **SMS** auf dem Display,
  mit **Ton** und Lautstärkeregler, auch bei gesperrtem Handy – per Android-App; **FRITZ!App Fon**-Anrufe
  werden mit erkannt; iPhone-Modus (ohne App, ungetestet) – siehe [docs/ANRUFANZEIGE.md](docs/ANRUFANZEIGE.md)
- **Einrichtung per Handy/PC:** WLAN-Netze, Zeitzone, Wetter-Ort, Anzeige, Ton und vCard über eine Weboberfläche ändern
  (WLAN nur für einige Minuten an) – siehe [docs/EINRICHTUNG.md](docs/EINRICHTUNG.md)
- **Helligkeitsregler** im Hauptmenü
- Bildschirmfoto per USB ([tools/bildschirmfoto.py](tools/bildschirmfoto.py))

## Aufbau des Repositorys

| Ordner | Inhalt |
|---|---|
| `bahnhofsuhr_totp_waveshare/` | Arduino-Sketch für das Waveshare (alle `konfiguration*.h` liegen daneben) |
| `atoms3r_totp_scanner_funk/` | Arduino-Sketch für die Kamera AtomS3R-CAM (Konten per Funk übertragen) |
| `android_anrufanzeige/` | Android-App (Kotlin, vier Dateien zum Einfügen in ein neues Projekt) |
| `tools/` | Hilfsprogramm für Bildschirmfotos |
| `docs/` | Ausführliche Beschreibung von Kamera, Anrufanzeige und Einrichtung per Handy |

`funk_protokoll.h` liegt absichtlich in **beiden** Sketch-Ordnern und muss identisch sein (Arduino braucht die
Datei im jeweiligen Sketch-Ordner).

## Schnellstart (Waveshare)

1. **Arduino IDE** mit dem Board-Paket *esp32 (Espressif) 3.3.x* installieren.
2. Bibliotheken (Bibliotheksverwalter): **lvgl 9.3.0**, **GFX Library for Arduino**, **SensorLib**,
   **ArduinoJson 7**, **NimBLE-Arduino 2.x** (nur für die Anrufanzeige).
3. **lvgl einrichten:** `lv_conf.h` gehört in den `libraries`-Ordner (nicht in den Sketch-Ordner). Wichtig:
   `LV_COLOR_DEPTH 16`, `LV_USE_QRCODE 1`, `LV_USE_SLIDER 1` (Lautstärke- und Helligkeitsregler), `LV_USE_SNAPSHOT 1` (nur für Bildschirmfotos) und die
   Schriften `montserrat` 16/20/24/28/32/48 einschalten.
4. Den Ordner `bahnhofsuhr_totp_waveshare/` öffnen. Einstellungen:
   - **Board:** ESP32S3 Dev Module, **USB CDC On Boot:** Disabled, **PSRAM:** OPI PSRAM,
     **Flash Size:** 16MB, **Flash Mode:** QIO 80MHz,
     **Partition Scheme:** „Huge APP (3MB No OTA/1MB SPIFFS)“ (die IDE setzt die Auswahl manchmal zurück)
5. **`konfiguration.h`** anpassen: WLAN (bis zu drei Netze), Zeitzone, vCard, Zifferblatt, Sekundenzeiger, Start.
   **`konfiguration_wetter.h`:** eigener Ort. **`konfiguration_ble.h`:** Anrufanzeige, Ton, SMS.
   **`konfiguration_funk.h`:** eigener 16-Byte-Schlüssel (nur nötig für die Kamera).
   **`konfiguration_web.h`:** Einrichtung per Handy (Dauer, Hotspot-Name).
6. Hochladen. Beim ersten Start wird nach einer **PIN** gefragt.

Alle Einstellungen stehen in den `konfiguration*.h`-Dateien; der Hauptsketch muss nicht angefasst werden. Die wichtigsten
Werte lassen sich später auch am Gerät über die Weboberfläche ändern.

## Wichtige Hinweise

**Sicherheit**
- TOTP-Geheimnisse (secrets), WLAN-Zugangsdaten und der Funkschlüssel liegen **unverschlüsselt im Flash**.
  Die PIN schützt die Bedienung, **nicht** den Speicher. Die Flash-Verschlüsselung des ESP32 ist nicht eingerichtet.
  Wer das Gerät in die Hand bekommt und auslesen kann, kommt an die Geheimnisse.
- Den Funkschlüssel (`konfiguration_funk.h`) **selbst erzeugen**; solange er nur aus Nullen besteht, sendet die Kamera nichts.
- Die Android-App benötigt für die Fritz!Fon-Erkennung den **Benachrichtigungszugriff**. Sie wertet nur Apps mit „fritz“
  oder „avm“ im Namen aus, verwirft alles andere und speichert/protokolliert nichts.
  SMS-Texte werden nur weitergereicht, nicht gespeichert.
- Eigene, ausgefüllte `konfiguration*.h` **nie** veröffentlichen (WLAN-Passwort, Funkschlüssel, Adresse in der vCard).

**Akku**
- Viele LiPo-Akkus haben die **umgekehrte Polung** zum Anschluss des Boards. **Niemals** einfach einstecken:
  Polung am Stecker gegen die Beschriftung des Boards prüfen, gegebenenfalls einen Adapter (gekreuzte Leitungen) bauen.
  Die Akkus selbst nicht umlöten.

**Hardware-Zuordnung**
- Die Anschlüsse (Display, Touch, Audio-Chip ES8311, Verstärker) stammen aus dem Waveshare-Schaltplan und wurden
  am eigenen Gerät erprobt. Bei anderen Hardwareversionen können Abweichungen auftreten
  (siehe `AUDIO_PIN_*` in `konfiguration_ble.h`).

**Haftung**
- Keine Gewähr, siehe Lizenz. Nutzung auf eigene Verantwortung. Ein TOTP-Generator ersetzt kein
  Backup der Konten: Bewahre Wiederherstellungscodes getrennt auf.

## Android-App (Anrufanzeige)

Vier Dateien für ein neues Android-Studio-Projekt (Paketname `de.bahnhofsuhr.anruf`, Minimum API 33).
Schritt-für-Schritt: [android_anrufanzeige/ANLEITUNG.txt](android_anrufanzeige/ANLEITUNG.txt).
Hinweis: Die App ist nur für den eigenen Gebrauch gedacht (SMS-/Benachrichtigungszugriff); sie ist nicht für den
Play Store vorgesehen.

## Lizenz

[GNU General Public License, Version 3](LICENSE) (`GPL-3.0-only`). Das Zifferblatt orientiert sich an einer
GPL-lizenzierten Vorlage von ludi.de – Näheres und Drittkomponenten in [NOTICE.md](NOTICE.md).
