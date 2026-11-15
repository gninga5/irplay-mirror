/*
 * ReceiverState.kt
 * Scopo: stato del ricevitore condiviso tra servizio (che lo aggiorna dai thread nativi)
 * e Activity (che lo mostra). StateFlow è thread-safe e conserva l'ultimo valore, così
 * un'Activity aperta a sessione già iniziata mostra subito lo stato corretto.
 */
package com.irplay.tv.core

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

object ReceiverState {

    sealed interface Status {
        data object Starting : Status
        data object Ready : Status
        /** clientName: nome del dispositivo iOS (dato personale: solo a schermo, mai nel log). */
        data class Connected(val clientName: String) : Status
        data class Mirroring(val clientName: String) : Status
        data object Failed : Status
    }

    private val mutable = MutableStateFlow<Status>(Status.Starting)
    private val pinMutable = MutableStateFlow<String?>(null)

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
        get() = status.value.let { it is Status.Connected || it is Status.Mirroring }
}
