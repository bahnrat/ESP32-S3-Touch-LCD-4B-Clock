// ============================================================
//  Einstellungen: Einrichtung ueber Weboberflaeche
//  Im Menue "Einrichtung per Handy" schaltet das Waveshare fuer einige Minuten das WLAN ein.
//  Mit Handy oder PC lassen sich dann WLAN-Netze, Zeitzone, Wetter-Ort, Anzeige-Optionen,
//  Toene und der QR-Code (vCard) aendern. Danach schaltet sich das WLAN wieder ab.
//  Nicht aenderbar ueber die Seite: PIN, TOTP-Konten, Funkschluessel.
// ============================================================

// 1 = Einrichtung per Weboberflaeche einbauen, 0 = weglassen
#define WEB_EINRICHTUNG_EINBAUEN 1

// So lange bleibt das WLAN der Einrichtung offen (Minuten). Danach schaltet es sich selbst ab.
#define WEB_EINRICHTUNG_MINUTEN 10

// Name des eigenen WLANs (Hotspot), das das Waveshare dafuer aufspannt.
// Das Passwort wird bei jedem Start neu gewuerfelt und steht auf dem Display.
#define WEB_HOTSPOT_NAME "Bahnhofsuhr-Setup"

// WLAN-Kanal des Hotspots (1 bis 11)
#define WEB_HOTSPOT_KANAL 6
