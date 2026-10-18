// SPDX-License-Identifier: GPL-3.0-only
package de.bahnhofsuhr.anruf

import android.app.Notification
import android.app.Person
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Liest NUR die Benachrichtigungen der FRITZ!App Fon (Paketname enthält "fritz" oder "avm") und
 * meldet eingehende Anrufe an AnrufDienst, der sie per Bluetooth an das Waveshare schickt.
 * Alle anderen Benachrichtigungen werden sofort verworfen: nicht gelesen, nicht gespeichert.
 * Nichts wird protokolliert. Nur im Diagnose-Modus (per Knopf, standardmäßig aus) merkt sich die App
 * im Arbeitsspeicher die Paketnamen der zuletzt gesehenen Apps (ohne Inhalt) und den Text der
 * letzten Fritz-Benachrichtigung, damit die Erkennung angepasst werden kann.
 */
class FritzListener : NotificationListenerService() {

    companion object {
        @Volatile var status: String = "noch nichts gesehen"
        @Volatile var diagnoseAn: Boolean = false
        @Volatile var apps: String = "-"
        @Volatile var diagnose: String = "-"

        private val NUMMER = Regex("\\+?\\d[\\d ()/\\-]{4,}\\d")
        private val ALLGEMEIN = listOf("anruf", "call", "eingehend", "incoming", "fritz", "ruft an", "telefon", "annehmen")
        private val IM_GESPRAECH = listOf("gespräch", "gespraech", "verbunden", "laufend", "ongoing", "in call",
            "beendet", "aufgelegt", "ended")
    }

    private var klingelSchluessel: String? = null
    private val gesehen = ArrayList<String>()

    private fun istFritz(paket: String): Boolean {
        val p = paket.lowercase(Locale.ROOT)
        return p.contains("fritz") || p.contains("avm")
    }

    override fun onListenerConnected() {
        status = "Zugriff aktiv"
    }

    override fun onListenerDisconnected() {
        status = "Zugriff getrennt"
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val paket = sbn.packageName ?: return
        if (!istFritz(paket)) {
            if (diagnoseAn) merkePaket(paket)
            return
        }
        if (diagnoseAn) merkePaket(paket)

        val n = sbn.notification
        val e = n.extras
        val titel = e.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val text = e.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        val unter = e.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString() ?: ""
        val typ = e.getInt(Notification.EXTRA_CALL_TYPE, 0)          // 1 = eingehend, 2 = laufend (CallStyle)
        val person = try { e.getParcelable(Notification.EXTRA_CALL_PERSON, Person::class.java) } catch (_: Exception) { null }
        val personName = person?.name?.toString() ?: ""
        val alles = "$titel $text $unter $personName".lowercase(Locale.GERMANY)

        val verpasst = alles.contains("verpasst") || alles.contains("missed")
        val imGespraech = typ == 2 || e.getBoolean(Notification.EXTRA_SHOW_CHRONOMETER) ||
            IM_GESPRAECH.any { alles.contains(it) }
        val kategorieAnruf = n.category == Notification.CATEGORY_CALL
        val klingelt = !verpasst && !imGespraech &&
            (typ == 1 || kategorieAnruf || alles.contains("anruf") || alles.contains("incoming") || alles.contains("ruft an"))

        if (diagnoseAn) {
            diagnose = "Paket=$paket Kat=${n.category} Typ=$typ Chrono=${e.getBoolean(Notification.EXTRA_SHOW_CHRONOMETER)} " +
                "Person='$personName' Titel='$titel' Text='$text' Sub='$unter' -> " +
                (if (klingelt) "KLINGELT" else if (imGespraech) "im Gespraech" else if (verpasst) "verpasst" else "ignoriert")
        }

        if (klingelt && AnrufDienst.instance?.fritzKuerzlichBeendet() == true) {
            // Abschlussmeldung kurz nach dem Ende eines Anrufs: ignorieren
            if (diagnoseAn) diagnose += " [nach Ende ignoriert]"
        } else if (klingelt) {
            val kandidaten = listOf(personName, text, titel, unter).filter { it.isNotBlank() }
            var nummer = ""
            for (k in kandidaten) {
                val m = NUMMER.find(k)
                if (m != null) { nummer = m.value.trim(); break }
            }
            // Name: erster Text ohne Nummer, der nicht nur ein allgemeines Stichwort ist
            var name = ""
            for (k in kandidaten) {
                val kl = k.lowercase(Locale.GERMANY)
                if (NUMMER.containsMatchIn(k)) continue
                if (ALLGEMEIN.any { kl.contains(it) }) continue
                name = k.trim()
                break
            }
            klingelSchluessel = sbn.key
            status = "Anruf erkannt " + SimpleDateFormat("HH:mm:ss", Locale.GERMANY).format(Date())
            val dienst = AnrufDienst.instance
            if (dienst == null) status += " (Dienst nicht gestartet!)" else dienst.fritzAnruf(nummer, name)
        } else if (sbn.key == klingelSchluessel) {
            // Aus dem Klingeln wurde ein Gespräch (angenommen) oder die Meldung ist anders geworden
            klingelSchluessel = null
            AnrufDienst.instance?.fritzEnde()
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.key == klingelSchluessel) {
            klingelSchluessel = null
            AnrufDienst.instance?.fritzEnde()
        }
    }

    private fun merkePaket(paket: String) {
        if (!gesehen.contains(paket)) {
            gesehen.add(paket)
            if (gesehen.size > 12) gesehen.removeAt(0)
            apps = gesehen.joinToString(", ")
        }
    }
}
