/*
 * VideoActivity.kt
 * Scopo: lettore dell'AirPlay video (ExoPlayer di Media3). L'iPhone passa l'indirizzo e
 * fa da telecomando; qui si riproduce e si comunica lo stato al motore nativo, che lo
 * gira a iOS (GET /playback-info ed eventi sul canale inverso).
 *
 * Telecomando della TV: OK/play/pausa e frecce passano ai controlli di PlayerView;
 * INDIETRO ferma il video e avvisa l'iPhone. Se l'Activity va in background (tasto Home)
 * il video si ferma: senza schermo non avrebbe senso continuare a scaricarlo.
 *
 * Nel log non vanno indirizzi né titoli, solo codici d'errore.
 */
package com.irplay.tv.ui

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.TextView
import androidx.annotation.OptIn
import androidx.appcompat.app.AppCompatActivity
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DefaultHttpDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.ui.CaptionStyleCompat
import androidx.media3.ui.SubtitleView
import androidx.media3.ui.PlayerView
import com.irplay.tv.R
import com.irplay.tv.core.MediaCast
import com.irplay.tv.core.NativeBridge
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

@OptIn(UnstableApi::class)
class VideoActivity : AppCompatActivity(), MediaCast.Controller {

    private lateinit var player: ExoPlayer
    private lateinit var playerView: PlayerView
    private lateinit var message: TextView
    private val main = Handler(Looper.getMainLooper())
    // Le chiamate al nativo possono inviare un evento in rete (fino a 200 ms): fuori dal main thread.
    private val reporter: ExecutorService = Executors.newSingleThreadExecutor()

    private var loadedId = 0L
    private var pendingFraction = -1.0   // posizione iniziale come frazione, applicata a durata nota
    private var triedAsHls = false
    private var current: MediaCast.Request? = null
    private var stoppedByIphone = false

    private val tick = object : Runnable {
        override fun run() {
            report()
            main.postDelayed(this, REPORT_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_video)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        playerView = findViewById(R.id.player_view)
        message = findViewById(R.id.video_message)

        val http = DefaultHttpDataSource.Factory()
            .setConnectTimeoutMs(HTTP_TIMEOUT_MS)
            .setReadTimeoutMs(HTTP_TIMEOUT_MS)
            .setAllowCrossProtocolRedirects(false)   // un redirect https -> http non deve passare in silenzio
        player = ExoPlayer.Builder(this)
            .setMediaSourceFactory(DefaultMediaSourceFactory(CastDataSource.Factory(http)))
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MOVIE).build(),
                true,   // gestione del focus audio: si mette in pausa se un'altra app prende l'audio
            )
            .setHandleAudioBecomingNoisy(true)
            .build()
        player.addListener(listener)
        playerView.player = player
        playerView.controllerShowTimeoutMs = CONTROLLER_TIMEOUT_MS
        // Un solo video alla volta: "precedente" e "successivo" non avrebbero effetto.
        playerView.setShowPreviousButton(false)
        playerView.setShowNextButton(false)
        // Sottotitoli leggibili dal divano: bianco con bordo nero, un po' più grandi del normale.
        // Lo stile del video (colori, dimensioni) è ignorato, la posizione no.
        playerView.subtitleView?.apply {
            setStyle(CaptionStyleCompat(Color.WHITE, Color.TRANSPARENT, Color.TRANSPARENT,
                CaptionStyleCompat.EDGE_TYPE_OUTLINE, Color.BLACK, null))
            setFractionalTextSize(SubtitleView.DEFAULT_TEXT_SIZE_FRACTION * SUBTITLE_SCALE)
            setApplyEmbeddedStyles(false)
            setApplyEmbeddedFontSizes(false)
        }
        applyTracks(MediaCast.tracks)

