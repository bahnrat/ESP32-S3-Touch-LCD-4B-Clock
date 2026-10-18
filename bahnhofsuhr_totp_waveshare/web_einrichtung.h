// SPDX-License-Identifier: GPL-3.0-only
// ============================================================
//  web_einrichtung.h  -  Einrichtung ueber eine Weboberflaeche
//  Wird vom Hauptprogramm (.ino) eingebunden und liegt im selben Ordner.
//
//  Ablauf: Menue "Einrichtung per Handy" -> Hotspot (eigenes WLAN, WPA2 mit Zufallspasswort, QR-Code
//  zum Verbinden) oder Heimnetz (bekanntes WLAN, Adresse und Code stehen auf dem Display).
//  Die Seite aendert nur Einstellungen: WLAN-Netze, Zeitzone, Wetter-Ort, Anzeige, Ton, vCard.
//  PIN, TOTP-Konten und Funkschluessel gibt es dort NICHT; gespeicherte WLAN-Passwoerter werden
//  nie wieder angezeigt. Nach "Speichern und Beenden" startet das Geraet neu (Einstellungen gelten
//  sofort, die Uhrzeit bleibt in der Echtzeituhr erhalten).
// ============================================================

#if WEB_EINRICHTUNG_EINBAUEN
#include <WebServer.h>

static WebServer *webServer = NULL;
static bool webAktiv = false;
static bool webHotspot = true;
static unsigned long webEnde = 0;
static volatile int webStartWunsch = 0;        // 1 = Hotspot, 2 = Heimnetz (wird in loop() abgearbeitet)
static volatile bool webBeendenWunsch = false;
static bool webNeustartWunsch = false;
static unsigned long webNeustartZeit = 0;
static char webHotspotPasswort[10] = "";
static char webCode[8] = "";
static char webToken[20] = "";
static bool webTokenNoetig = false;
static int webFehlversuche = 0;
static String webNetzListe;                    // <option>-Zeilen der sichtbaren WLAN-Netze
static String webAdresse;
static lv_obj_t *web_start_screen = NULL;
static lv_obj_t *web_start_status = NULL;
static lv_obj_t *web_screen = NULL;
static lv_obj_t *web_zeit_label = NULL;

// ---------------- Hilfsfunktionen ----------------
static String webHtml(const String &s) {
    String r;
    r.reserve(s.length() + 16);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '&') r += "&amp;";
        else if (c == '<') r += "&lt;";
        else if (c == '>') r += "&gt;";
        else if (c == '"') r += "&quot;";
        else r += c;
    }
    return r;
}

// Nur ASCII fuer Texte, die auf dem Display erscheinen (Umlaute werden umgeschrieben)
static String webAscii(const String &q, size_t maxLaenge) {
    String r;
    for (unsigned i = 0; i < q.length() && r.length() < maxLaenge; i++) {
        uint8_t c = (uint8_t)q[i];
        if (c == 0xC3 && i + 1 < q.length()) {
            const char *e = "?";
            switch ((uint8_t)q[i + 1]) {
                case 0xA4: e = "ae"; break;
                case 0xB6: e = "oe"; break;
                case 0xBC: e = "ue"; break;
                case 0x84: e = "Ae"; break;
                case 0x96: e = "Oe"; break;
                case 0x9C: e = "Ue"; break;
                case 0x9F: e = "ss"; break;
            }
            r += e;
            i++;
        } else if (c >= 32 && c < 127) {
            r += (char)c;
        } else if (c >= 0x80) {
            while (i + 1 < q.length() && (((uint8_t)q[i + 1]) & 0xC0) == 0x80) i++;
            r += '?';
        }
    }
    r.trim();
    return r;
}

static void webZufallPasswort(char *ziel, size_t n) {
    const char *z = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // ohne verwechselbare Zeichen
    for (size_t i = 0; i + 1 < n; i++) ziel[i] = z[esp_random() % 32];
    ziel[n - 1] = 0;
}

static bool webErlaubt() {
    if (!webTokenNoetig) return true;
    if (!webServer->hasHeader("Cookie")) return false;
    String c = webServer->header("Cookie");
    return c.indexOf(String("t=") + webToken) >= 0;
}

