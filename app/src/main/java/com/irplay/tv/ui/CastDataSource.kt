/*
 * CastDataSource.kt
 * Scopo: unica sorgente di dati del lettore AirPlay video.
 *  - irplay-hls://...  playlist arrivate dall'iPhone, lette dalla memoria (MediaCast);
 *  - http:// https://  segmenti e video, tramite lo stack HTTP di Media3;
 *  - qualsiasi altro schema è rifiutato.
 *
 * Perché non DefaultDataSource: aprirebbe anche file://, content:// e asset dell'app. Un
 * dispositivo sulla rete che manda un "video" con uno di quegli indirizzi (o una playlist
 * che li contiene) potrebbe far leggere al lettore file privati della TV.
 */
package com.irplay.tv.ui

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.ByteArrayDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSourceException
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.TransferListener
import androidx.media3.common.PlaybackException
import com.irplay.tv.core.MediaCast

@UnstableApi
class CastDataSource(private val http: DataSource) : DataSource {

    class Factory(private val http: DataSource.Factory) : DataSource.Factory {
        override fun createDataSource(): DataSource = CastDataSource(http.createDataSource())
    }

    private var active: DataSource? = null

    override fun addTransferListener(transferListener: TransferListener) {
        http.addTransferListener(transferListener)
    }

    override fun open(dataSpec: DataSpec): Long {
        val source = when (dataSpec.uri.scheme?.lowercase()) {
            LOCAL -> {
                val bytes = MediaCast.playlist(dataSpec.uri.toString())
                    ?: throw DataSourceException(PlaybackException.ERROR_CODE_IO_FILE_NOT_FOUND)
                ByteArrayDataSource(bytes)
            }
            "http", "https" -> http
            // Schema non ammesso: errore generico, senza l'indirizzo (che finirebbe nel log).
            else -> throw DataSourceException(PlaybackException.ERROR_CODE_IO_NO_PERMISSION)
        }
        active = source
        return source.open(dataSpec)
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int =
        active?.read(buffer, offset, length) ?: C.RESULT_END_OF_INPUT

    override fun getUri(): Uri? = active?.uri

    override fun getResponseHeaders(): Map<String, List<String>> = active?.responseHeaders ?: emptyMap()

    override fun close() {
        try {
            active?.close()
        } finally {
            active = null
        }
    }

    private companion object {
        const val LOCAL = "irplay-hls"
    }
}
