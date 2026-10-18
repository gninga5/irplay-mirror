/*
 * NativeBridge.kt
 * Scopo: unico punto di accesso Kotlin -> C++ (libicast_native.so).
 * Le funzioni JNI sono raccolte qui perché i nomi dei simboli C++ dipendono dal
 * nome della classe: spostare i metodi in altre classi romperebbe il collegamento
 * con il codice nativo.
 */
package com.icast.tv.core

object NativeBridge {
    init {
        System.loadLibrary("icast_native")
    }

    /**
     * Calcola la chiave pubblica Ed25519 (32 byte) da un seed di 32 byte.
     * Restituisce null se l'input non è valido. Il seed non viene conservato nel codice nativo.
     */
    @JvmStatic
    external fun ed25519PublicKey(seed: ByteArray): ByteArray?

    /** Avvia il server RTSP. Restituisce la porta reale (che va annunciata via mDNS) oppure -1. */
    @JvmStatic
    external fun startServer(preferredPort: Int): Int

    /** Ferma il server e chiude tutte le connessioni. Idempotente. */
    @JvmStatic
    external fun stopServer()
}