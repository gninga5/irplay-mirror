/*
 * MediaCast.kt
 * Scopo: punto d'incontro tra il servizio (eventi AirPlay video dal motore nativo) e
 * VideoActivity (ExoPlayer). Il servizio scrive la richiesta corrente e inoltra i comandi
 * dell'iPhone; l'Activity la legge all'avvio e si registra come controller.
 *
 * Le playlist HLS arrivate dall'iPhone (YouTube) restano qui, in memoria, e il lettore le
 * legge con lo schema irplay-hls:// (CastDataSource): la TV non apre nessuna porta HTTP.
 */
package com.irplay.tv.core

import java.util.concurrent.atomic.AtomicLong

object MediaCast {

    /**
     * Un video chiesto dall'iPhone.
     * startFraction >= 0 (mittenti con il protocollo testuale) vale al posto di startMs:
     * va moltiplicata per la durata, nota solo quando il lettore ha caricato il video.
     */
    class Request(
        val id: Long,
        val url: String,
        val startMs: Long,
        val startFraction: Double,
        val playlists: Map<String, ByteArray>,
    ) {
        val isHls: Boolean
            get() = url.startsWith(LOCAL_SCHEME) || android.net.Uri.parse(url).path?.endsWith(".m3u8") == true
    }

    /**
     * Tracce scelte nell'app dell'iPhone (selectedMediaArray). Arriva subito dopo /play e a ogni
     * cambio: resta valida anche tra la rimozione e il reinserimento dello stesso video.
     */
    data class Tracks(val audioLanguage: String = "", val textLanguage: String = "", val textEnabled: Boolean = false)

    @Volatile var tracks = Tracks()

    /** Comandi dell'iPhone. Arrivano da thread nativi: chi li implementa passa al main thread. */
    interface Controller {
        fun onMediaTracks(tracks: Tracks)
        fun onMediaRequest(request: Request)
        fun onMediaRate(rate: Float)
        fun onMediaScrub(positionMs: Long)
        fun onMediaStop()
    }

    const val LOCAL_SCHEME = "irplay-hls://"

    private val ids = AtomicLong()

    @Volatile var current: Request? = null
        private set

    @Volatile var controller: Controller? = null

    fun newRequest(url: String, startMs: Long, startFraction: Double, playlists: Map<String, ByteArray>): Request =
        Request(ids.incrementAndGet(), url, startMs, startFraction, playlists).also { current = it }

    fun clear() {
        current = null
    }

    /** Playlist servita dalla memoria; null se l'URI non appartiene alla richiesta corrente. */
    fun playlist(uri: String): ByteArray? = current?.playlists?.get(uri)
}
