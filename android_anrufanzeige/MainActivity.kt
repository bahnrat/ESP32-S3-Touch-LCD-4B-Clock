// SPDX-License-Identifier: GPL-3.0-only
package de.bahnhofsuhr.anruf

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.app.NotificationManager
import android.content.ComponentName
import android.provider.Settings
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

/**
 * Einfache Oberfläche: Berechtigungen erlauben, Waveshare wählen, Dienst starten, testen.
 * Die eigentliche Arbeit macht AnrufDienst.
 */
class MainActivity : Activity() {

    private val berechtigungen = arrayOf(
        Manifest.permission.READ_PHONE_STATE,
        Manifest.permission.READ_CALL_LOG,
        Manifest.permission.READ_CONTACTS,
        Manifest.permission.RECEIVE_SMS,
        Manifest.permission.BLUETOOTH_CONNECT,
        Manifest.permission.POST_NOTIFICATIONS
    )

    private lateinit var statusText: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val aktualisieren = object : Runnable {
        override fun run() {
            statusText.text = baueStatus()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val spalte = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40, 60, 40, 40)
        }
        val titel = TextView(this).apply {
            text = "Anrufanzeige für die Bahnhofsuhr"
            textSize = 22f
        }
        statusText = TextView(this).apply {
            textSize = 15f
            setPadding(0, 30, 0, 30)
        }
        spalte.addView(titel)
        spalte.addView(statusText)

        fun knopf(text: String, aktion: () -> Unit) {
            spalte.addView(Button(this).apply {
                this.text = text
                setOnClickListener { aktion() }
            })
        }

        knopf("1. Berechtigungen erlauben") { requestPermissions(berechtigungen, 1) }
        knopf("2. Waveshare auswählen") { geraetWaehlen() }
        knopf("3. Dienst starten") { dienstStarten() }
        knopf("Dienst beenden") { stopService(Intent(this, AnrufDienst::class.java)) }
        knopf("Testanruf senden") { dienstBefehl(AnrufDienst.AKTION_TEST) }
        knopf("Test-SMS senden") { dienstBefehl(AnrufDienst.AKTION_TEST_SMS) }
        knopf("4. Fritz!Fon: Benachrichtigungszugriff erlauben") {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        knopf("Fritz-Diagnose an/aus") { FritzListener.diagnoseAn = !FritzListener.diagnoseAn }

        setContentView(ScrollView(this).apply { addView(spalte) })
    }

    override fun onResume() {
        super.onResume()
        handler.post(aktualisieren)
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(aktualisieren)
    }

    private fun fehlendeBerechtigungen(): List<String> =
        berechtigungen.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            .map { it.substringAfterLast('.') }

    private fun baueStatus(): String {
        val prefs = getSharedPreferences("einstellungen", MODE_PRIVATE)
        val adresse = prefs.getString("geraet", null)
        val fehlt = fehlendeBerechtigungen()
        return buildString {
            append("Waveshare: ").append(adresse ?: "noch nicht gewählt").append("\n")
            append("Berechtigungen: ").append(if (fehlt.isEmpty()) "alle erteilt" else "fehlen: " + fehlt.joinToString()).append("\n")
            append("Dienst: ").append(AnrufDienst.status).append("\n")
            append("Letzter Anruf (ohne Nummer): ").append(AnrufDienst.letzterAnruf).append("\n")
            append("SMS-Empfang: ").append(AnrufDienst.smsEmpfang).append("\n")
            append("Letzte SMS (ohne Inhalt): ").append(AnrufDienst.letzteSms).append("\n")
            append("Fritz!Fon-Erkennung: ").append(if (fritzZugriff()) "Zugriff erlaubt, " else "Zugriff fehlt, ")
                .append(FritzListener.status).append("\n")
            if (FritzListener.diagnoseAn) {
                append("Fritz-Diagnose AN. Gesehene Apps: ").append(FritzListener.apps).append("\n")
                append("Letzte Fritz-Meldung: ").append(FritzListener.diagnose).append("\n")
            }
            append("Technik: ").append(AnrufDienst.debug)
        }
    }

    private fun fritzZugriff(): Boolean =
        getSystemService(NotificationManager::class.java)
            .isNotificationListenerAccessGranted(ComponentName(this, FritzListener::class.java))

    // Nach dem Erlauben den Dienst neu starten, damit er den SMS-Empfang anmeldet
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (getSharedPreferences("einstellungen", MODE_PRIVATE).getString("geraet", null) != null) dienstStarten()
    }

    private fun geraetWaehlen() {
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "Erst Berechtigungen erlauben", Toast.LENGTH_LONG).show()
            return
        }
        val adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val geraete = adapter.bondedDevices.toList()
        if (geraete.isEmpty()) {
            Toast.makeText(this, "Keine gekoppelten Geräte. Erst in nRF Connect koppeln.", Toast.LENGTH_LONG).show()
            return
        }
        val namen = geraete.map { (it.name ?: "?") + "  " + it.address }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Waveshare wählen")
            .setItems(namen) { _, welches ->
                getSharedPreferences("einstellungen", MODE_PRIVATE).edit()
                    .putString("geraet", geraete[welches].address).apply()
                stopService(Intent(this, AnrufDienst::class.java))
                dienstStarten()
            }
            .show()
    }

    private fun dienstStarten() {
        startForegroundService(Intent(this, AnrufDienst::class.java))
    }

    private fun dienstBefehl(aktion: String) {
        startForegroundService(Intent(this, AnrufDienst::class.java).setAction(aktion))
    }
}
