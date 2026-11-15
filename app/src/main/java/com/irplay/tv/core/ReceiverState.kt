/*
 * ReceiverState.kt
 * Scopo: stato del ricevitore condiviso tra servizio (che lo aggiorna dai thread nativi)
 * e Activity (che lo mostra). StateFlow è thread-safe e conserva l'ultimo valore, così
 * un'Activity aperta a sessione già iniziata mostra subito lo stato corretto.
 */
package com.irplay.tv.core

import android.graphics.Bitmap
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

object ReceiverState {

    sealed interface Status {
        data object Starting : Status
        data object Ready : Status
        /** clientName: nome del dispositivo iOS (dato personale: solo a schermo, mai nel log). */
        data class Connected(val clientName: String) : Status
        data class Mirroring(val clientName: String) : Status
        /** Solo audio (musica, podcast): la TV fa da altoparlante. */
        data class Playing(val clientName: String) : Status
        data object Failed : Status
    }

    private val mutable = MutableStateFlow<Status>(Status.Starting)
    private val pinMutable = MutableStateFlow<String?>(null)
    private val nameMutable = MutableStateFlow<String?>(null)
    private val nowPlayingMutable = MutableStateFlow(NowPlaying())

    /**
     * Brano in riproduzione. La posizione è quella comunicata da iOS all'istante
     * progressAtElapsed (SystemClock.elapsedRealtime): la UI la fa avanzare da sola.
     */
    data class NowPlaying(
        val title: String = "",
        val artist: String = "",
        val album: String = "",
        val artwork: Bitmap? = null,
        val positionMs: Long = 0,
        val durationMs: Long = 0,
        val progressAtElapsed: Long = 0,
    )

    val nowPlaying: StateFlow<NowPlaying> = nowPlayingMutable

    fun updateNowPlaying(transform: (NowPlaying) -> NowPlaying) {
        // update atomico: le callback arrivano da thread nativi diversi
        while (true) {
            val current = nowPlayingMutable.value
            if (nowPlayingMutable.compareAndSet(current, transform(current))) return
        }
    }

    fun clearNowPlaying() {
        nowPlayingMutable.value = NowPlaying()
    }

    fun elapsedNow(): Long = SystemClock.elapsedRealtime()

    /** Nome con cui la TV è davvero annunciata in rete (può differire da quello scelto, vedi AirPlayService). */
    val publishedName: StateFlow<String?> = nameMutable

    fun setPublishedName(value: String?) {
        nameMutable.value = value
    }

    /** PIN da mostrare a schermo mentre un dispositivo si associa; null se nessuno. */
    val pin: StateFlow<String?> = pinMutable

    fun setPin(value: String?) {
        pinMutable.value = value
    }
    val status: StateFlow<Status> = mutable

    fun set(value: Status) {
        mutable.value = value
    }

    val isInSession: Boolean
        get() = status.value.let { it is Status.Connected || it is Status.Mirroring || it is Status.Playing }
}
