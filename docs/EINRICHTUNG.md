# Einrichtung per Handy (Weboberfläche)

Statt die `konfiguration*.h` im Quelltext zu ändern und neu hochzuladen, lassen sich die wichtigsten Einstellungen
am laufenden Gerät über eine Webseite ändern. Das WLAN ist dafür nur für einige Minuten eingeschaltet.

## Ablauf

1. Im Hauptmenü das **Zahnrad-Symbol** (unten, Mitte) antippen.
2. Weg wählen:
   - **Hotspot (empfohlen):** Das Gerät spannt ein eigenes WLAN `Bahnhofsuhr-Setup` auf. Das Passwort wird bei jedem Start
     neu zufällig erzeugt und zusammen mit einem **QR-Code zum Verbinden** auf dem Display angezeigt. Mit Handy oder PC
     verbinden und die angezeigte Adresse im Browser öffnen. Die IP-Adresse des Routers muss man nicht kennen.
   - **Im Heimnetz:** Das Gerät verbindet sich mit einem seiner gespeicherten WLAN-Netze. Auf dem Display stehen die
     Adresse und ein **6-stelliger Code**; der Code wird auf der Seite eingegeben (nach 5 Fehlversuchen wird die
     Einrichtung beendet).
3. Einstellungen ändern und **„Speichern und Beenden“** drücken. Das Gerät startet neu und schaltet das WLAN ab.
   Ohne Speichern beendet **„Abbrechen“** oder die Taste am Display die Einrichtung; spätestens nach der eingestellten
   Zeit (Standard 10 Minuten) wird das WLAN von selbst abgeschaltet.

## Was sich einstellen lässt

| Bereich | Einstellungen |
|---|---|
| WLAN und Zeit | bis zu **5 WLAN-Netze** (werden der Reihe nach probiert), Zeit beim Start per WLAN holen, Zeitzone |
| Wetter | Ortsname, Breiten-/Längengrad, Wetter beim Start holen, Aktualisierungsabstand, nur mit Netzteil, Display-Aus-Timeout |
| Anzeige und Ton | Startbild (Uhr/Wetter), Start mit Startbild oder Menü, Zifferblatt (quadratisch/rund, Ring), Ton bei Anruf/SMS, Lautstärke, SMS-Wiederholungen |
| QR-Code | vCard (`BEGIN:VCARD` … `END:VCARD`, höchstens 1000 Zeichen) |

**Nicht** über die Seite einstellbar (nur am Gerät): PIN, TOTP-Konten, Funkschlüssel.

## Hinweise zur Sicherheit

- Gespeicherte WLAN-Passwörter werden **nie wieder angezeigt**; ein leeres Passwortfeld lässt das alte Passwort unverändert.
- Im **Hotspot-Weg** ist die Verbindung per WPA2 verschlüsselt. Im **Heimnetz-Weg** läuft die Seite ohne Verschlüsselung
  (`http`) im eigenen WLAN. Wer das nicht möchte, nimmt den Hotspot.
- Die Einstellungen aus der Weboberfläche liegen wie alle anderen Daten **unverschlüsselt im Flash**.
- Was über die Seite gespeichert wurde, hat **Vorrang** vor den Werten in den `konfiguration*.h`-Dateien. Die Dateien
  liefern nur die Ausgangswerte.
- Ganz abschalten: `WEB_EINRICHTUNG_EINBAUEN 0` in `konfiguration_web.h`.

## Weitere Einstellungen in `konfiguration_web.h`

```cpp
#define WEB_EINRICHTUNG_EINBAUEN 1          // 0 = weglassen
#define WEB_EINRICHTUNG_MINUTEN 10          // so lange bleibt das WLAN offen
#define WEB_HOTSPOT_NAME "Bahnhofsuhr-Setup"
#define WEB_HOTSPOT_KANAL 6
```

## Bildschirmhelligkeit

Im Hauptmenü gibt es unten einen **Helligkeitsregler** (5 bis 100 %, bleibt gespeichert). Die Hintergrundbeleuchtung
wird per PWM an GPIO4 geregelt. Wirkt der Regler verkehrt herum, hilft `#define HELLIGKEIT_PWM_INVERTIERT 0` in
`konfiguration.h`; bei Flackern `#define HELLIGKEIT_PWM_HZ 5000`.
