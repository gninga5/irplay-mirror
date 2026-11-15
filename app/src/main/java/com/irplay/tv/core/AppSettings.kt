/*
 * AppSettings.kt
 * Scopo: preferenze dell'utente, condivise tra Activity (che le modifica) e servizio
 * (che le legge quando parte una sessione). SharedPreferences basta: sono pochi valori non
 * sensibili, letti raramente; nessun backup cloud (allowBackup=false nel manifest).
 */
package com.irplay.tv.core

import android.content.Context

object AppSettings {
    private const val FILE = "irplay_settings"
    private const val KEY_AUTO_OPEN = "auto_open"

    /** Apertura automatica dell'app quando un iPhone inizia la duplicazione. Attiva di default. */
    fun autoOpen(context: Context): Boolean =
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).getBoolean(KEY_AUTO_OPEN, true)

    fun setAutoOpen(context: Context, enabled: Boolean) {
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit().putBoolean(KEY_AUTO_OPEN, enabled).apply()
    }
}
