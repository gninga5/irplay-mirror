/*
 * AirPlayService.kt
 * Scopo: tiene vivo il ricevitore in background su Google TV e annuncia la TV sulla LAN.
 *
 * Protocolli: DNS-SD su mDNS (RFC 6762 / RFC 6763) tramite NsdManager.
 *  - _airplay._tcp: servizio usato da iOS per il mirroring dello schermo.
 *  - _raop._tcp:    servizio audio (Remote Audio Output Protocol). Il nome deve essere
 *                   nel formato "MAC@Nome", altrimenti iOS lo scarta.
 *  Entrambi puntano alla stessa porta: un solo server RTSP (Fase 2) gestisce tutte e due.
 *
 * Perché NsdManager e non un risponditore mDNS scritto in C++:
 *  - su Google TV la porta 5353 è già usata dal demone di sistema (Cast);
 *  - nessun socket multicast aperto dall'app verso la LAN = meno superficie d'attacco.
 *
 * Su TV, la gestione del risparmio energetico (task killer Sony, Doze su Chromecast)
 * uccide i processi in background: il servizio in primo piano con notifica è l'unico
 * modo supportato per restare in ascolto.
 *
 * Il servizio possiede anche l'audio (AudioRenderer) e traduce gli eventi del motore
 * nativo in ReceiverState, notifica e apertura dell'Activity quando parte il mirroring.
 */
