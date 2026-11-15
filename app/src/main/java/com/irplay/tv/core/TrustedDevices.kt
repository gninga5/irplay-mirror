/*
 * TrustedDevices.kt
 * Scopo: elenco dei dispositivi associati con il PIN (modalità "solo la prima volta").
 * Si salvano solo le chiavi pubbliche Ed25519 dei client (32 byte): non sono segreti, ma da
 * loro dipende chi può trasmettere senza PIN, quindi il file sta nello spazio privato dell'app
 * (noBackupFilesDir: escluso da backup e trasferimenti) ed è scritto in modo atomico.
 * Formato: chiavi concatenate, massimo MAX_DEVICES (lo stesso limite del codice nativo).
 */
package com.irplay.tv.core

import android.content.Context
import android.util.AtomicFile
import java.io.File

object TrustedDevices {
    private const val FILE_NAME = "trusted_devices.bin"
    private const val KEY_SIZE = 32
    private const val MAX_DEVICES = 64
    private val lock = Any()

    private fun file(context: Context) = AtomicFile(File(context.noBackupFilesDir, FILE_NAME))

    /** Tutte le chiavi concatenate, nel formato che si aspetta NativeBridge.startServer. */
    fun load(context: Context): ByteArray = synchronized(lock) {
        val data = try {
            file(context).readFully()
        } catch (e: Exception) {
            return ByteArray(0)   // file assente o illeggibile: nessun dispositivo associato
        }
        // Un file troncato o alterato non deve impedire l'avvio: si tengono le chiavi intere.
        val count = minOf(data.size / KEY_SIZE, MAX_DEVICES)
        data.copyOf(count * KEY_SIZE)
    }

    fun count(context: Context): Int = load(context).size / KEY_SIZE

    fun add(context: Context, key: ByteArray) = synchronized(lock) {
        if (key.size != KEY_SIZE) return
        val current = load(context)
        for (i in 0 until current.size / KEY_SIZE) {
            if (current.copyOfRange(i * KEY_SIZE, (i + 1) * KEY_SIZE).contentEquals(key)) return
        }
        // Oltre il limite si scarta il più vecchio, come fa il server nativo.
        val kept = if (current.size / KEY_SIZE >= MAX_DEVICES) current.copyOfRange(KEY_SIZE, current.size) else current
        write(context, kept + key)
    }

    fun clear(context: Context) = synchronized(lock) { file(context).delete() }

    private fun write(context: Context, data: ByteArray) {
        val f = file(context)
        val out = f.startWrite()
        try {
            out.write(data)
            f.finishWrite(out)
        } catch (e: Exception) {
            f.failWrite(out)
        }
    }
}
