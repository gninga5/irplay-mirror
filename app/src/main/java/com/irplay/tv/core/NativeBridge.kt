/*
 * NativeBridge.kt
 * Scopo: unico punto di accesso Kotlin <-> C++ (libirplay_native.so).
 * Le funzioni JNI sono raccolte qui perché i nomi dei simboli C++ dipendono dal
 * nome della classe: spostare i metodi in altre classi romperebbe il collegamento
 * con il codice nativo. Le regole R8 in keepRules/rules.keep impediscono che la
 * build release rinomini o rimuova i metodi chiamati dal C++.
 *
 * Le callback on* arrivano dai thread nativi (RTSP, video, audio), mai dal main thread:
 *  - non devono bloccare: il thread video ha il socket TCP di iOS da svuotare;
 *  - non devono richiamare il nativo in modo sincrono (stopServer prende un lock che il
 *    thread chiamante potrebbe già tenere durante la chiusura);
 *  - un'eccezione non deve risalire nel C++: viene registrata e ignorata.
 */
package com.irplay.tv.core

import android.util.Log

object NativeBridge {
    private const val TAG = "iRPlay_Bridge"

    init {
        System.loadLibrary("irplay_native")
    }

    /** Destinatario del video decifrato (CodecManager, legato alla SurfaceView dell'Activity). */
    interface VideoSink {
        fun onVideoStarted()
        fun onVideoStopped()
        /** SPS e PPS con start code Annex-B, da passare come csd-0 e csd-1. */
        /**
         * codec: CODEC_H264 (csd0 = SPS, csd1 = PPS) o CODEC_HEVC (csd0 = VPS+SPS+PPS, csd1
         * vuoto); parametri con start code Annex-B.
         */
        fun onVideoConfig(codec: Int, csd0: ByteArray, csd1: ByteArray)
        fun onVideoFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean)
    }

    /** Destinatario dell'audio decifrato (AudioRenderer, legato al servizio). */
    interface AudioSink {
        /** codecType: 1 = PCM 16 bit (ALAC già decodificato in C++), 4 = AAC-LC, 8 = AAC-ELD. */
        fun onAudioStarted(codecType: Int)
        fun onAudioStopped()
        /** dueMs: istante di uscita (SystemClock.uptimeMillis, stesso orologio del C++); 0 = subito. */
        fun onAudioFrame(data: ByteArray, dueMs: Long)
        fun onVolume(db: Float)
        /** Pausa o salto nel brano: l'audio in coda va scartato. */
        fun onAudioFlush()
    }

    /** Informazioni sul brano (solo audio AirPlay, non duplicazione). */
    interface NowPlayingListener {
        fun onMetadata(title: String, artist: String, album: String)
        /** JPEG o PNG; array vuoto = nessuna copertina. */
        fun onArtwork(image: ByteArray)
        /** atMs: istante (SystemClock.uptimeMillis) in cui la posizione si sente; 0 = adesso. */
        fun onProgress(positionMs: Long, durationMs: Long, atMs: Long)
    }

    /** Dati del telecomando DACP dell'iPhone (vedi DacpRemote). activeRemote è un token: mai nel log. */
    interface RemoteControlListener {
        fun onRemoteControl(dacpId: String, activeRemote: String, peerAddr: String)
    }

    /** Eventi del PIN (pair-pin-start / pair-setup-pin). */
    interface PinListener {
        fun onPinRequested(pin: String)
        fun onPinDismissed()
        fun onClientTrusted(publicKey: ByteArray)
    }

    /** Eventi di sessione per stato, notifica e UI. */
    interface SessionListener {
        fun onSessionStarted(clientName: String)
        fun onSessionEnded(reason: Int)
        fun onVideoStarted()
        fun onVideoStopped()
    }

    /** AirPlay video: l'iPhone passa un indirizzo e la TV lo riproduce (VideoActivity). */
    interface MediaListener {
        /** playlists: URI irplay-hls:// -> testo m3u8 (vuota per gli indirizzi http diretti). */
        fun onMediaPlay(url: String, startSec: Double, startFraction: Double, playlists: Map<String, ByteArray>)
        fun onMediaRate(rate: Float)
        fun onMediaScrub(positionSec: Double)
        fun onMediaStop()
        /** Tracce scelte sull'iPhone: lingua audio e sottotitoli ("" = nessuna preferenza). */
        fun onMediaTracks(audioLanguage: String, textLanguage: String, textEnabled: Boolean)
    }

    /** Formati video della duplicazione (kVideoH264 / kVideoHevc in session_sink.h). */
    const val CODEC_H264 = 0
    const val CODEC_HEVC = 1

    const val END_TEARDOWN = 0
    const val END_DISCONNECTED = 1
    const val END_WATCHDOG = 2
    const val END_LOCAL = 3

    @Volatile var videoSink: VideoSink? = null
    @Volatile var audioSink: AudioSink? = null
    @Volatile var sessionListener: SessionListener? = null
    @Volatile var pinListener: PinListener? = null
    @Volatile var nowPlayingListener: NowPlayingListener? = null
    @Volatile var mediaListener: MediaListener? = null
    @Volatile var remoteControlListener: RemoteControlListener? = null

    /**
     * Calcola la chiave pubblica Ed25519 (32 byte) da un seed di 32 byte.
     * Restituisce null se l'input non è valido. Il seed non viene conservato nel codice nativo.
     */
    @JvmStatic
    external fun ed25519PublicKey(seed: ByteArray): ByteArray?

    /**
     * Avvia il server RTSP. Restituisce la porta reale (che va annunciata via mDNS) oppure -1,
     * anche quando un parametro non è valido. txtAirPlay è il record TXT di _airplay._tcp
     * codificato (vedi AirPlayService.encodeTxt): GET /info lo restituisce a iOS.
     * seed: segreto Ed25519 (32 byte). Il nativo ne tiene solo la chiave espansa in memoria
     * protetta; il chiamante deve azzerare il proprio array subito dopo.
     */
    @JvmStatic
    external fun startServer(
        preferredPort: Int,
        name: ByteArray,   // UTF-8 standard (vedi jni_bridge.cpp)
        deviceId: String,
        pairingId: String,
        publicKey: ByteArray,
        features: Long,
        model: String,
        sourceVersion: String,
        txtAirPlay: ByteArray,
        seed: ByteArray,
        maxFps: Int,
        pinMode: Int,               // AppSettings.PIN_*
        trustedKeys: ByteArray,     // chiavi Ed25519 dei dispositivi associati, 32 byte l'una
        hevc: Boolean,              // HEVC per la duplicazione (VideoQuality)
        displayWidth: Int,          // schermo annunciato a iOS
        displayHeight: Int,
    ): Int

    /** Ferma il server e chiude tutte le connessioni. Idempotente. */
    @JvmStatic
    external fun stopServer()

    /** Chiude la sessione di mirroring in corso (tasto BACK). Non blocca. */
    @JvmStatic
    external fun stopSession()

    /**
     * Stato del lettore AirPlay video: posizione e durata in secondi, rate (0 = pausa),
     * state 1 caricamento, 2 riproduzione, 3 pausa, 4 finito. Può inviare un evento a iOS
     * (al massimo 200 ms): da chiamare fuori dal main thread.
     */
    @JvmStatic
    external fun updateMediaPlayback(positionSec: Double, durationSec: Double, rate: Float, state: Int)

    /** Video fermato dalla TV: avvisa l'iPhone. Stesso vincolo di thread di updateMediaPlayback. */
    @JvmStatic
    external fun stopMedia()

    // ------------------------------------------------------------ callback dal C++

    private inline fun guarded(what: String, block: () -> Unit) {
        try {
            block()
        } catch (e: Exception) {
            Log.w(TAG, "Errore nella callback $what: ${e.javaClass.simpleName}")
        }
    }

    @JvmStatic
    fun onSessionStarted(name: ByteArray) = guarded("sessione") {
        sessionListener?.onSessionStarted(String(name, Charsets.UTF_8))
    }

    @JvmStatic
    fun onSessionEnded(reason: Int) = guarded("fine sessione") {
        sessionListener?.onSessionEnded(reason)
    }

    @JvmStatic
    fun onVideoStarted() = guarded("video") {
        videoSink?.onVideoStarted()
        sessionListener?.onVideoStarted()
    }

    @JvmStatic
    fun onVideoStopped() = guarded("video") {
        videoSink?.onVideoStopped()
        sessionListener?.onVideoStopped()
    }

    @JvmStatic
    fun onVideoConfig(codec: Int, csd0: ByteArray, csd1: ByteArray) = guarded("formato video") {
        videoSink?.onVideoConfig(codec, csd0, csd1)
    }

    @JvmStatic
    fun onVideoFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) = guarded("frame video") {
        videoSink?.onVideoFrame(data, ptsUs, keyFrame)
    }

    @JvmStatic
    fun onAudioStarted(codecType: Int) = guarded("audio") { audioSink?.onAudioStarted(codecType) }

    @JvmStatic
    fun onAudioStopped() = guarded("audio") { audioSink?.onAudioStopped() }

    @JvmStatic
    fun onAudioFrame(data: ByteArray, dueMs: Long) = guarded("frame audio") { audioSink?.onAudioFrame(data, dueMs) }

    @JvmStatic
    fun onVolume(db: Float) = guarded("volume") { audioSink?.onVolume(db) }

    @JvmStatic
    fun onAudioFlush() = guarded("audio") { audioSink?.onAudioFlush() }

    @JvmStatic
    fun onMetadata(title: ByteArray, artist: ByteArray, album: ByteArray) = guarded("metadati") {
        nowPlayingListener?.onMetadata(
            String(title, Charsets.UTF_8), String(artist, Charsets.UTF_8), String(album, Charsets.UTF_8),
        )
    }

    @JvmStatic
    fun onArtwork(image: ByteArray) = guarded("copertina") { nowPlayingListener?.onArtwork(image) }

    @JvmStatic
    fun onProgress(positionMs: Long, durationMs: Long, atMs: Long) = guarded("avanzamento") {
        nowPlayingListener?.onProgress(positionMs, durationMs, atMs)
    }

    @JvmStatic
    fun onMediaPlay(url: ByteArray, startSec: Double, startFraction: Double, keys: Array<ByteArray>, bodies: Array<ByteArray>) =
        guarded("video AirPlay") {
            val n = minOf(keys.size, bodies.size)
            val playlists = HashMap<String, ByteArray>(n * 2)
            for (i in 0 until n) playlists[String(keys[i], Charsets.UTF_8)] = bodies[i]
            mediaListener?.onMediaPlay(String(url, Charsets.UTF_8), startSec, startFraction, playlists)
        }

    @JvmStatic
    fun onMediaRate(rate: Float) = guarded("video AirPlay") { mediaListener?.onMediaRate(rate) }

    @JvmStatic
    fun onMediaScrub(positionSec: Double) = guarded("video AirPlay") { mediaListener?.onMediaScrub(positionSec) }

    @JvmStatic
    fun onMediaStop() = guarded("video AirPlay") { mediaListener?.onMediaStop() }

    @JvmStatic
    fun onMediaTracks(audioLanguage: String, textLanguage: String, textEnabled: Boolean) = guarded("video AirPlay") {
        mediaListener?.onMediaTracks(audioLanguage, textLanguage, textEnabled)
    }

    @JvmStatic
    fun onRemoteControl(dacpId: String, activeRemote: String, peerAddr: String) = guarded("telecomando") {
        remoteControlListener?.onRemoteControl(dacpId, activeRemote, peerAddr)
    }

    @JvmStatic
    fun onPinRequested(pin: String) = guarded("PIN") { pinListener?.onPinRequested(pin) }

    @JvmStatic
    fun onPinDismissed() = guarded("PIN") { pinListener?.onPinDismissed() }

    @JvmStatic
    fun onClientTrusted(publicKey: ByteArray) = guarded("PIN") { pinListener?.onClientTrusted(publicKey) }
}
