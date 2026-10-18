# Kamera / Scanner (AtomS3R-CAM)

```text
Dieser Ordner ist ein eigenes Arduino-Programm fuer die Kamera.
- Ordner und .ino-Datei heissen gleich (atoms3r_totp_scanner_funk).
- Die Dateien funk_protokoll.h und konfiguration_funk.h muessen
  im selben Ordner liegen.
- In konfiguration_funk.h muessen Schluessel (FUNK_SCHLUESSEL) und
  Kanal (FUNK_KANAL) EXAKT dieselben sein wie beim Waveshare.
  Solange der Schluessel nur aus Nullen besteht, sendet nichts.
- Ablauf: QR-Code scannen -> Kamera sendet verschluesselt per
  ESP-NOW -> Waveshare speichert das Konto und antwortet.
```