static void webKopf(const char *titel) {
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer->send(200, "text/html; charset=utf-8", "");
    webServer->sendContent(
        "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Bahnhofsuhr</title><style>"
        ":root{color-scheme:light dark}"
        "body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:12px}"
        "h1{font-size:1.35em}fieldset{margin:14px 0;border-radius:8px}legend{font-weight:600}"
        "label{display:block;margin:10px 0 3px}"
        "input[type=text],input[type=password],input[type=number],select,textarea{width:100%;box-sizing:border-box;padding:9px;font-size:1em}"
        ".chk{display:flex;gap:10px;align-items:center;margin:10px 0}.chk input{width:auto;transform:scale(1.3)}"
        "button{padding:12px 20px;font-size:1.05em;margin:8px 8px 8px 0}"
        ".hint{font-size:.85em;opacity:.7;margin:3px 0}"
        ".err{background:#fdd;color:#900;padding:10px;border-radius:6px;margin:10px 0}"
        "</style></head><body><h1>");
    webServer->sendContent(titel);
    webServer->sendContent("</h1>");
}

static void webFuss() {
    webServer->sendContent("</body></html>");
    webServer->sendContent("");
}

static void webEinfachSeite(const char *titel, const String &text) {
    webKopf(titel);
    webServer->sendContent(text);
    webFuss();
}

static String webOption(const char *wert, const char *text, bool gewaehlt) {
    String s = "<option value=\"";
    s += wert;
    s += "\"";
    if (gewaehlt) s += " selected";
    s += ">";
    s += text;
    s += "</option>";
    return s;
}

static String webHaken(const char *name, const char *text, bool an) {
    String s = "<div class=\"chk\"><input type=\"checkbox\" id=\"";
    s += name;
    s += "\" name=\"";
    s += name;
    s += "\" value=\"1\"";
    if (an) s += " checked";
    s += "><label for=\"";
    s += name;
    s += "\" style=\"margin:0\">";
    s += text;
    s += "</label></div>";
    return s;
}

struct WebZone { const char *wert; const char *text; };
static const WebZone WEB_ZONEN[] = {
    {"CET-1CEST,M3.5.0,M10.5.0/3", "Mitteleuropa (Deutschland, &Ouml;sterreich, Schweiz ...)"},
    {"GMT0BST,M3.5.0/1,M10.5.0", "Gro&szlig;britannien, Irland (London)"},
    {"WET0WEST,M3.5.0/1,M10.5.0", "Westeuropa (Lissabon)"},
    {"EET-2EEST,M3.5.0/3,M10.5.0/4", "Osteuropa (Helsinki, Athen)"},
    {"UTC0", "UTC (keine Umstellung)"},
};
static const int WEB_ZONEN_ANZAHL = 5;

// ---------------- Seiten ----------------
static void webSeiteLogin(bool fehler) {
    String t = "<p>Bitte den Code eingeben, der am Display des Waveshare steht.</p>";
    if (fehler) t += "<div class=\"err\">Falscher Code.</div>";
    t += "<form method=\"post\" action=\"/login\"><input type=\"text\" name=\"code\" inputmode=\"numeric\" "
         "autocomplete=\"off\" maxlength=\"6\" autofocus><button type=\"submit\">Weiter</button></form>";
    webEinfachSeite("Bahnhofsuhr: Einrichtung", t);
}

