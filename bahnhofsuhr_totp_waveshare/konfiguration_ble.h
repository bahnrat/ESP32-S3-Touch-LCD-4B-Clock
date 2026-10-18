// ============================================================
//  Einstellungen: Bluetooth-Anrufanzeige (BLE)
//  Das Handy meldet einen eingehenden Anruf, das Waveshare zeigt ihn an.
//  Verschluesselt: Kopplung mit 6-stelligem Code (der Code erscheint auf dem Waveshare),
//  danach laeuft alles verschluesselt. Nur gekoppelte Handys duerfen schreiben.
//
//  Benoetigt die Bibliothek "NimBLE-Arduino" von h2zero (Version 2.x) im Bibliotheksverwalter.
// ============================================================

// 1 = Anrufanzeige in das Programm einbauen, 0 = weglassen (dann wird NimBLE nicht gebraucht
// und das Programm ist rund 200 KB kleiner). Bei "Sketch zu gross" erst mal auf 0 stellen.
#define BLE_ANRUFANZEIGE_EINBAUEN 1

// Anrufanzeige nach dem ersten Start eingeschaltet? (Danach gilt die Einstellung im Menue.)
const bool BLE_ANRUFANZEIGE_STANDARD = true;

// Welches Handy? (Nach dem ersten Start gilt die Einstellung im Menue "Anrufanzeige".)
//   false = Android-Handy mit der mitgelieferten App (Ordner android_anrufanzeige)
//   true  = iPhone, ohne App: das Waveshare liest die Benachrichtigungen des iPhones (ANCS).
//           Am iPhone muss dazu "Systembenachrichtigungen teilen" fuer das Geraet eingeschaltet sein.
const bool BLE_IPHONE_STANDARD = false;

// true = Bluetooth nur bei angeschlossenem Netzteil/USB (spart Akku)
const bool BLE_NUR_MIT_NETZTEIL = true;

// Name, unter dem das Waveshare am Handy erscheint
#define BLE_GERAETENAME "Bahnhofsuhr"

// Wie lange ein Anruf maximal angezeigt wird, falls das Handy kein "Ende" meldet (Sekunden)
const unsigned long BLE_ANRUF_MAX_SEKUNDEN = 45;

// Wie lange die Kopplung nach dem Antippen von "Kopplung starten" offen bleibt (Sekunden)
const unsigned long BLE_KOPPLUNG_SEKUNDEN = 120;

// false = Rufnummer nicht anzeigen, wenn ein Name bekannt ist (Datenschutz an der Wand)
const bool BLE_NUMMER_ANZEIGEN = true;

// Eingehende SMS auf dem Display anzeigen (1 = ja, 0 = ignorieren; nur Android-App, nicht iPhone). Der Text wird nicht gespeichert.
#define BLE_SMS_ANZEIGEN 1

// Wie lange eine SMS angezeigt wird (Sekunden). Antippen schliesst sofort.
#define BLE_SMS_SEKUNDEN 60

// ---- Ton ueber den eingebauten Lautsprecher (Anschluss "Speaker" am Board) ----
// 1 = Ton-Funktion einbauen, 0 = weglassen (z.B. falls die Arduino IDE bei "i2s" einen Fehler meldet)
#define BLE_TON_EINBAUEN 1
// Ton nach dem ersten Start eingeschaltet? (Danach gelten die Schalter im Menue "Anrufanzeige".)
#define BLE_TON_STANDARD_ANRUF 1
#define BLE_TON_STANDARD_SMS 1
// Lautstaerke 0 bis 100. Lieber leise anfangen.
#define BLE_TON_LAUTSTAERKE 40
// 1 = Anruf klingelt, bis er endet oder angenommen wird; 0 = nur einmal
#define BLE_TON_ANRUF_WIEDERHOLEN 1
// Wie oft der SMS-Ton hintereinander gespielt wird (1 bis 20)
#define BLE_TON_SMS_ANZAHL 3
// Anschluss des Audio-Chips ES8311 laut Waveshare-Schaltplan. Nur aendern, wenn der Ton ausbleibt
// und die Waveshare-Demo "07_ES8311" andere Nummern verwendet.
#define AUDIO_PIN_MCLK 5
#define AUDIO_PIN_BCLK 16
#define AUDIO_PIN_WS 7
#define AUDIO_PIN_DOUT 6
#define AUDIO_PA_EXPANDER_PIN 3

// Beschriftung der SIM-Karten (das Handy meldet 1 oder 2). Nur ASCII, keine Umlaute.
const char *const BLE_SIM_BESCHRIFTUNG[2] = {"SIM 1", "SIM 2"};

// Kennungen (UUIDs) des Dienstes - muessen mit der Handy-App uebereinstimmen.
// Nur aendern, wenn du weisst warum.
#define BLE_SERVICE_UUID "b7a0c7e0-1c4a-4f6e-9a3e-5d1c0a7e0001"
#define BLE_ANRUF_UUID "b7a0c7e0-1c4a-4f6e-9a3e-5d1c0a7e0002"

// Nachrichtenformat (Text, UTF-8), vom Handy in die Anruf-Eigenschaft geschrieben:
//   RING|<SIM 1 oder 2, 0 = unbekannt>|<Rufnummer>|<Name aus den Kontakten, kann leer sein>
//   IDLE            (Anruf beendet oder angenommen)
//   SMS|<Absender oder Kontaktname>|<Text>      (eingehende SMS, Text bis rund 170 Zeichen)
