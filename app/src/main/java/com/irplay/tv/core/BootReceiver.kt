/*
 * BootReceiver.kt
 * Scopo: avvia il ricevitore all'accensione della TV, così l'iPhone la vede senza che
 * l'utente apra prima l'app (comportamento atteso da un ricevitore come Chromecast).
 * BOOT_COMPLETED è tra le eccezioni che permettono di avviare un servizio in primo piano
 * dal background. Il receiver è esportato perché il sistema possa consegnare il broadcast;
 * l'azione è protetta, quindi nessun'altra app può inviarla: le altre vengono ignorate.
 */
package com.irplay.tv.core

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        try {
            AirPlayService.start(context)
        } catch (e: Exception) {
            // Alcuni produttori bloccano l'avvio automatico: l'utente aprirà l'app a mano.
            Log.w("iRPlay_Boot", "Avvio automatico non consentito: ${e.javaClass.simpleName}")
        }
    }
}