static void webSeiteFormular() {
    webKopf("Bahnhofsuhr: Einrichtung");
    String s;
    s.reserve(1200);

    webServer->sendContent("<form method=\"post\" action=\"/save\" accept-charset=\"utf-8\">");

    // ---- 1. WLAN und Zeit ----
    webServer->sendContent("<fieldset><legend>1. WLAN und Zeit</legend>"
                           "<div class=\"hint\">Bis zu f&uuml;nf Netze, in dieser Reihenfolge werden sie probiert. "
                           "Passwort leer lassen = unver&auml;ndert (bei einem neuen Netz: offenes Netz). "
                           "Netzname l&ouml;schen = Eintrag entfernen. Gespeicherte Passw&ouml;rter werden nie angezeigt.</div>");
    webServer->sendContent("<datalist id=\"netze\">" + webNetzListe + "</datalist>");
    for (int i = 0; i < WE_NETZE; i++) {
        s = "<label>Netz ";
        s += String(i + 1);
        s += "</label><input type=\"text\" name=\"s";
        s += String(i);
        s += "\" list=\"netze\" maxlength=\"32\" autocomplete=\"off\" value=\"";
        s += webHtml(String(we.ssid[i]));
        s += "\"><input type=\"password\" name=\"w";
        s += String(i);
        s += "\" maxlength=\"64\" autocomplete=\"new-password\" placeholder=\"Passwort (leer = unver&auml;ndert)\" style=\"margin-top:4px\">";
        webServer->sendContent(s);
    }
    webServer->sendContent(webHaken("zs", "Beim Start die Zeit per WLAN holen", we.zeitBeimStart));
    bool bekannt = false;
    s = "<label>Zeitzone</label><select name=\"tzp\">";
    for (int i = 0; i < WEB_ZONEN_ANZAHL; i++) {
        bool g = strcmp(we.tz, WEB_ZONEN[i].wert) == 0;
        if (g) bekannt = true;
        s += webOption(WEB_ZONEN[i].wert, WEB_ZONEN[i].text, g);
    }
    s += webOption("eigene", "Eigene Angabe (unten)", !bekannt);
    s += "</select><label>Eigene Zeitzone (nur bei &bdquo;Eigene Angabe&ldquo;)</label>"
         "<input type=\"text\" name=\"tz\" maxlength=\"63\" autocomplete=\"off\" value=\"";
    s += webHtml(String(we.tz));
    s += "\"><div class=\"hint\">POSIX-Schreibweise, z. B. CET-1CEST,M3.5.0,M10.5.0/3</div></fieldset>";
    webServer->sendContent(s);

    // ---- 2. Wetter ----
    s = "<fieldset><legend>2. Wetter</legend><label>Ortsname (nur Anzeige, Umlaute werden umgeschrieben)</label>"
        "<input type=\"text\" name=\"ort\" maxlength=\"31\" value=\"";
    s += webHtml(String(we.ort));
    s += "\"><label>Breitengrad (z. B. 52.5200)</label><input type=\"text\" name=\"br\" inputmode=\"decimal\" value=\"";
    s += String(we.breite, 4);
    s += "\"><label>L&auml;ngengrad (z. B. 13.4050)</label><input type=\"text\" name=\"la\" inputmode=\"decimal\" value=\"";
    s += String(we.laenge, 4);
    s += "\"><div class=\"hint\">Dezimalgrad mit Punkt. Die Koordinaten findest du z. B. in der Karte "
         "(Rechtsklick auf den Ort).</div>";
    webServer->sendContent(s);
    webServer->sendContent(webHaken("wbs", "Beim Start auch das Wetter holen", we.wetterBeimStart));
    s = "<label>Wetter automatisch aktualisieren alle ... Minuten (0 = nie)</label><input type=\"number\" name=\"wm\" min=\"0\" max=\"1440\" value=\"";
    s += String(we.wetterMinuten);
    s += "\">";
    webServer->sendContent(s);
    webServer->sendContent(webHaken("wn", "Automatisch nur mit Netzteil/USB (spart Akku)", we.wetterNurNetz));
    webServer->sendContent(webHaken("wa", "Beim Wetter-Startbild das Display nach 2 Minuten ausschalten", we.wetterAusTimeout));
    webServer->sendContent("</fieldset>");

    // ---- 3. Anzeige und Ton ----
    s = "<fieldset><legend>3. Anzeige und Ton</legend><label>Startbild</label><select name=\"st\">";
    s += webOption("0", "Uhr", startModus == 0);
    s += webOption("1", "Wetter", startModus == 1);
    s += "</select>";
    webServer->sendContent(s);
    webServer->sendContent(webHaken("nu", "Nach dem Start sofort das Startbild zeigen (sonst das Men&uuml;)", we.nachStartUhr));
    s = "<label>Zifferblatt</label><select name=\"qd\">";
    s += webOption("1", "Quadratisch (ganzes Display wei&szlig;)", we.quadratisch);
    s += webOption("0", "Rund auf schwarzem Grund", !we.quadratisch);
    s += "</select>";
    webServer->sendContent(s);
    webServer->sendContent(webHaken("qr", "Beim quadratischen Zifferblatt d&uuml;nnen schwarzen Ring zeichnen", we.quadratRing));
#if BLE_ANRUFANZEIGE_EINBAUEN
    webServer->sendContent(webHaken("ta", "Ton bei Anruf", bleTonAnruf));
    webServer->sendContent(webHaken("ts", "Ton bei SMS", bleTonSms));
    s = "<label>Lautst&auml;rke (0 bis 100)</label><input type=\"number\" name=\"tv\" min=\"0\" max=\"100\" value=\"";
    s += String((int)bleTonLautstaerke);
    s += "\"><label>SMS-Ton wiederholen (1 bis 20 Mal)</label><input type=\"number\" name=\"sa\" min=\"1\" max=\"20\" value=\"";
    s += String(we.smsAnzahl);
    s += "\">";
    webServer->sendContent(s);
#endif
    webServer->sendContent("</fieldset>");

    // ---- 4. vCard ----
    s = "<fieldset><legend>4. QR-Code (Visitenkarte, vCard)</legend>"
        "<div class=\"hint\">Muss mit BEGIN:VCARD beginnen und mit END:VCARD enden, h&ouml;chstens 1000 Zeichen. "
        "Leer lassen und speichern = Vorgabe aus konfiguration.h.</div>"
        "<textarea name=\"vc\" rows=\"10\" maxlength=\"1000\" spellcheck=\"false\">";
    s += webHtml(String(we.vcard));
    s += "</textarea></fieldset>";
    webServer->sendContent(s);

    webServer->sendContent("<button type=\"submit\">Speichern und Beenden</button></form>"
                           "<form method=\"post\" action=\"/ende\"><button type=\"submit\">Abbrechen (nichts speichern)</button></form>");
    webFuss();
}

