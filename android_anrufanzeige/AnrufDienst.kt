// SPDX-License-Identifier: GPL-3.0-only
package de.bahnhofsuhr.anruf

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.provider.ContactsContract
import android.provider.Telephony
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

/**
 * Läuft im Vordergrund (mit Symbol in der Statusleiste). Hört auf Anrufe und eingehende SMS und
 * schickt sie verschlüsselt per Bluetooth Low Energy an das gekoppelte Waveshare:
 *   RING|<SIM 1 oder 2, 0 = unbekannt>|<Nummer>|<Name>      bei eingehendem Anruf
 *   IDLE                                                     wenn der Anruf endet / angenommen wird
 *   SMS|<Kontaktname oder Absender>|<Text>                   bei eingehender SMS
 * Der SMS-Text wird nur durchgereicht: nicht gespeichert, nicht protokolliert, nicht angezeigt.
 */
@SuppressLint("MissingPermission")
class AnrufDienst : Service() {

    companion object {
        @Volatile var status: String = "Dienst aus"
        @Volatile var letzterAnruf: String = "-"
        @Volatile var debug: String = "-"
        @Volatile var letzteSms: String = "-"      // nur Uhrzeit und Länge, nie der Inhalt
        @Volatile var smsEmpfang: String = "aus"   // ob der SMS-Empfänger angemeldet ist

        @Volatile var instance: AnrufDienst? = null   // damit FritzListener Anrufe der FRITZ!App Fon übergeben kann

        const val AKTION_TEST = "de.bahnhofsuhr.anruf.TEST"
        const val AKTION_TEST_SMS = "de.bahnhofsuhr.anruf.TEST_SMS"
        private const val KANAL = "dienst"
        private const val NOTIZ_ID = 1
        private const val MAX_BYTES = 200          // Muss zum Puffer BLE_NACHRICHT_MAX (210) im Waveshare passen

        // Muss zu konfiguration_ble.h im Waveshare-Programm passen
        val SERVICE_UUID: UUID = UUID.fromString("b7a0c7e0-1c4a-4f6e-9a3e-5d1c0a7e0001")
        val ANRUF_UUID: UUID = UUID.fromString("b7a0c7e0-1c4a-4f6e-9a3e-5d1c0a7e0002")
    }

    private val handler = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var anrufEigenschaft: BluetoothGattCharacteristic? = null
    private val warteschlange = ArrayDeque<ByteArray>()
    private var schreibtGerade = false

    // Anruf-Zwischenspeicher
    private var ringNummer = ""
    private var ringSim = 0
    private var ringMitNummerGesendet = false
    private val ringSenden = Runnable { sendeRing() }

