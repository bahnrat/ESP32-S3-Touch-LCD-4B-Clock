#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
Holt ein Bildschirmfoto vom Waveshare-Display ueber USB und speichert es als PNG.

Voraussetzungen (einmalig):   pip install pyserial pillow numpy
Aufruf:                       python bildschirmfoto.py COM3
                              python bildschirmfoto.py COM3 mein_bild.png
Den Seriellen Monitor der Arduino IDE vorher schliessen!
"""
import sys
import time
import datetime
import serial          # pyserial
import numpy as np
from PIL import Image

if len(sys.argv) < 2:
    print("Aufruf: python bildschirmfoto.py COM3 [datei.png]")
    sys.exit(1)

port = sys.argv[1]
datei = sys.argv[2] if len(sys.argv) > 2 else datetime.datetime.now().strftime("bildschirm_%Y%m%d_%H%M%S.png")

VERSUCHE = 5


def hole_bild():
    """Ein Versuch. Gibt (breite, hoehe, daten) zurueck oder (0, 0, None), wenn nichts kam."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 1
    ser.dtr = False   # verhindert moeglichst den Reset beim Oeffnen
    ser.rts = False
    ser.open()
    try:
        ser.reset_input_buffer()
        breite = hoehe = 0
        ende = time.time() + 40
        letzter_versuch = 0
        while time.time() < ende and breite == 0:
            if time.time() - letzter_versuch > 3:
                ser.write(b"S")
                letzter_versuch = time.time()
            zeile = ser.readline().decode("ascii", errors="ignore").strip()
            if zeile.startswith("SCREENSHOT-FEHLER"):
                print(zeile)
                sys.exit(2)
            if zeile.startswith("SCREENSHOT "):
                teile = zeile.split()
                breite, hoehe = int(teile[1]), int(teile[2])
        if breite == 0:
            return 0, 0, None
        ser.baudrate = 921600
        ser.timeout = 20
        anzahl = breite * hoehe * 2
        print(f"Empfange {breite}x{hoehe} ({anzahl} Bytes) ...")
        daten = ser.read(anzahl)
        return breite, hoehe, daten
    finally:
        try:
            ser.baudrate = 115200
            time.sleep(1.5)          # Reste der Uebertragung abwarten
            ser.reset_input_buffer()
        except Exception:
            pass
        ser.close()


print("Fordere Bildschirmfoto an ...")
for versuch in range(1, VERSUCHE + 1):
    breite, hoehe, daten = hole_bild()
    if breite == 0:
        print("Keine Antwort vom Geraet. Ist die neue Firmware drauf, und der Monitor geschlossen?")
        sys.exit(3)
    if len(daten) == breite * hoehe * 2:
        break
    print(f"Unvollstaendig: {len(daten)} von {breite * hoehe * 2} Bytes (Versuch {versuch} von {VERSUCHE}).")
    time.sleep(2)
else:
    print("Auch nach mehreren Versuchen unvollstaendig. USB-Kabel/Port wechseln und den Monitor schliessen.")
    sys.exit(4)

wert = np.frombuffer(daten, dtype="<u2").reshape(hoehe, breite).astype(np.uint32)
rgb = np.zeros((hoehe, breite, 3), dtype=np.uint8)
rgb[..., 0] = (((wert >> 11) & 0x1F) * 255 // 31).astype(np.uint8)
rgb[..., 1] = (((wert >> 5) & 0x3F) * 255 // 63).astype(np.uint8)
rgb[..., 2] = ((wert & 0x1F) * 255 // 31).astype(np.uint8)
Image.fromarray(rgb, "RGB").save(datei)
print("Gespeichert:", datei)
