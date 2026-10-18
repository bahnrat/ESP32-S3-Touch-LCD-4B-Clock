# Bluetooth-Anrufanzeige (Handy -> Waveshare)

Siehe auch `android_anrufanzeige/ANLEITUNG.txt` (App bauen und einrichten).

```text
Bei eingehendem Anruf zeigt das Waveshare Name und Rufnummer auf rotem Bildschirm.
Nur mit Netzteil/USB (einstellbar in konfiguration_ble.h), Antippen schliesst die Anzeige.
Die Uebertragung ist verschluesselt (Bluetooth LE Secure Connections mit 6-stelligem Code).
Es wird nur der Anruf-Eingang angezeigt, kein Freisprechen.
Ausserdem zeigt das Waveshare eingehende SMS auf blauem Bildschirm an (Absender bzw. Kontaktname
und Text, ein Code aus 5 bis 8 Ziffern gross) - auch bei gesperrtem Handy. Nur mit der Android-App;
der Text wird nicht gespeichert und nicht protokolliert. Es werden nur normale SMS weitergegeben,
keine RCS-/Chat-Nachrichten. Dauer der Anzeige: BLE_SMS_SEKUNDEN in konfiguration_ble.h.

Ton ueber den eingebauten Lautsprecher (Anschluss "Speaker" am Board, 8 Ohm):
  - Anruf: klingelt, bis der Anruf endet oder die Anzeige angetippt wird.
  - SMS: kurzer Ton, mehrfach hintereinander (BLE_TON_SMS_ANZAHL, Standard 3).
  - Im Menue "Anrufanzeige" -> "Ton-Einstellungen": Ton bei Anruf und bei SMS einzeln ein/aus,
    Lautstaerkeregler, Testknoepfe. Die Einstellungen bleiben gespeichert.
  - Wer den Ton nicht braucht: BLE_TON_EINBAUEN in konfiguration_ble.h auf 0 stellen.
Zwei Arten von Handys (im Menue "Anrufanzeige" umschaltbar, Standard in konfiguration_ble.h):
  - Android: mit der mitgelieferten App (siehe unten)
  - iPhone:  ohne App, siehe Abschnitt "iPhone" ganz unten

Programmgroesse: mit Anrufanzeige rund 1,9 MB. Deshalb muss das Partition Scheme
"Huge APP (3MB No OTA/1MB SPIFFS)" gewaehlt sein, sonst meldet die IDE "Sketch zu gross".
(Die Arduino IDE setzt die Auswahl manchmal auf den Standard zurueck - bitte kontrollieren.)

Einrichten (Android)
  1. Waveshare hochladen, per USB/Netzteil betreiben.
  2. Am Waveshare: Menue -> Anrufanzeige. Schalter an, "Kopplung starten" antippen
     (2 Minuten offen).
  3. Am Android-Handy die zugehoerige App installieren (Ordner android_anrufanzeige,
     in Android Studio bauen, siehe ANLEITUNG.txt darin). In der App: 1. Berechtigungen erlauben, 2. Waveshare
     auswaehlen, 3. Dienst starten. Beim ersten Verbinden fragt das Handy nach der Kopplung;
     den 6-stelligen Code vom Waveshare-Bildschirm eingeben.
  4. Samsung: Akku-Einstellung der App auf "Unbegrenzt", sonst beendet Android den Dienst.
  5. SMS: Bei einer per APK-Datei installierten App sperrt Android die SMS-Berechtigung zunaechst
     ("Eingeschraenkte Einstellung"): Einstellungen -> Apps -> Anrufanzeige -> Menue (drei Punkte)
     -> "Eingeschraenkte Einstellungen zulassen", danach die Berechtigung erneut erteilen.
  - Anrufe der FRITZ!App Fon (Internettelefonie ueber die Fritz!Box): Die Android-App liest dafuer
    die Anruf-Benachrichtigung dieser App mit (Benachrichtigungszugriff, nur fuer Apps mit "fritz" oder
    "avm" im Namen) und gibt den Anruf ans Waveshare weiter. Einrichtung und Hinweise stehen in der
    ANLEITUNG.txt im Ordner android_anrufanzeige. Nur Android.
  - Die Anzeige der SIM-Karte funktioniert je nach Handy nicht (auf dem Galaxy S24 Ultra nicht).
  - "Kopplungen loeschen" im Waveshare-Menue entfernt alle gekoppelten Handys.
```

## iPhone (ohne App) - noch nicht an einem echten iPhone getestet

```text
iOS erlaubt Apps nicht, Anrufe mitzulesen. Dafuer gibt es einen eingebauten Weg: Das iPhone reicht
seine Benachrichtigungen an gekoppeltes Bluetooth-Zubehoer weiter (Apple Notification Center Service,
ANCS, so arbeiten auch Smartwatches). Das Waveshare liest darueber nur die Meldung "Eingehender Anruf".
Es ist keine App noetig. Name und Nummer zeigt das Waveshare so an, wie das iPhone sie meldet.
Die SIM-Karte (bei Dual-SIM) meldet iOS nicht.

Einrichten
  1. Am Waveshare: Menue -> Anrufanzeige. Schalter "Bluetooth-Anzeige" an, Schalter
     "iPhone (ohne App)" an.
  2. "Kopplung starten" antippen (2 Minuten offen).
  3. Am iPhone: Einstellungen -> Bluetooth, unter "Weitere Geraete" "Bahnhofsuhr" antippen.
     Den 6-stelligen Code, der am Waveshare erscheint, am iPhone eingeben.
  4. Am iPhone: Einstellungen -> Bluetooth, beim Geraet "Bahnhofsuhr" auf das (i) tippen und
     "Systembenachrichtigungen teilen" einschalten (falls es nicht schon gefragt wurde).
  5. Am Waveshare steht dann "iPhone verbunden - Anrufe werden gemeldet".
Das iPhone verbindet sich danach von selbst wieder, sobald das Waveshare in Reichweite ist.

Wenn es nicht klappt
  - Erscheint "Bahnhofsuhr" nicht in der iPhone-Liste: Waveshare nicht im Akkubetrieb? (Bluetooth laeuft
    nur mit Netzteil.) Danach in beiden Richtungen Bluetooth aus und wieder an.
  - Steht am Waveshare "keine Meldungen": "Systembenachrichtigungen teilen" einschalten, dann
    am iPhone das Geraet einmal "ignorieren" und neu koppeln ("Kopplungen loeschen" am Waveshare).
  - Im seriellen Monitor (115200) erscheinen Zeilen mit "ANCS:". Die Zeile
    ANCS: Titel=... Untertitel=... Nachricht=...
    zeigt, was das iPhone bei einem Anruf liefert. Damit laesst sich die Anzeige anpassen.
```