    private var smsAngemeldet = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        val kanal = NotificationChannel(KANAL, "Anrufanzeige", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(kanal)
        registerReceiver(
            telefonEmpfaenger,
            IntentFilter(TelephonyManager.ACTION_PHONE_STATE_CHANGED),
            Context.RECEIVER_EXPORTED
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIZ_ID, benachrichtigung(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        smsEmpfaengerAnmelden()
        if (gatt == null) verbinden()
        when (intent?.action) {
            AKTION_TEST -> testanruf()
            AKTION_TEST_SMS -> testSms()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        instance = null
        handler.removeCallbacksAndMessages(null)
        try { unregisterReceiver(telefonEmpfaenger) } catch (_: Exception) {}
        try { unregisterReceiver(smsEmpfaenger) } catch (_: Exception) {}
        smsAngemeldet = false
        smsEmpfang = "aus"
        gatt?.close()
        gatt = null
        status = "Dienst aus"
        super.onDestroy()
    }

    // ---------------- Benachrichtigung ----------------
    private fun benachrichtigung(): Notification =
        Notification.Builder(this, KANAL)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Anrufanzeige aktiv")
            .setContentText(status)
            .setOngoing(true)
            .build()

    private fun setzeStatus(neu: String) {
        status = neu
        getSystemService(NotificationManager::class.java).notify(NOTIZ_ID, benachrichtigung())
    }

    // ---------------- Bluetooth ----------------
    private fun verbinden() {
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            setzeStatus("Berechtigung Bluetooth fehlt")
            return
        }
        val adresse = getSharedPreferences("einstellungen", MODE_PRIVATE).getString("geraet", null)
        if (adresse == null) {
            setzeStatus("Kein Waveshare gewählt")
            return
        }
        val adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (adapter == null || !adapter.isEnabled) {
            setzeStatus("Bluetooth ist aus")
            return
        }
        val geraet = adapter.getRemoteDevice(adresse)
        gatt?.close()
        setzeStatus("Verbinde mit Waveshare ...")
        // autoConnect = true: Android verbindet von selbst neu, sobald das Waveshare in Reichweite ist
        gatt = geraet.connectGatt(this, true, gattRueckruf, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattRueckruf = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, st: Int, neu: Int) {
            handler.post {
                if (neu == BluetoothProfile.STATE_CONNECTED) {
                    setzeStatus("Verbunden, richte ein ...")
                    if (!g.requestMtu(247)) g.discoverServices()
                } else if (neu == BluetoothProfile.STATE_DISCONNECTED) {
                    anrufEigenschaft = null
                    warteschlange.clear()
                    schreibtGerade = false
                    setzeStatus("Getrennt - warte auf Waveshare")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, st: Int) {
            handler.post { debug = "MTU $mtu"; g.discoverServices() }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, st: Int) {
            handler.post {
                anrufEigenschaft = g.getService(SERVICE_UUID)?.getCharacteristic(ANRUF_UUID)
                if (anrufEigenschaft != null) setzeStatus("Bereit (verbunden)")
                else setzeStatus("Dienst am Waveshare nicht gefunden")
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, st: Int) {
            handler.post {
                schreibtGerade = false
                if (st != BluetoothGatt.GATT_SUCCESS) debug = "Schreibfehler $st"
                naechstesSenden()
            }
        }
    }

    private fun senden(text: String) {
        // An einer Zeichengrenze kürzen, damit kein halbes Umlaut-Zeichen übrig bleibt
        val bytes = kuerzeUtf8(text, MAX_BYTES).toByteArray(Charsets.UTF_8)
        handler.post {
            warteschlange.addLast(bytes)
            naechstesSenden()
        }
    }

    private fun naechstesSenden() {
        if (schreibtGerade) return
        val eigenschaft = anrufEigenschaft ?: run { warteschlange.clear(); return }
        val verbindung = gatt ?: return
        val daten = warteschlange.removeFirstOrNull() ?: return
        schreibtGerade = true
        val ergebnis = verbindung.writeCharacteristic(eigenschaft, daten, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        if (ergebnis != BluetoothStatusCodes.SUCCESS) {
            schreibtGerade = false
            debug = "Senden abgelehnt ($ergebnis)"
        }
    }

    // ---------------- Anrufe ----------------
    private val telefonEmpfaenger = object : BroadcastReceiver() {
        @Suppress("DEPRECATION")
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return
            val zustand = intent.getStringExtra(TelephonyManager.EXTRA_STATE)
            val nummer = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)
            when (zustand) {
                TelephonyManager.EXTRA_STATE_RINGING -> klingelt(nummer, simSlot(intent))
                TelephonyManager.EXTRA_STATE_OFFHOOK, TelephonyManager.EXTRA_STATE_IDLE -> beendet()
            }
        }
    }

    /** Welche SIM-Karte? Liefert 1 oder 2, 0 wenn unbekannt. */
    private fun simSlot(intent: Intent): Int {
        debug = intent.extras?.keySet()?.joinToString(",") ?: "-"
        val subId = intent.getIntExtra(
            SubscriptionManager.EXTRA_SUBSCRIPTION_INDEX,
            intent.getIntExtra("subscription", -1)
        )
        if (subId >= 0) {
            try {
                val info = getSystemService(SubscriptionManager::class.java).getActiveSubscriptionInfo(subId)
                if (info != null) return info.simSlotIndex + 1
            } catch (_: Exception) {}
        }
        for (schluessel in arrayOf("slot", "android.telephony.extra.SLOT_INDEX", "simSlot")) {
            val s = intent.getIntExtra(schluessel, -1)
            if (s >= 0) return s + 1
        }
        return 0
    }

    private fun klingelt(nummer: String?, sim: Int) {
        handler.removeCallbacks(ringSenden)
        val neueNummer = nummer ?: ""
        if (neueNummer.isNotEmpty() && ringMitNummerGesendet && neueNummer == ringNummer) return
        ringNummer = neueNummer
        if (sim != 0) ringSim = sim
        if (neueNummer.isNotEmpty()) {
            sendeRing()
        } else {
            // Android meldet oft zuerst ohne Nummer; kurz auf die zweite Meldung warten
            handler.postDelayed(ringSenden, 800)
        }
    }

    private fun sendeRing() {
        val name = kontaktName(ringNummer)
        val text = "RING|$ringSim|${bereinigen(ringNummer)}|${bereinigen(name)}"
        if (ringNummer.isNotEmpty()) ringMitNummerGesendet = true
        letzterAnruf = "SIM $ringSim, " + (if (name.isNotEmpty()) "Kontakt bekannt" else "Kontakt unbekannt")
        senden(text)
    }

    private fun beendet() {
        handler.removeCallbacks(ringSenden)
        if (ringNummer.isNotEmpty() || ringSim != 0 || ringMitNummerGesendet) senden("IDLE")
        ringNummer = ""
        ringSim = 0
        ringMitNummerGesendet = false
    }

    private fun bereinigen(s: String) =
        s.replace("|", " ").replace("\r", " ").replace("\n", " ").replace("\t", " ").trim()

    /** Kürzt auf höchstens maxBytes Byte (UTF-8), ohne ein Zeichen zu zerschneiden. */
    private fun kuerzeUtf8(s: String, maxBytes: Int): String {
        var bytes = 0
        var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i)
            val n = when {
                cp < 0x80 -> 1
                cp < 0x800 -> 2
                cp < 0x10000 -> 3
                else -> 4
            }
            if (bytes + n > maxBytes) break
            bytes += n
            i += Character.charCount(cp)
        }
        return s.substring(0, i)
    }

    private fun kontaktName(nummer: String): String {
        if (nummer.isEmpty() || checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) return ""
        return try {
            val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(nummer))
            contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) it.getString(0) ?: "" else ""
            } ?: ""
        } catch (_: Exception) {
            ""
        }
    }

    // ---------------- FRITZ!App Fon (über FritzListener) ----------------
    private var fritzSchluessel = ""
    // Ende mit kurzer Verzögerung: die FRITZ!App Fon aktualisiert ihre Meldung beim Klingeln mehrfach.
    // Kommt gleich danach wieder ein Klingeln, verfällt das Ende (sonst blinkt der Bildschirm).
    private var fritzEndeZeit = 0L
    private val fritzEndeAufgabe = Runnable {
        fritzSchluessel = ""
        fritzEndeZeit = SystemClock.elapsedRealtime()
        beendet()
    }

    /** True kurz nach dem Ende eines Anrufs: Abschlussmeldungen ("Anruf beendet") sind kein neuer Anruf. */
    fun fritzKuerzlichBeendet(): Boolean =
        fritzEndeZeit != 0L && SystemClock.elapsedRealtime() - fritzEndeZeit < 5000

    /** Eingehender Anruf der FRITZ!App Fon. Nummer und Name kommen aus deren Benachrichtigung. */
    fun fritzAnruf(nummer: String, name: String) {
        handler.post {
            handler.removeCallbacks(fritzEndeAufgabe)
            val schluessel = "$nummer|$name"
            if (ringMitNummerGesendet && schluessel == fritzSchluessel) return@post
            fritzSchluessel = schluessel
            handler.removeCallbacks(ringSenden)
            ringNummer = nummer
            ringSim = 0
            val anzeigeName = if (name.isNotEmpty()) name else kontaktName(nummer)
            ringMitNummerGesendet = true
            letzterAnruf = "Fritz!Fon, " + (if (anzeigeName.isNotEmpty()) "Name bekannt" else "Name unbekannt")
            senden("RING|0|${bereinigen(nummer)}|${bereinigen(anzeigeName)}")
        }
    }

    /** Der Fritz!Fon-Anruf ist beendet oder angenommen. */
    fun fritzEnde() {
        handler.post {
            handler.removeCallbacks(fritzEndeAufgabe)
            handler.postDelayed(fritzEndeAufgabe, 1500)
        }
    }

    // ---------------- SMS ----------------
    /** Meldet den SMS-Empfänger an, sobald die Berechtigung da ist (auch nachträglich). */
    private fun smsEmpfaengerAnmelden() {
        if (smsAngemeldet) return
        if (checkSelfPermission(Manifest.permission.RECEIVE_SMS) != PackageManager.PERMISSION_GRANTED) {
            smsEmpfang = "aus (Berechtigung SMS fehlt)"
            return
        }
        // BROADCAST_SMS als Sendeberechtigung: nur das Telefonsystem darf an diesen Empfänger senden
        registerReceiver(
            smsEmpfaenger,
            IntentFilter(Telephony.Sms.Intents.SMS_RECEIVED_ACTION),
            Manifest.permission.BROADCAST_SMS,
            null,
            Context.RECEIVER_EXPORTED
        )
        smsAngemeldet = true
        smsEmpfang = "an"
    }

    private val smsEmpfaenger = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != Telephony.Sms.Intents.SMS_RECEIVED_ACTION) return
            val teile = Telephony.Sms.Intents.getMessagesFromIntent(intent)
            if (teile.isNullOrEmpty()) return
            val absender = teile[0].displayOriginatingAddress ?: ""
            // Lange SMS kommen in Teilen; sie gehören zusammen
            val text = teile.joinToString("") { it.displayMessageBody ?: "" }
            sendeSms(absender, text)
        }
    }

    private fun sendeSms(absender: String, text: String) {
        val name = kontaktName(absender)
        val von = bereinigen(if (name.isNotEmpty()) name else absender).take(24)
        val kopf = "SMS|$von|"
        val platz = MAX_BYTES - kopf.toByteArray(Charsets.UTF_8).size
        val rumpf = kuerzeUtf8(bereinigen(text), platz)
        letzteSms = SimpleDateFormat("HH:mm:ss", Locale.GERMANY).format(Date()) + ", " + text.length + " Zeichen"
        senden(kopf + rumpf)
    }

    // ---------------- Test ----------------
    private fun testanruf() {
        ringNummer = "0301234567"
        ringSim = 1
        sendeRing()
        handler.postDelayed({ beendet() }, 6000)
    }

    private fun testSms() {
        sendeSms("Testabsender", "Ihr Code lautet 482915. Er ist 10 Minuten gültig. Bitte nicht weitergeben.")
    }
}