// ---------------- Speichern ----------------
static float webZahl(const String &roh, bool &ok) {
    String t = roh;
    t.trim();
    t.replace(',', '.');
    if (t.length() == 0) { ok = false; return 0; }
    char *ende = NULL;
    double v = strtod(t.c_str(), &ende);
    ok = (ende != NULL && *ende == 0);
    return (float)v;
}

// Liefert "" bei Erfolg, sonst eine Fehlerliste (HTML)
static String webSpeichern() {
    String fehler;
    char ssid[WE_NETZE][33], pass[WE_NETZE][65];
    memcpy(ssid, we.ssid, sizeof(ssid));
    memcpy(pass, we.pass, sizeof(pass));
    for (int i = 0; i < WE_NETZE; i++) {
        char sk[3] = {'s', (char)('0' + i), 0}, wk[3] = {'w', (char)('0' + i), 0};
        String s = webServer->arg(sk);
        String w = webServer->arg(wk);
        s.trim();
        if (s.length() > 32) { fehler += "<li>Netz " + String(i + 1) + ": Name l&auml;nger als 32 Zeichen.</li>"; continue; }
        if (w.length() > 63) { fehler += "<li>Netz " + String(i + 1) + ": Passwort l&auml;nger als 63 Zeichen.</li>"; continue; }
        if (s.length() == 0) {
            ssid[i][0] = 0;
            pass[i][0] = 0;
        } else {
            bool neuesNetz = strcmp(ssid[i], s.c_str()) != 0;
            strlcpy(ssid[i], s.c_str(), sizeof(ssid[i]));
            if (w.length() > 0) strlcpy(pass[i], w.c_str(), sizeof(pass[i]));
            else if (neuesNetz) pass[i][0] = 0;
        }
    }

    String tz = webServer->arg("tzp");
    if (tz == "eigene" || tz.length() == 0) tz = webServer->arg("tz");
    tz.trim();
    bool tzOk = tz.length() > 0 && tz.length() < 64;
    for (unsigned i = 0; tzOk && i < tz.length(); i++) {
        char c = tz[i];
        if (!(isalnum((unsigned char)c) || c == ',' || c == '.' || c == ':' || c == '+' || c == '-' || c == '/' || c == '<' || c == '>')) tzOk = false;
    }
    if (!tzOk) fehler += "<li>Zeitzone: nur Buchstaben, Ziffern und , . : + - / &lt; &gt; erlaubt (h&ouml;chstens 63 Zeichen).</li>";

    String ort = webAscii(webServer->arg("ort"), 31);
    if (ort.length() == 0) ort = "Ort";
    bool bOk = false, lOk = false;
    float breite = webZahl(webServer->arg("br"), bOk);
    float laenge = webZahl(webServer->arg("la"), lOk);
    if (!bOk || breite < -90.0f || breite > 90.0f) fehler += "<li>Breitengrad: Zahl von -90 bis 90 mit Punkt.</li>";
    if (!lOk || laenge < -180.0f || laenge > 180.0f) fehler += "<li>L&auml;ngengrad: Zahl von -180 bis 180 mit Punkt.</li>";

    int wm = webServer->arg("wm").toInt();
    if (wm < 0 || wm > 1440) fehler += "<li>Wetter-Abstand: 0 bis 1440 Minuten.</li>";
    int st = webServer->arg("st").toInt();
    if (st != 0 && st != 1) st = 0;
    int tv = 40;
    int sa = webServer->hasArg("sa") ? webServer->arg("sa").toInt() : we.smsAnzahl;
#if BLE_ANRUFANZEIGE_EINBAUEN
    tv = webServer->hasArg("tv") ? webServer->arg("tv").toInt() : (int)bleTonLautstaerke;
    if (tv < 0 || tv > 100) fehler += "<li>Lautst&auml;rke: 0 bis 100.</li>";
#endif
    if (sa < 1 || sa > 20) fehler += "<li>SMS-Ton: 1 bis 20 Wiederholungen.</li>";

    String vc = webServer->arg("vc");
    vc.replace("\r\n", "\n");
    vc.replace("\r", "\n");
    vc.trim();
    bool vcLeer = (vc.length() == 0);
    if (!vcLeer) {
        String gross = vc;
        gross.toUpperCase();
        if (!gross.startsWith("BEGIN:VCARD") || !gross.endsWith("END:VCARD") || vc.length() > WE_VCARD_MAX - 1) {
            fehler += "<li>vCard: muss mit BEGIN:VCARD beginnen, mit END:VCARD enden und h&ouml;chstens 1000 Zeichen haben.</li>";
        }
    }

    if (fehler.length() > 0) return fehler;

    // ---- alles in Ordnung: speichern ----
    Preferences p;
    p.begin("web", false);
    for (int i = 0; i < WE_NETZE; i++) {
        char sk[3] = {'s', (char)('0' + i), 0}, wk[3] = {'w', (char)('0' + i), 0};
        p.putString(sk, ssid[i]);
        p.putString(wk, pass[i]);
    }
    p.putBool("zs", webServer->hasArg("zs"));
    p.putString("tz", tz);
    p.putString("ort", ort);
    p.putDouble("br", breite);
    p.putDouble("la", laenge);
    p.putBool("wbs", webServer->hasArg("wbs"));
    p.putInt("wm", wm);
    p.putBool("wn", webServer->hasArg("wn"));
    p.putBool("wa", webServer->hasArg("wa"));
    p.putBool("nu", webServer->hasArg("nu"));
    p.putBool("qd", webServer->arg("qd") != "0");
    p.putBool("qr", webServer->hasArg("qr"));
    p.putInt("sa", sa);
    if (vcLeer) p.remove("vc");
    else p.putString("vc", vc);
    p.end();

    Preferences e;      // dieselben Schluessel wie das Menue (Startbild, Ton)
    e.begin("einst", false);
    e.putUChar("start", (uint8_t)st);
#if BLE_ANRUFANZEIGE_EINBAUEN
    e.putBool("tonA", webServer->hasArg("ta"));
    e.putBool("tonS", webServer->hasArg("ts"));
    e.putInt("tonV", tv);
#endif
    e.end();
    return "";
}

