// SPDX-License-Identifier: GPL-3.0-only
// ============================================================
//  funk_protokoll.h  -  gemeinsame Verschluesselung fuer Scanner UND Anzeige
//  Diese Datei muss in BEIDEN Sketch-Ordnern liegen und identisch sein.
//
//  Aufbau eines Funkpakets (ESP-NOW, max. 250 Byte):
//     [12 Byte Nonce][verschluesselter Text][16 Byte Pruefwert (GCM-Tag)]
//  Verschluesselt wird mit AES-128-GCM. Ohne den gemeinsamen Schluessel kann
//  niemand den Inhalt lesen und niemand ein gueltiges Paket fuellen/aendern.
//  Klartext-Formate (vor dem Verschluesseln):
//     "TOTP|<Label>|<Secret>"   Scanner -> Anzeige
//     "INFO|<Text>"             Scanner -> Anzeige (nur Anzeige, nie ein Secret)
//     "OK" / "DUP" / "ERR"      Anzeige -> Scanner
// ============================================================
#pragma once
#include <Arduino.h>
#include <mbedtls/gcm.h>
#include <esp_random.h>

const size_t FUNK_NONCE_LAENGE = 12;
const size_t FUNK_TAG_LAENGE = 16;
const size_t FUNK_MAX_PAKET = 250;
const size_t FUNK_MAX_KLARTEXT = FUNK_MAX_PAKET - FUNK_NONCE_LAENGE - FUNK_TAG_LAENGE; // 222
static const uint8_t FUNK_AAD[] = {'T', 'O', 'T', 'P', 'C', 'A', 'M', '1'};          // Protokollkennung

// Schluessel nur aus Nullen = nicht eingerichtet
inline bool funkSchluesselGesetzt(const uint8_t *schluessel) {
    for (int i = 0; i < 16; i++) {
        if (schluessel[i] != 0) {
            return true;
        }
    }
    return false;
}

// Verschluesselt klartext. paket muss FUNK_MAX_PAKET Byte gross sein. Rueckgabe: Paketlaenge, 0 = Fehler.
inline size_t funkVerschluesseln(const uint8_t *schluessel, const String &klartext, uint8_t *paket) {
    size_t laenge = klartext.length();
    if (laenge == 0 || laenge > FUNK_MAX_KLARTEXT) {
        return 0;
    }
    esp_fill_random(paket, FUNK_NONCE_LAENGE); // Nonce aus dem Hardware-Zufallsgenerator
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int fehler = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, schluessel, 128);
    if (fehler == 0) {
        fehler = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, laenge,
                                           paket, FUNK_NONCE_LAENGE,
                                           FUNK_AAD, sizeof(FUNK_AAD),
                                           (const uint8_t *)klartext.c_str(),
                                           paket + FUNK_NONCE_LAENGE,
                                           FUNK_TAG_LAENGE,
                                           paket + FUNK_NONCE_LAENGE + laenge);
    }
    mbedtls_gcm_free(&gcm);
    return (fehler == 0) ? (FUNK_NONCE_LAENGE + laenge + FUNK_TAG_LAENGE) : 0;
}

// Entschluesselt und prueft. true = Paket echt, klartext enthaelt den Inhalt.
inline bool funkEntschluesseln(const uint8_t *schluessel, const uint8_t *paket, size_t paketLaenge, String *klartext) {
    if (paketLaenge <= FUNK_NONCE_LAENGE + FUNK_TAG_LAENGE || paketLaenge > FUNK_MAX_PAKET) {
        return false;
    }
    size_t laenge = paketLaenge - FUNK_NONCE_LAENGE - FUNK_TAG_LAENGE;
    uint8_t ausgabe[FUNK_MAX_KLARTEXT + 1];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int fehler = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, schluessel, 128);
    if (fehler == 0) {
        fehler = mbedtls_gcm_auth_decrypt(&gcm, laenge,
                                          paket, FUNK_NONCE_LAENGE,
                                          FUNK_AAD, sizeof(FUNK_AAD),
                                          paket + FUNK_NONCE_LAENGE + laenge, FUNK_TAG_LAENGE,
                                          paket + FUNK_NONCE_LAENGE, ausgabe);
    }
    mbedtls_gcm_free(&gcm);
    if (fehler != 0) {
        return false; // falscher Schluessel oder veraendertes Paket
    }
    ausgabe[laenge] = 0;
    *klartext = String((const char *)ausgabe);
    return true;
}