        MediaCast.controller = this
        MediaCast.current?.let { load(it) } ?: finish()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        MediaCast.current?.let { load(it) }
    }

    override fun onStart() {
        super.onStart()
        isVisible = true
        main.post(tick)
    }

    override fun onStop() {
        super.onStop()
        isVisible = false
        main.removeCallbacks(tick)
        if (!isFinishing) {
            // Uscita con Home o un'altra app in primo piano: il video si ferma e l'iPhone lo sa.
            stopFromTv()
            finish()
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        if (MediaCast.controller === this) MediaCast.controller = null
        main.removeCallbacksAndMessages(null)
        player.removeListener(listener)
        player.release()
        reporter.shutdown()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            // Prima pressione con i controlli visibili: li nasconde, come ogni lettore su TV.
            if (playerView.isControllerFullyVisible) {
                playerView.hideController()
                return true
            }
            stopFromTv()
            finish()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    // ------------------------------------------------------------------ riproduzione

    private fun load(request: MediaCast.Request) {
        if (request.id == loadedId) return
        loadedId = request.id
        current = request
        triedAsHls = false
        message.visibility = View.GONE
        play(request, asHls = request.isHls)
    }

    private fun play(request: MediaCast.Request, asHls: Boolean) {
        val item = MediaItem.Builder().setUri(request.url).apply {
            if (asHls) setMimeType(MimeTypes.APPLICATION_M3U8)
        }.build()
        pendingFraction = request.startFraction
        if (request.startFraction < 0 && request.startMs > 0) player.setMediaItem(item, request.startMs)
        else player.setMediaItem(item)
        player.playWhenReady = true
        player.prepare()
        report()
    }

    private val listener = object : Player.Listener {
        override fun onPlaybackStateChanged(state: Int) {
            if (state == Player.STATE_READY && pendingFraction >= 0) {
                val duration = player.duration
                if (duration != C.TIME_UNSET && duration > 0) player.seekTo((duration * pendingFraction).toLong())
                pendingFraction = -1.0
            }
            if (state == Player.STATE_ENDED) {
                report()
                MediaCast.clear()
                finish()
                return
            }
            report()
        }

        override fun onIsPlayingChanged(isPlaying: Boolean) = report()

        override fun onPlayerError(error: PlaybackException) {
            val request = current
            // Indirizzo diretto senza estensione che in realtà è HLS: un secondo tentativo come HLS.
            if (request != null && !triedAsHls && !request.isHls &&
                error.errorCode == PlaybackException.ERROR_CODE_PARSING_CONTAINER_UNSUPPORTED
            ) {
                triedAsHls = true
                play(request, asHls = true)
                return
            }
            Log.w(TAG, "Riproduzione non riuscita: ${error.errorCodeName}")
            message.setText(
                // Codici 6xxx: contenuto protetto (FairPlay/Widevine), non riproducibile qui.
                if (error.errorCode in PlaybackException.ERROR_CODE_DRM_UNSPECIFIED until 7000) R.string.video_error_protected
                else R.string.video_error
            )
            message.visibility = View.VISIBLE
            stopFromTv()
            main.postDelayed({ finish() }, ERROR_CLOSE_MS)
        }
    }

    /** Stato attuale verso il nativo: posizione e durata in secondi. */
    private fun report() {
        val state = when {
            stoppedByIphone -> return
            player.playbackState == Player.STATE_ENDED -> STATE_ENDED
            player.playbackState == Player.STATE_READY && player.playWhenReady -> STATE_PLAYING
            player.playbackState == Player.STATE_READY -> STATE_PAUSED
            player.playbackState == Player.STATE_BUFFERING -> if (player.playWhenReady) STATE_LOADING else STATE_PAUSED
            else -> STATE_LOADING
        }
        val position = player.currentPosition.coerceAtLeast(0) / 1000.0
        val duration = player.duration.let { if (it == C.TIME_UNSET || it < 0) 0.0 else it / 1000.0 }
        val rate = if (state == STATE_PLAYING) player.playbackParameters.speed else 0f
        if (!reporter.isShutdown) reporter.execute { NativeBridge.updateMediaPlayback(position, duration, rate, state) }
    }

    private fun stopFromTv() {
        if (stoppedByIphone) return
        stoppedByIphone = true   // da qui in poi nessun aggiornamento di stato
        player.stop()
        MediaCast.clear()
        if (!reporter.isShutdown) reporter.execute { NativeBridge.stopMedia() }
    }

    // ------------------------------------------------------------------ comandi dall'iPhone

    override fun onMediaRequest(request: MediaCast.Request) {
        main.post { if (!isFinishing) load(request) }
    }

    override fun onMediaTracks(tracks: MediaCast.Tracks) {
        main.post { if (!isFinishing) applyTracks(tracks) }
    }

    /**
     * Le scelte dell'iPhone diventano preferenze del lettore: ExoPlayer sceglie fra le tracce
     * della playlist (YouTube vi mette audio e sottotitoli WebVTT di ogni lingua) quella che
     * corrisponde. Sottotitoli spenti = tipo di traccia disattivato.
     */
    private fun applyTracks(tracks: MediaCast.Tracks) {
        val b = player.trackSelectionParameters.buildUpon()
        if (tracks.audioLanguage.isNotEmpty()) b.setPreferredAudioLanguage(tracks.audioLanguage)
        b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, !tracks.textEnabled)
        if (tracks.textEnabled) {
            b.setPreferredTextLanguage(tracks.textLanguage.ifEmpty { null })
            b.setSelectUndeterminedTextLanguage(true)
        }
        player.trackSelectionParameters = b.build()
    }

    override fun onMediaRate(rate: Float) {
        main.post { player.playWhenReady = rate > 0f }
    }

    override fun onMediaScrub(positionMs: Long) {
        main.post {
            val duration = player.duration
            val target = if (duration != C.TIME_UNSET && duration > 0) positionMs.coerceIn(0, duration) else positionMs
            player.seekTo(target)
        }
    }

    override fun onMediaStop() {
        main.post {
            stoppedByIphone = true
            player.stop()
            finish()
        }
    }

    companion object {
        private const val TAG = "iRPlay_Video"
        private const val REPORT_INTERVAL_MS = 1000L
        private const val HTTP_TIMEOUT_MS = 15_000
        private const val CONTROLLER_TIMEOUT_MS = 3000
        private const val ERROR_CLOSE_MS = 4000L
        private const val SUBTITLE_SCALE = 1.2f

        // Valori di AirPlayServer::MediaState (C++).
        private const val STATE_LOADING = 1
        private const val STATE_PLAYING = 2
        private const val STATE_PAUSED = 3
        private const val STATE_ENDED = 4

        @Volatile var isVisible = false
            private set
    }
}