// ---------------- Anfragen ----------------
static void webAnfrageWurzel() {
    if (!webErlaubt()) { webSeiteLogin(false); return; }
    webSeiteFormular();
}

static void webAnfrageLogin() {
    if (!webTokenNoetig) {
        webServer->sendHeader("Location", "/");
        webServer->send(303, "text/plain", "");
        return;
    }
    if (webServer->arg("code") == String(webCode)) {
        webFehlversuche = 0;
        webServer->sendHeader("Set-Cookie", String("t=") + webToken + "; Path=/; SameSite=Strict");
        webServer->sendHeader("Location", "/");
        webServer->send(303, "text/plain", "");
        return;
    }
    webFehlversuche++;
    if (webFehlversuche >= 5) {     // zu viele Versuche: Einrichtung beenden
        webEinfachSeite("Bahnhofsuhr: Einrichtung", "<div class=\"err\">Zu viele falsche Eingaben. Die Einrichtung wurde beendet.</div>");
        webBeendenWunsch = true;
        return;
    }
    webSeiteLogin(true);
}

static void webAnfrageSpeichern() {
    if (!webErlaubt()) { webSeiteLogin(false); return; }
    String fehler = webSpeichern();
    if (fehler.length() > 0) {
        webEinfachSeite("Bitte korrigieren", "<div class=\"err\"><ul>" + fehler + "</ul></div>"
                        "<p><a href=\"javascript:history.back()\">Zur&uuml;ck zum Formular</a></p>");
        return;
    }
    String liste = "<p>Gespeicherte WLAN-Netze:</p><ol>";
    Preferences chk;
    chk.begin("web", true);
    for (int i = 0; i < WE_NETZE; i++) {
        char sk[3] = {'s', (char)('0' + i), 0}, wk[3] = {'w', (char)('0' + i), 0};
        String n = chk.getString(sk, "");
        if (n.length() == 0) continue;
        liste += "<li>" + webHtml(n) + (chk.getString(wk, "").length() > 0 ? " (mit Passwort)" : " (ohne Passwort)") + "</li>";
        Serial.printf("Gespeichert: Netz %d = '%s'\n", i + 1, n.c_str());
    }
    chk.end();
    liste += "</ol>";
    webEinfachSeite("Gespeichert", liste + "<p>Die Einstellungen sind gespeichert. Das Waveshare startet jetzt neu und schaltet das WLAN ab. "
                                   "Du kannst diese Seite schlie&szlig;en.</p>");
    webNeustartWunsch = true;
    webNeustartZeit = millis();
}

