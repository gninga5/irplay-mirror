/*
 * AppSettings.kt
 * Scopo: preferenze dell'utente, condivise tra Activity (che le modifica) e servizio
 * (che le legge quando parte una sessione). SharedPreferences basta: sono pochi valori non
 * sensibili, letti raramente; nessun backup cloud (allowBackup=false nel manifest).
 */
package com.irplay.tv.core

import android.content.Context
import android.os.Build
import android.provider.Settings

object AppSettings {
    private const val FILE = "irplay_settings"
    private const val KEY_AUTO_OPEN = "auto_open"
    private const val KEY_TV_NAME = "tv_name"
    private const val KEY_PIN_MODE = "pin_mode"
    private const val KEY_LYRICS = "lyrics"

    /** Valori uguali a irplay::PinMode nel codice nativo. */
    const val PIN_OFF = 0
    const val PIN_FIRST_TIME = 1
    const val PIN_ALWAYS = 2

    /** Lunghezza massima scelta per l'interfaccia; il limite tecnico mDNS è 63 byte UTF-8. */
    const val MAX_NAME_CHARS = 40
    private const val MAX_NAME_BYTES = 63
    private const val FALLBACK_NAME = "iRPlay Mirror"

    private fun prefs(context: Context) = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    /** Apertura automatica dell'app quando un iPhone inizia la duplicazione. Attiva di default. */
    fun autoOpen(context: Context): Boolean = prefs(context).getBoolean(KEY_AUTO_OPEN, true)

    fun setAutoOpen(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_AUTO_OPEN, enabled).apply()
    }

    /**
     * Nome con cui la TV compare in "Duplica schermo". Se l'utente non ne ha scelto uno si usa
     * il nome che la TV ha già nelle impostazioni di Android (es. "BRAVIA 4K VH2" o quello dato
     * in fase di configurazione), così chi ha più TV le distingue anche senza fare nulla.
     */
    fun tvName(context: Context): String =
        prefs(context).getString(KEY_TV_NAME, null)?.let(::sanitizeName)?.takeIf { it.isNotEmpty() }
            ?: systemName(context)

    fun hasCustomName(context: Context): Boolean = prefs(context).contains(KEY_TV_NAME)

    /**
     * Testi delle canzoni (vedi Lyrics.kt). Attivati di default; è l'unica funzione che manda
     * dati a servizi esterni (titolo, artista, album e durata del brano), come indicato nella
     * schermata Licenze, e si può disattivare dalla home.
     */
    fun lyricsEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_LYRICS, true)

    fun setLyricsEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_LYRICS, enabled).apply()
    }

    /**
     * Richiesta del PIN. Di default "solo la prima volta": ogni nuovo dispositivo deve dimostrare
     * di vedere lo schermo della TV, poi viene ricordato e non lo chiede più.
     */
    fun pinMode(context: Context): Int =
        prefs(context).getInt(KEY_PIN_MODE, PIN_FIRST_TIME).takeIf { it in PIN_OFF..PIN_ALWAYS } ?: PIN_FIRST_TIME

    fun setPinMode(context: Context, mode: Int) {
        if (mode !in PIN_OFF..PIN_ALWAYS) return
        prefs(context).edit().putInt(KEY_PIN_MODE, mode).apply()
    }

    /** Salva il nome scelto (null = torna a quello di sistema). Restituisce false se il nome non è valido. */
    fun setTvName(context: Context, name: String?): Boolean {
        if (name == null) {
            prefs(context).edit().remove(KEY_TV_NAME).apply()
            return true
        }
        val clean = sanitizeName(name)
        if (clean.isEmpty()) return false
        prefs(context).edit().putString(KEY_TV_NAME, clean).apply()
        return true
    }

    private fun systemName(context: Context): String {
        val fromSettings = try {
            Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
        } catch (e: SecurityException) {
            null   // alcuni produttori limitano la lettura: si ripiega sul modello
        }
        return listOf(fromSettings, Build.MODEL)
            .mapNotNull { it?.let(::sanitizeName) }
            .firstOrNull { it.isNotEmpty() } ?: FALLBACK_NAME
    }

    /**
     * Rende un nome sicuro da annunciare in rete:
     *  - via caratteri di controllo e caratteri di formato invisibili (categoria Cf), tra cui
     *    quelli che invertono il verso del testo: servirebbero solo a mostrare su iOS un nome
     *    diverso da quello reale;
     *  - spazi multipli ridotti a uno, niente spazi agli estremi;
     *  - al massimo MAX_NAME_CHARS caratteri e 63 byte UTF-8 (un'etichetta DNS), tagliando
     *    sempre tra un carattere e l'altro, mai a metà di un'emoji.
     * Il codice nativo ripete la validazione: questa serve a dare all'utente un nome già corretto.
     */
    fun sanitizeName(raw: String): String {
        val sb = StringBuilder()
        var count = 0
        var bytes = 0
        var lastSpace = true   // evita spazi iniziali
        var i = 0
        while (i < raw.length && count < MAX_NAME_CHARS) {
            val cp = raw.codePointAt(i)
            i += Character.charCount(cp)
            val type = Character.getType(cp)
            if (Character.isISOControl(cp) || type == Character.FORMAT.toInt() ||
                type == Character.SURROGATE.toInt() || type == Character.UNASSIGNED.toInt()
            ) continue
            val space = Character.isWhitespace(cp) || Character.isSpaceChar(cp)
            if (space && lastSpace) continue
            val toAppend = if (space) " " else String(Character.toChars(cp))
            val len = toAppend.toByteArray(Charsets.UTF_8).size
            if (bytes + len > MAX_NAME_BYTES) break
            sb.append(toAppend)
            bytes += len
            count++
            lastSpace = space
        }
        return sb.toString().trimEnd()
    }
}