package com.irplay.tv.core

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaCodecList
import android.media.MediaFormat
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import com.irplay.tv.R
import com.irplay.tv.audio.AudioRenderer
import com.irplay.tv.ui.MainActivity
import com.irplay.tv.ui.VideoActivity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class AirPlayService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var multicastLock: WifiManager.MulticastLock? = null
    private var nsd: NsdManager? = null
    private val registrations = mutableListOf<NsdManager.RegistrationListener>()
    private var started = false
    private var audio: AudioRenderer? = null
    @Volatile private var pausedByTvAt = Long.MIN_VALUE / 2
    @Volatile private var lastSoundAt = 0L
    @Volatile private var pcmStream = false
    @Volatile private var resumeCheckPending = false
    private var identity: DeviceIdentity? = null
    private var deviceName = ""
    // Cambio di nome richiesto durante una sessione: applicato quando finisce, per non
    // interrompere la duplicazione in corso.
    @Volatile private var renamePending = false
    // Nome alternativo usato quando quello scelto è già annunciato da un altro dispositivo
    // (es. una Sony con AirPlay integrato che si chiama come la TV). Solo in memoria.
    private var nameOverride: String? = null
    private var overrideBase: String? = null   // nome scelto che aveva causato il conflitto
    private var lyricsJob: Job? = null

    /**
     * Se il testo non è stato cercato perché la TV era offline, lo si cerca appena torna una
     * connessione a Internet verificata, sempre che si stia ancora ascoltando un brano.
     */
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
            if (caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) &&
                ReceiverState.status.value is ReceiverState.Status.Playing &&
                ReceiverState.lyrics.value == null && lyricsJob?.isActive != true
            ) {
                scope.launch { loadLyrics() }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForeground va chiamato subito: Android termina il servizio se non arriva entro pochi secondi.
        startInForeground()
        if (intent?.action == ACTION_RELOAD && started) {
            if (ReceiverState.isInSession) renamePending = true else scope.launch { republish() }
            return START_STICKY
        }
        if (!started) {
            started = true
            acquireMulticastLock()
            scope.launch {
                val identity = try {
                    withContext(Dispatchers.IO) { DeviceIdentity.loadOrCreate(applicationContext) }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // Senza identità non si può annunciare nulla di valido: meglio fermarsi
                    // che pubblicare record TXT incompleti.
                    Log.e(TAG, "Identità non disponibile, servizio fermato")
                    ReceiverState.set(ReceiverState.Status.Failed)
                    stopSelf()
                    return@launch
                }

                audio = AudioRenderer(applicationContext)
                val dacp = DacpRemote(applicationContext)
                dacp.onPausedByTv = {
                    pausedByTvAt = ReceiverState.elapsedNow()
                    audio?.onAudioFlush()
                    markPaused()
                }
                dacp.onScanStarted = { audio?.setScanMuted(true) }
                dacp.onScanEnded = {
                    // L'audio accelerato già ricevuto (circa 1,8 s) non va suonato: si scarta e si
                    // riparte dal punto nuovo come dopo una pausa (ripresa al primo suono vero).
                    audio?.onAudioFlush()
                    markPaused()
                    audio?.setScanMuted(false)
                }
                remote = dacp
                NativeBridge.remoteControlListener = object : NativeBridge.RemoteControlListener {
                    override fun onRemoteControl(dacpId: String, activeRemote: String, peerAddr: String) =
                        dacp.attach(dacpId, activeRemote, peerAddr)
                }
                NativeBridge.audioSink = audioSink
                NativeBridge.nowPlayingListener = nowPlayingListener
                getSystemService(ConnectivityManager::class.java)?.registerDefaultNetworkCallback(networkCallback)
                NativeBridge.sessionListener = sessionListener
                NativeBridge.pinListener = pinListener
                NativeBridge.mediaListener = mediaListener
                this@AirPlayService.identity = identity
                publish()
            }
        }
        return START_STICKY
    }

    /** Avvia il server e annuncia la TV con il nome corrente. */
    private suspend fun publish() {
        val id = identity ?: return
        // L'alternativa resta valida finché l'utente non sceglie un altro nome: cambiare solo
        // il PIN non deve far ripartire il giro "conflitto -> ripubblica".
        val chosen = AppSettings.tvName(applicationContext)
        if (overrideBase != chosen) { nameOverride = null; overrideBase = null }
        deviceName = nameOverride ?: chosen
        ReceiverState.setPublishedName(deviceName)
        val rtspPort = withContext(Dispatchers.IO) { startNativeServer(id) }
        if (rtspPort <= 0) {
            Log.e(TAG, "Server RTSP non avviato, servizio fermato")
            ReceiverState.set(ReceiverState.Status.Failed)
            stopSelf()
            return
        }
        registerServices(id, rtspPort)
        ReceiverState.set(ReceiverState.Status.Ready)
    }

    /** Ritira l'annuncio mDNS e ferma il server (le connessioni aperte vengono chiuse). */
    private fun unpublish() {
        nsd?.let { n ->
            // unregisterService lancia IllegalArgumentException se la registrazione era già fallita.
            registrations.forEach { runCatching { n.unregisterService(it) } }
        }
        registrations.clear()
        NativeBridge.stopServer()
    }

    /**
     * Nuovo nome: va cambiato sia nell'annuncio mDNS sia in GET /info (iOS li confronta).
     * Il modo più semplice e sicuro è ripartire da zero: pochi decimi di secondo, fatti solo
     * quando nessuno sta trasmettendo.
     */
    private suspend fun republish() {
        renamePending = false
        Log.i(TAG, "Impostazioni cambiate (nome o PIN): ripubblico il servizio")
        withContext(Dispatchers.IO) { unpublish() }
        publish()
    }

    override fun onDestroy() {
        scope.cancel()
        nsd?.let { n ->
            // unregisterService lancia IllegalArgumentException se la registrazione era già fallita.
            registrations.forEach { runCatching { n.unregisterService(it) } }
        }
        registrations.clear()
        NativeBridge.sessionListener = null
        NativeBridge.pinListener = null
        NativeBridge.mediaListener = null
        MediaCast.clear()
        NativeBridge.nowPlayingListener = null
        runCatching { getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(networkCallback) }
        ReceiverState.setPin(null)
        ReceiverState.clearNowPlaying()
        NativeBridge.stopServer()     // prima si ritira l'annuncio, poi si chiudono le connessioni
        NativeBridge.audioSink = null
        NativeBridge.remoteControlListener = null
        audio?.release()
        audio = null
        remote?.release()
        remote = null
        ReceiverState.set(ReceiverState.Status.Starting)
        multicastLock?.takeIf { it.isHeld }?.release()
        multicastLock = null
        super.onDestroy()
    }

    /**
     * Il seed Ed25519 resta in chiaro nella memoria Java solo per la durata di questa chiamata:
     * il nativo ne deriva la chiave in memoria protetta, poi l'array viene azzerato.
     */
    private fun startNativeServer(identity: DeviceIdentity): Int {
        val seed = identity.loadSeed()
        try {
            return NativeBridge.startServer(
                PREFERRED_RTSP_PORT, deviceName.toByteArray(Charsets.UTF_8), identity.deviceId, identity.pairingId,
                identity.publicKey, FEATURES, MODEL, SRCVERS, encodeTxt(airplayTxt(identity)), seed,
                preferredFps(), AppSettings.pinMode(applicationContext), TrustedDevices.load(applicationContext),
            )
        } finally {
            seed.fill(0)
        }
    }

    /**
     * 60 fps se un decoder H.264 hardware dichiara 1080p a 60 fps, altrimenti 30.
     * A parità di movimento, 60 fps riducono il ritardo tra schermo dell'iPhone e TV
     * di circa 17 ms, ma raddoppiano il lavoro del decoder.
     */
    private fun preferredFps(): Int = try {
        val ok = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            !info.isEncoder && info.isHardwareAccelerated &&
                info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) } &&
                info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    .videoCapabilities?.areSizeAndRateSupported(1920, 1080, 60.0) == true
        }
        if (ok) 60 else 30
    } catch (e: Exception) {
        30
    }.also { Log.i(TAG, "Frame rate richiesto a iOS: $it fps") }

    /** Eventi dal motore nativo (thread nativi): aggiornano lo stato e non bloccano. */
    private val sessionListener = object : NativeBridge.SessionListener {
        @Volatile private var clientName = ""

        override fun onSessionStarted(clientName: String) {
            this.clientName = clientName
            ReceiverState.set(ReceiverState.Status.Connected(clientName))
            updateNotification(getString(R.string.receiver_connected))
        }

        override fun onVideoStarted() {
            ReceiverState.set(ReceiverState.Status.Mirroring(clientName))
            updateNotification(getString(R.string.receiver_mirroring))
            bringActivityToFront()
        }

        override fun onVideoStopped() {
            if (ReceiverState.status.value is ReceiverState.Status.Mirroring) {
                ReceiverState.set(ReceiverState.Status.Connected(clientName))
            }
        }

        override fun onSessionEnded(reason: Int) {
            Log.i(TAG, "Sessione terminata, motivo $reason")
            clientName = ""
            remote?.detach()
            ReceiverState.clearNowPlaying()
            ReceiverState.set(ReceiverState.Status.Ready)
            updateNotification(getString(R.string.receiver_waiting))
            if (renamePending) scope.launch { republish() }
        }
    }

    /**
     * Audio: inoltra all'AudioRenderer e aggiorna lo stato. Se arriva audio "musica" (non AAC-ELD)
     * senza video, la TV sta facendo da altoparlante: si mostra la schermata "In riproduzione".
     */
    private val audioSink = object : NativeBridge.AudioSink {
        override fun onAudioStarted(codecType: Int) {
            pcmStream = codecType == CT_PCM
            audio?.onAudioStarted(codecType)
            val status = ReceiverState.status.value
            if (codecType != CT_AAC_ELD && status is ReceiverState.Status.Connected) {
                ReceiverState.set(ReceiverState.Status.Playing(status.clientName))
                updateNotification(getString(R.string.receiver_playing))
                bringActivityToFront()
            }
        }

        /**
         * In pausa iOS chiude il solo flusso audio (TEARDOWN 96) dopo qualche secondo e lo riapre
         * alla ripresa, con la connessione sempre attiva. Si resta quindi sulla schermata del brano,
         * in pausa: la si lascia solo quando l'iPhone si scollega (onSessionEnded) o se inizia la
         * duplicazione dello schermo.
         */
        override fun onAudioStopped() {
            audio?.onAudioStopped()
            if (ReceiverState.status.value is ReceiverState.Status.Playing) markPaused()
        }

        override fun onAudioFrame(data: ByteArray, dueMs: Long) {
            audio?.onAudioFrame(data, dueMs)
            // Durante la pausa iOS continua a mandare pacchetti (silenzio) finché non chiude il
            // flusso, qualche secondo dopo: la ripresa la segnala solo audio non silenzioso.
            // Con AAC (non PCM) il contenuto non si vede qui: vale ogni pacchetto.
            val now = ReceiverState.elapsedNow()
            val sound = !pcmStream || !isSilent(data)
            if (sound) lastSoundAt = now
            // Si guarda all'istante in cui quell'audio esce dagli altoparlanti: se il suono
            // continua ad arrivare è una ripresa vera, e barra e testo ripartono da lì.
            if (ReceiverState.nowPlaying.value.paused && !resumeCheckPending &&
                sound && now - pausedByTvAt > RESUME_GUARD_MS
            ) {
                resumeCheckPending = true
                val lead = if (dueMs > 0) (dueMs - SystemClock.uptimeMillis()).coerceIn(0, MAX_AUDIO_LEAD_MS) else 0
                val startAt = now + lead
                scope.launch {
                    delay(lead.coerceAtLeast(RESUME_CHECK_MIN_MS))
                    resumeCheckPending = false
                    val sounding = ReceiverState.elapsedNow() - lastSoundAt < AUDIO_FLOW_MS
                    if (sounding) {
                        ReceiverState.updateNowPlaying { if (!it.paused) it else it.copy(progressAtElapsed = startAt, paused = false) }
                    }
                }
            }
        }

        override fun onVolume(db: Float) { audio?.onVolume(db) }
        override fun onAudioFlush() {
            audio?.onAudioFlush()
            markPaused()   // FLUSH = pausa o salto: se è un salto, il prossimo avanzamento lo riattiva
        }
    }

    /**
     * PCM 16 bit tutto sotto ~-60 dBFS: i pacchetti di riempimento che iOS manda in pausa.
     * Si guarda un campione ogni 8 (basta per il silenzio, costa poco: chiamato ~125 volte al secondo).
     */
    private fun isSilent(pcm: ByteArray): Boolean {
        var i = 0
        while (i + 1 < pcm.size) {
            val sample = (pcm[i].toInt() and 0xFF) or (pcm[i + 1].toInt() shl 8)
            if (sample > SILENCE_LEVEL || sample < -SILENCE_LEVEL) return false
            i += 16
        }
        return true
    }

    /** Ferma la barra di avanzamento sulla posizione raggiunta. */
    private fun markPaused() {
        val now = ReceiverState.elapsedNow()
        ReceiverState.updateNowPlaying {
            if (it.paused) it else it.copy(positionMs = it.currentPositionMs(now), progressAtElapsed = now, paused = true)
        }
    }

    /** Titolo, copertina e avanzamento del brano (thread nativi). */
    private val nowPlayingListener = object : NativeBridge.NowPlayingListener {
        override fun onMetadata(title: String, artist: String, album: String) {
            val before = ReceiverState.nowPlaying.value
            ReceiverState.updateNowPlaying {
                it.copy(title = cleanText(title), artist = cleanText(artist), album = cleanText(album))
            }
            val now = ReceiverState.nowPlaying.value
            if (now.title != before.title || now.artist != before.artist) scope.launch { loadLyrics() }
        }

        override fun onArtwork(image: ByteArray) {
            val bitmap = if (image.isEmpty()) null else decodeArtwork(image)
            ReceiverState.updateNowPlaying { it.copy(artwork = bitmap) }
        }

        override fun onProgress(positionMs: Long, durationMs: Long, atMs: Long) {
            // La posizione è dell'orologio di iOS; atMs dice quando quel punto esce dalla TV
            // (alla ripresa e al cambio brano, circa 1,8 s dopo): la barra parte da lì.
            // La pausa non si tocca qui: iOS manda l'avanzamento anche subito dopo averla chiesta;
            // la ripresa la segnala l'audio che torna.
            val now = ReceiverState.elapsedNow()
            val at = if (atMs > 0) now + (atMs - SystemClock.uptimeMillis()).coerceIn(-MAX_AUDIO_LEAD_MS, MAX_AUDIO_LEAD_MS) else now
            ReceiverState.updateNowPlaying {
                val frozen = it.currentPositionMs(now)
                // In pausa si tiene la posizione raggiunta, salvo un salto vero (altro brano, scorrimento).
                if (it.paused && kotlin.math.abs(positionMs - frozen) < PAUSE_JUMP_MS) {
                    it.copy(durationMs = durationMs)
                } else {
                    it.copy(positionMs = positionMs, durationMs = durationMs, progressAtElapsed = at)
                }
            }
        }
    }

    /**
     * Cerca il testo del brano appena cambiato. Si aspetta un attimo perché iOS manda durata e
     * avanzamento subito dopo i metadati, e la durata aiuta LRCLIB a scegliere la versione giusta.
     * Una nuova ricerca annulla quella del brano precedente.
     */
    private fun loadLyrics() {
        lyricsJob?.cancel()
        ReceiverState.setLyrics(null)
        if (!AppSettings.lyricsEnabled(applicationContext)) return
        if (!Lyrics.isOnline(applicationContext)) {   // TV senza Internet: nessuna richiesta
            Log.i(TAG, "TV senza Internet: testo non cercato (si riproverà quando torna la connessione)")
            return
        }
        lyricsJob = scope.launch {
            delay(LYRICS_DEBOUNCE_MS)
            val np = ReceiverState.nowPlaying.value
            val result = withContext(Dispatchers.IO) { Lyrics.fetch(applicationContext, np.title, np.artist, np.album, np.durationMs) }
            val current = ReceiverState.nowPlaying.value
            if (current.title == np.title && current.artist == np.artist) ReceiverState.setLyrics(result)
        }
    }

    /** Testo da mostrare a schermo: niente caratteri di controllo o invisibili, al massimo 200 caratteri. */
    private fun cleanText(s: String): String =
        s.filter { !Character.isISOControl(it) && Character.getType(it) != Character.FORMAT.toInt() }.take(200).trim()

    /**
     * La copertina arriva dalla rete: prima si leggono solo le dimensioni, poi si decodifica
     * ridotta a circa 600 px. Così un'immagine enorme (o costruita per esserlo una volta
     * decompressa) non può occupare centinaia di MB su una TV con 1-2 GB di RAM.
     */
    private fun decodeArtwork(data: ByteArray): Bitmap? = try {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 8192 || bounds.outHeight > 8192) {
            null
        } else {
            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= ARTWORK_MAX_PX) sample *= 2
            BitmapFactory.decodeByteArray(data, 0, data.size, BitmapFactory.Options().apply {
                inSampleSize = sample
                inPreferredConfig = Bitmap.Config.RGB_565
            })
        }
    } catch (e: Exception) {
        null
    } catch (e: OutOfMemoryError) {
        null
    }

    /**
     * Eventi del PIN (thread nativi). Il PIN va mostrato anche se l'apertura automatica è
     * disattivata: senza vederlo l'utente non può completare l'associazione. Compare sia
     * nell'app sia nella notifica, così è leggibile anche se Android blocca l'apertura.
     */
    /**
     * AirPlay video: la richiesta passa a MediaCast e si apre (o si aggiorna) VideoActivity.
     * L'apertura è sempre forzata, anche con l'apertura automatica spenta: è l'utente ad aver
     * scelto la TV dal pulsante AirPlay dell'app, e senza schermo il video non si vedrebbe.
     */
    private val mediaListener = object : NativeBridge.MediaListener {
        override fun onMediaPlay(url: String, startSec: Double, startFraction: Double, playlists: Map<String, ByteArray>) {
            val startMs = if (startSec.isFinite() && startSec > 0) (startSec * 1000).toLong() else 0L
            val request = MediaCast.newRequest(url, startMs, startFraction, playlists)
            updateNotification(getString(R.string.receiver_video))
            val controller = MediaCast.controller
            if (controller != null && VideoActivity.isVisible) {
                controller.onMediaRequest(request)
                return
            }
            try {
                startActivity(
                    Intent(this@AirPlayService, VideoActivity::class.java).addFlags(
                        Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
                    )
                )
            } catch (e: Exception) {
                Log.w(TAG, "Apertura del lettore non consentita: ${e.javaClass.simpleName}")
            }
        }

        override fun onMediaRate(rate: Float) { MediaCast.controller?.onMediaRate(rate) }

        override fun onMediaScrub(positionSec: Double) {
            if (positionSec.isFinite() && positionSec >= 0) MediaCast.controller?.onMediaScrub((positionSec * 1000).toLong())
        }

        override fun onMediaStop() {
            MediaCast.clear()
            MediaCast.controller?.onMediaStop()
            scope.launch {
                if (!ReceiverState.isInSession) updateNotification(getString(R.string.receiver_waiting))
            }
        }
    }

    private val pinListener = object : NativeBridge.PinListener {
        override fun onPinRequested(pin: String) {
            ReceiverState.setPin(pin)
            updateNotification(getString(R.string.pin_notification, pin))
            bringActivityToFront(force = true)
            // Il server invalida il PIN dopo 2 minuti senza avvisare: lo si toglie anche dallo
            // schermo, controllando che nel frattempo non ne sia arrivato uno nuovo.
            scope.launch {
                delay(PIN_SCREEN_TIMEOUT_MS)
                if (ReceiverState.pin.value == pin) onPinDismissed()
            }
        }

        override fun onPinDismissed() {
            ReceiverState.setPin(null)
            updateNotification(getString(R.string.receiver_waiting))
        }

        override fun onClientTrusted(publicKey: ByteArray) {
            TrustedDevices.add(applicationContext, publicKey)
        }
    }

    /**
     * Porta in primo piano lo schermo del video quando l'iPhone inizia a trasmettere, se
     * l'utente non ha disattivato "Apertura automatica" (AppSettings).
     * Da Android 10 l'avvio di un'Activity dal background è consentito solo se l'app ha il
     * permesso "mostra sopra altre app" (SYSTEM_ALERT_WINDOW) o è già visibile: senza, il
     * sistema ignora la richiesta e l'utente apre iRPlay dalla notifica o dal launcher.
     */
    private fun bringActivityToFront(force: Boolean = false) {
        if (MainActivity.isVisible || (!force && !AppSettings.autoOpen(this))) return
        try {
            startActivity(
                Intent(this, MainActivity::class.java).addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or
                        Intent.FLAG_ACTIVITY_SINGLE_TOP
                )
            )
        } catch (e: Exception) {
            Log.w(TAG, "Apertura automatica non consentita: ${e.javaClass.simpleName}")
        }
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun startInForeground() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                getString(R.string.receiver_channel_name),
                NotificationManager.IMPORTANCE_LOW    // nessun suono o pop-up sulla TV
            )
        )
        startForeground(
            NOTIFICATION_ID, buildNotification(getString(R.string.receiver_waiting)),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK,
        )
    }

    /**
     * Alcuni driver Wi-Fi delle TV (soprattutto Realtek e Amlogic) scartano i pacchetti
     * multicast in risparmio energetico: l'iPhone smetterebbe di "vedere" la TV dopo
     * qualche minuto. Il lock mantiene attiva la ricezione multicast mentre il servizio vive.
     * Non contato per riferimenti: un solo acquire, un solo release.
     */
    private fun acquireMulticastLock() {
        val wifi = applicationContext.getSystemService(WifiManager::class.java) ?: return
        multicastLock = wifi.createMulticastLock("iRPlay_MulticastLock").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun registerServices(id: DeviceIdentity, rtspPort: Int) {
        val manager = getSystemService(NsdManager::class.java) ?: return
        nsd = manager

        val airplay = NsdServiceInfo().apply {
            serviceName = deviceName
            serviceType = "_airplay._tcp"
            port = rtspPort
            airplayTxt(id).forEach { (k, v) -> setAttribute(k, v) }
        }

        val raop = NsdServiceInfo().apply {
            serviceName = "${id.raopPrefix}@$deviceName"
            serviceType = "_raop._tcp"
            port = rtspPort
            raopTxt(id).forEach { (k, v) -> setAttribute(k, v) }
        }

        register(manager, airplay)
        register(manager, raop)
    }

    private val pinRequired: Boolean
        get() = AppSettings.pinMode(applicationContext) != AppSettings.PIN_OFF

    /**
     * TXT di _airplay._tcp. Unica fonte sia per mDNS sia per GET /info: iOS confronta
     * le due copie e, se non coincidono, scarta il ricevitore senza dare errori.
     */
    private fun airplayTxt(id: DeviceIdentity): Map<String, String> = linkedMapOf(
        "deviceid" to id.deviceId,      // identità stabile del ricevitore
        "features" to FEATURES_HEX,     // bitmask delle funzioni supportate (vedi sotto)
        // Status flags: 0x4 = uscita audio collegata. Il bit 0x80 significa "password richiesta":
        // con 0x84 iOS 27 avvia pair-pin-start e la connessione fallisce.
        "flags" to "0x4",
        "pw" to pinRequired.toString(),   // "true": iOS chiede il PIN mostrato sulla TV
        "model" to MODEL,               // iOS sceglie la versione del protocollo in base al modello
        "pi" to id.pairingId,
        "pk" to id.publicKeyHex,        // chiave pubblica Ed25519 per pair-verify
        "srcvers" to SRCVERS,
        "vv" to "2",
    )

    private fun raopTxt(id: DeviceIdentity): Map<String, String> = linkedMapOf(
        "txtvers" to "1",
        "ch" to "2",            // canali audio
        "cn" to "0,1,2,3",      // codec: PCM, ALAC, AAC, AAC-ELD
        "da" to "true",
        "et" to "0,3,5",        // cifratura: nessuna, FairPlay, FairPlay SAPv2.5
        "vv" to "2",
        "ft" to FEATURES_HEX,
        "am" to MODEL,
        "md" to "0,1,2",        // metadati: testo, copertina, avanzamento
        "rhd" to "5.6.0.0",
        "pw" to pinRequired.toString(),
        "sf" to if (pinRequired) "0x8c" else "0x4",   // come UxPlay con il PIN a schermo
        "sr" to "44100",
        "ss" to "16",
        "sv" to "false",
        "tp" to "UDP",
        "vs" to SRCVERS,
        "vn" to "65537",
        "pk" to id.publicKeyHex,
    )

    private fun register(manager: NsdManager, info: NsdServiceInfo) {
        val type = info.serviceType
        val requested = info.serviceName
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(s: NsdServiceInfo) {
                Log.i(TAG, "Annunciato: ${s.serviceName} ($type)")
                // In caso di conflitto il sistema rinomina da solo ("TV della cucina (2)"), ma
                // GET /info continuerebbe a dire il nome originale e l'utente vedrebbe due voci
                // quasi uguali senza sapere quale è questa app. Si ripubblica una volta sola
                // con un nome che la distingue: "TV della cucina (iRPlay)".
                if (type == "_airplay._tcp" && s.serviceName != requested && nameOverride == null) {
                    val suffix = " (iRPlay)"
                    val base = requested.take(AppSettings.MAX_NAME_CHARS - suffix.length).trimEnd()
                    nameOverride = AppSettings.sanitizeName(base + suffix)
                    overrideBase = requested
                    Log.i(TAG, "Nome già usato da un altro dispositivo: ripubblico con un nome distinto")
                    scope.launch { republish() }
                } else if (type == "_airplay._tcp") {
                    ReceiverState.setPublishedName(s.serviceName)
                }
            }
            override fun onRegistrationFailed(s: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "Registrazione fallita $type, codice $errorCode")
            }
            override fun onServiceUnregistered(s: NsdServiceInfo) {
                Log.i(TAG, "Ritirato: ${s.serviceName} ($type)")
            }
            override fun onUnregistrationFailed(s: NsdServiceInfo, errorCode: Int) {
                Log.w(TAG, "Ritiro fallito $type, codice $errorCode")
            }
        }
        registrations += listener
        manager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    companion object {
        /** Telecomando verso l'iPhone durante l'audio AirPlay (null a servizio fermo). */
        @Volatile var remote: DacpRemote? = null
            private set

        /**
         * Codifica un record TXT come in DNS-SD (RFC 6763 §6.1): ogni voce è "chiave=valore",
         * preceduta da un byte con la sua lunghezza (massimo 255). Stesso formato che
         * NsdManager mette nel pacchetto mDNS, così il confronto fatto da iOS va a buon fine.
         */
        internal fun encodeTxt(txt: Map<String, String>): ByteArray {
            val out = ByteArrayOutputStream()
            for ((k, v) in txt) {
                val entry = "$k=$v".toByteArray(Charsets.US_ASCII)
                require(entry.size <= 255) { "voce TXT troppo lunga: $k" }
                out.write(entry.size)
                out.write(entry)
            }
            return out.toByteArray()
        }

        private const val TAG = "iRPlay_Service"
        private const val PAUSE_JUMP_MS = 3000L
        private const val RESUME_GUARD_MS = 500L
        private const val MAX_AUDIO_LEAD_MS = 5000L
        private const val RESUME_CHECK_MIN_MS = 300L
        private const val AUDIO_FLOW_MS = 300L
        private const val SILENCE_LEVEL = 32   // ~-60 dBFS su 16 bit
        private const val CT_PCM = 1
        private const val CHANNEL_ID = "irplay_receiver"
        private const val NOTIFICATION_ID = 1


        private const val PREFERRED_RTSP_PORT = 7000

        // Feature flags di un ricevitore per mirroring dello schermo (gli stessi di UxPlay).
        // Il bit 27 ("legacy pairing") è attivo: fa usare a iOS il pair-setup/verify
        // Ed25519 che implementeremo, invece del pairing HomeKit di AirPlay 2.
        // Bit 0 (video) e 4 (HLS) attivi: le app con il pulsante AirPlay (YouTube, Safari ...)
        // passano alla TV l'indirizzo del video invece di duplicare lo schermo.
        private const val FEATURES = 0x5A7FFEF7L
        // Forma dei TXT "bit 0-31,bit 32-63", come UxPlay: "0x5A7FFEF7,0x0".
        private val FEATURES_HEX = "0x%X,0x%X".format(FEATURES and 0xFFFFFFFFL, FEATURES ushr 32)
        private const val MODEL = "AppleTV3,2"
        private const val SRCVERS = "220.68"

        /** Va chiamato con l'app in primo piano: da Android 12 non si può avviare dal background. */
        fun start(context: Context) {
            context.startForegroundService(Intent(context, AirPlayService::class.java))
        }

        /** Applica il nome salvato in AppSettings (subito, o a fine sessione se ce n'è una). */
        fun reload(context: Context) {
            context.startForegroundService(Intent(context, AirPlayService::class.java).setAction(ACTION_RELOAD))
        }

        private const val ACTION_RELOAD = "com.irplay.tv.action.RELOAD"
        private const val PIN_SCREEN_TIMEOUT_MS = 125_000L   // poco oltre la validità nativa (120 s)
        private const val CT_AAC_ELD = 8
        private const val ARTWORK_MAX_PX = 600
        private const val LYRICS_DEBOUNCE_MS = 700L
    }
}