static void webAnfrageEnde() {
    if (!webErlaubt()) { webSeiteLogin(false); return; }
    webEinfachSeite("Beendet", "<p>Nichts wurde ge&auml;ndert. Das WLAN am Waveshare wird abgeschaltet.</p>");
    webBeendenWunsch = true;
}

static void webAnfrageUnbekannt() {
    webServer->sendHeader("Location", "/");
    webServer->send(303, "text/plain", "");
}

// ---------------- Start / Ende ----------------
static void webNetzListeAufbauen() {
    webNetzListe = "";
    int n = WiFi.scanNetworks();
    String schon = "|";
    for (int i = 0; i < n && i < 25; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0 || s.length() > 32 || schon.indexOf("|" + s + "|") >= 0) continue;
        schon += s + "|";
        webNetzListe += "<option value=\"" + webHtml(s) + "\">";
    }
    WiFi.scanDelete();
}

static void webServerStarten() {
    if (webServer == NULL) {
        webServer = new WebServer(80);
        webServer->on("/", HTTP_GET, webAnfrageWurzel);
        webServer->on("/login", HTTP_POST, webAnfrageLogin);
        webServer->on("/save", HTTP_POST, webAnfrageSpeichern);
        webServer->on("/ende", HTTP_POST, webAnfrageEnde);
        webServer->onNotFound(webAnfrageUnbekannt);
        static const char *kopfzeilen[] = {"Cookie"};
        webServer->collectHeaders(kopfzeilen, 1);
    }
    webServer->begin();
}

static void webScreenErstellen() {
    if (web_screen != NULL) { lv_obj_delete(web_screen); web_screen = NULL; }
    web_screen = lv_obj_create(NULL);

    lv_obj_t *titel = lv_label_create(web_screen);
    lv_label_set_text(titel, "Einrichtung per Handy");
    lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *info = lv_label_create(web_screen);
    lv_obj_set_width(info, 450);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);

    char t[200];
    if (webHotspot) {
        char qrText[100];
        snprintf(qrText, sizeof(qrText), "WIFI:T:WPA;S:%s;P:%s;;", WEB_HOTSPOT_NAME, webHotspotPasswort);
        lv_obj_t *qr = lv_qrcode_create(web_screen);
        lv_qrcode_set_size(qr, 170);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 52);
        lv_qrcode_update(qr, qrText, strlen(qrText));
        lv_obj_set_style_border_color(qr, lv_color_white(), 0);
        lv_obj_set_style_border_width(qr, 6, 0);
        snprintf(t, sizeof(t), "1. Mit diesem WLAN verbinden:\n%s\nPasswort: %s\n2. Im Browser oeffnen:\n%s",
                 WEB_HOTSPOT_NAME, webHotspotPasswort, webAdresse.c_str());
        lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 238);
    } else {
        snprintf(t, sizeof(t), "Im Heimnetz verbunden.\nIm Browser oeffnen:\n%s\nDann diesen Code eingeben:", webAdresse.c_str());
        lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_t *code = lv_label_create(web_screen);
        lv_label_set_text(code, webCode);
        lv_obj_set_style_text_font(code, &lv_font_montserrat_48, 0);
        lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 200);
    }
    lv_label_set_text(info, t);

    web_zeit_label = lv_label_create(web_screen);
    lv_obj_set_style_text_font(web_zeit_label, &lv_font_montserrat_20, 0);
    lv_obj_align(web_zeit_label, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_label_set_text(web_zeit_label, "");

    lv_obj_t *knopf = lv_button_create(web_screen);
    lv_obj_set_size(knopf, 280, 50);
    lv_obj_align(knopf, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_t *kt = lv_label_create(knopf);
    lv_label_set_text(kt, "Beenden (WLAN aus)");
    lv_obj_center(kt);
    lv_obj_add_event_cb(knopf, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) webBeendenWunsch = true;
    }, LV_EVENT_CLICKED, NULL);
}

static bool webStarten(bool hotspot) {
    funkStoppen();                 // ESP-NOW (Konto-Funk) darf nicht parallel laufen
    webHotspot = hotspot;
    webTokenNoetig = !hotspot;
    webFehlversuche = 0;
    WiFi.persistent(false);
    if (hotspot) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(100);
        webNetzListeAufbauen();    // erst suchen, dann den Hotspot aufspannen
        webZufallPasswort(webHotspotPasswort, sizeof(webHotspotPasswort));
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(WEB_HOTSPOT_NAME, webHotspotPasswort, WEB_HOTSPOT_KANAL, 0, 2)) {
            WiFi.mode(WIFI_OFF);
            return false;
        }
        webAdresse = "http://" + WiFi.softAPIP().toString();
    } else {
        if (!wlanVerbinden()) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
        webAdresse = "http://" + WiFi.localIP().toString();
        snprintf(webCode, sizeof(webCode), "%06u", (unsigned)(esp_random() % 1000000UL));
        uint32_t a = esp_random(), b = esp_random();
        snprintf(webToken, sizeof(webToken), "%08x%08x", (unsigned)a, (unsigned)b);
        webNetzListeAufbauen();
    }
    webServerStarten();
    webEnde = millis() + (unsigned long)WEB_EINRICHTUNG_MINUTEN * 60000UL;
    webAktiv = true;
    webScreenErstellen();
    aktuellerStatus = STATUS_WEB;
    lv_screen_load(web_screen);
    Serial.printf("Einrichtung per Handy gestartet (%s).\n", hotspot ? "Hotspot" : "Heimnetz");
    return true;
}

static void webBeenden() {
    webBeendenWunsch = false;
    if (!webAktiv) return;
    webAktiv = false;
    if (webServer != NULL) webServer->stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    webTokenNoetig = false;
    memset(webHotspotPasswort, 0, sizeof(webHotspotPasswort));
    memset(webCode, 0, sizeof(webCode));
    memset(webToken, 0, sizeof(webToken));
    zeigeHauptmenue();
    if (web_screen != NULL) { lv_obj_delete(web_screen); web_screen = NULL; web_zeit_label = NULL; }
    Serial.println("Einrichtung per Handy beendet, WLAN aus.");
}

// Wird in jedem loop()-Durchlauf aufgerufen
static void webVerwalten() {
    if (webStartWunsch != 0 && !webAktiv) {
        int wunsch = webStartWunsch;
        webStartWunsch = 0;
        lv_timer_handler();        // "Bitte warten" erst zeichnen
        if (!webStarten(wunsch == 1) && web_start_status != NULL) {
            lv_label_set_text(web_start_status, wunsch == 1 ? "Hotspot konnte nicht gestartet werden."
                                                             : "Kein bekanntes WLAN gefunden.\nBitte den Hotspot nehmen.");
        }
        return;
    }
    if (!webAktiv) return;
    webServer->handleClient();
    if (webNeustartWunsch && millis() - webNeustartZeit > 1500) {
        Serial.println("Einstellungen gespeichert - Neustart.");
        delay(100);
        ESP.restart();
    }
    static unsigned long letzteAnzeige = 0;
    unsigned long jetzt = millis();
    if (web_zeit_label != NULL && jetzt - letzteAnzeige >= 1000) {
        letzteAnzeige = jetzt;
        long rest = (long)(webEnde - jetzt) / 1000L;
        if (rest < 0) rest = 0;
        char t[40];
        snprintf(t, sizeof(t), "Noch %ld:%02ld min", rest / 60L, rest % 60L);
        lv_label_set_text(web_zeit_label, t);
    }
    if (webBeendenWunsch || (long)(jetzt - webEnde) >= 0) webBeenden();
}

// ---- Auswahlbild ----
void zeigeWebStart() {
    if (web_start_screen == NULL) {
        web_start_screen = lv_obj_create(NULL);
        zurueckKnopf(web_start_screen, LV_SYMBOL_LEFT " Menue", [](lv_event_t *e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) zeigeHauptmenue();
        });
        lv_obj_t *titel = lv_label_create(web_start_screen);
        lv_label_set_text(titel, "Einrichtung per Handy");
        lv_obj_set_style_text_font(titel, &lv_font_montserrat_28, 0);
        lv_obj_align(titel, LV_ALIGN_TOP_RIGHT, -20, 18);

        lv_obj_t *text = lv_label_create(web_start_screen);
        lv_obj_set_width(text, 440);
        lv_label_set_text(text, "Schaltet das WLAN fuer einige Minuten ein. Mit Handy oder PC lassen sich dann "
                                "WLAN-Netze, Wetter-Ort, Anzeige, Ton und QR-Code einstellen. "
                                "Danach schaltet sich das WLAN wieder ab.");
        lv_obj_align(text, LV_ALIGN_TOP_MID, 0, 90);

        lv_obj_t *k1 = lv_button_create(web_start_screen);
        lv_obj_set_size(k1, 380, 56);
        lv_obj_align(k1, LV_ALIGN_TOP_MID, 0, 220);
        lv_obj_t *t1 = lv_label_create(k1);
        lv_label_set_text(t1, LV_SYMBOL_WIFI " Hotspot (empfohlen)");
        lv_obj_center(t1);
        lv_obj_add_event_cb(k1, [](lv_event_t *e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            lv_label_set_text(web_start_status, "Bitte warten ...");
            webStartWunsch = 1;
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *k2 = lv_button_create(web_start_screen);
        lv_obj_set_size(k2, 380, 56);
        lv_obj_align(k2, LV_ALIGN_TOP_MID, 0, 290);
        lv_obj_t *t2 = lv_label_create(k2);
        lv_label_set_text(t2, LV_SYMBOL_WIFI " Im Heimnetz");
        lv_obj_center(t2);
        lv_obj_add_event_cb(k2, [](lv_event_t *e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            lv_label_set_text(web_start_status, "Bitte warten ...");
            webStartWunsch = 2;
        }, LV_EVENT_CLICKED, NULL);

        web_start_status = lv_label_create(web_start_screen);
        lv_obj_set_width(web_start_status, 440);
        lv_obj_set_style_text_align(web_start_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(web_start_status, LV_ALIGN_TOP_MID, 0, 365);
        lv_label_set_text(web_start_status, "");
    }
    lv_label_set_text(web_start_status, "");
    aktuellerStatus = STATUS_HAUPTMENUE;       // normaler Timeout wie im Menue
    lv_screen_load(web_start_screen);
}

#else   // Einrichtung nicht eingebaut: leere Platzhalter
void zeigeWebStart() {}
static void webVerwalten() {}
#endif
