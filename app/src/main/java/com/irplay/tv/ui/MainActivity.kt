/*
 * MainActivity.kt
 * Scopo: schermo del ricevitore. In attesa mostra nome, indirizzo IP e istruzioni;
 * durante il mirroring mostra il video a tutto schermo sulla SurfaceView.
 *
 * Telecomando (D-pad):
 *  - BACK durante una sessione: chiude la connessione con l'iPhone (in AirPlay il ricevitore
 *    non può inviare un TEARDOWN: è sempre il client a mandarlo; chiudere il socket fa sì
 *    che iOS interrompa la duplicazione), pulisce la superficie e torna alla schermata di attesa.
 *  - BACK in attesa: esce dall'app; il servizio resta attivo e la TV resta visibile su iOS.
 *
 * FLAG_KEEP_SCREEN_ON è attivo solo durante la sessione: impedisce che l'Ambient Mode di
 * Google TV (salvaschermo) copra il video, ma non tiene accesa la TV quando non serve
 * (rischio di burn-in sui pannelli OLED).
 */
package com.irplay.tv.ui

import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.res.ColorStateList
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.text.InputFilter
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.irplay.tv.R
import com.irplay.tv.core.AirPlayService
import com.irplay.tv.core.AppSettings
import com.irplay.tv.core.Lyrics
import com.irplay.tv.core.CodecManager
import com.irplay.tv.core.DacpRemote
import com.irplay.tv.core.NativeBridge
import com.irplay.tv.core.ReceiverState
import com.irplay.tv.core.TrustedDevices
import com.irplay.tv.core.VideoQuality
import com.irplay.tv.databinding.ActivityMainBinding
import java.net.Inet4Address
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var codec: CodecManager
    private val scope = MainScope()
    private var videoWidth = 0
    private var videoHeight = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        codec = CodecManager(::onVideoSize)
        NativeBridge.videoSink = codec
        binding.video.holder.addCallback(surfaceCallback)
        binding.root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> fitVideo() }

        binding.nameRow.setOnClickListener { showRenameDialog() }
        binding.pinRow.setOnClickListener { showPinDialog() }
        binding.lyricsRow.setOnClickListener {
            val enable = !AppSettings.lyricsEnabled(this)
            AppSettings.setLyricsEnabled(this, enable)
            // Chi la attiva deve sapere che titolo e artista escono dalla TV: lo si dice subito.
            if (enable) Toast.makeText(this, R.string.lyrics_enabled_toast, Toast.LENGTH_LONG).show()
            renderLyricsSetting()
        }
        binding.licensesButton.setOnClickListener { startActivity(Intent(this, LicensesActivity::class.java)) }
        binding.autoOpenRow.setOnClickListener { onAutoOpenPressed() }
        binding.videoQualityRow.setOnClickListener { showVideoQualityDialog() }
        binding.npPrev.setOnClickListener { sendRemote(DacpRemote.Command.PREVIOUS) }
        binding.npPlayPause.setOnClickListener { sendRemote(DacpRemote.Command.PLAY_PAUSE) }
        binding.npNext.setOnClickListener { sendRemote(DacpRemote.Command.NEXT) }
        binding.npSeek.setOnKeyListener { _, keyCode, event -> onSeekKey(keyCode, event) }

        AirPlayService.start(this)
        scope.launch { ReceiverState.status.collect { render(it) } }
        scope.launch { ReceiverState.publishedName.collect { renderName() } }
        scope.launch { ReceiverState.nowPlaying.collect { renderNowPlaying(it) } }
        scope.launch { ReceiverState.lyrics.collect { renderLyrics() } }
        // La posizione del brano arriva da iOS ogni tanto: in mezzo la si fa avanzare qui.
        // Solo con la schermata visibile: con un'altra app in primo piano non serve ridisegnare.
        scope.launch {
            while (true) {
                if (isVisible && ReceiverState.status.value is ReceiverState.Status.Playing) {
                    renderProgress(ReceiverState.nowPlaying.value)
                    renderLyrics()
                }
                delay(200)   // il testo cambia riga entro 0,2 s dal momento giusto
            }
        }
        scope.launch {
            ReceiverState.pin.collect { pin ->
                // Il PIN si mostra solo a schermo; nessun log, nessuna copia oltre lo stato.
                binding.pinOverlay.visibility = if (pin == null) View.GONE else View.VISIBLE
                binding.pinDigits.text = pin ?: ""
                if (pin != null) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        isVisible = true
        val net = localIpv4()
        binding.ipAddress.text = net?.let { (ip, ethernet) ->
            getString(if (ethernet) R.string.ip_ethernet else R.string.ip_wifi, ip)
        } ?: getString(R.string.receiver_no_network)
        // L'icona segue il tipo di collegamento: cavo o Wi-Fi.
        binding.ipIcon.setImageResource(if (net == null || net.second) R.drawable.ic_ethernet else R.drawable.ic_wifi)
        renderAutoOpen()   // anche al ritorno dalla schermata di sistema dei permessi
        renderName()
        renderPin()
        renderLyricsSetting()
        renderVideoQuality()
    }

    override fun onStop() {
        isVisible = false
        super.onStop()
    }

    override fun onDestroy() {
        scope.cancel()
        if (NativeBridge.videoSink === codec) NativeBridge.videoSink = null
        codec.release()
        super.onDestroy()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && ReceiverState.isInSession) {
            NativeBridge.stopSession()
            return true
        }
        if (ReceiverState.status.value is ReceiverState.Status.Playing) {
            // Tasti multimediali del telecomando (dove ci sono): stessi comandi dei pulsanti a video.
            // Frecce e OK restano alla navigazione tra i pulsanti.
            val command = when (keyCode) {
                KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PLAY,
                KeyEvent.KEYCODE_MEDIA_PAUSE -> DacpRemote.Command.PLAY_PAUSE
                KeyEvent.KEYCODE_MEDIA_NEXT -> DacpRemote.Command.NEXT
                KeyEvent.KEYCODE_MEDIA_PREVIOUS -> DacpRemote.Command.PREVIOUS
                else -> null
            }
            if (command != null) {
                if (event.repeatCount == 0) sendRemote(command)   // tasto tenuto premuto: un solo comando
                return true
            }
        }
        return super.onKeyDown(keyCode, event)
    }

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) = codec.attachSurface(holder.surface)
        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
        override fun surfaceDestroyed(holder: SurfaceHolder) = codec.detachSurface()
    }

    private fun render(status: ReceiverState.Status) {
        val mirroring = status is ReceiverState.Status.Mirroring
        val playing = status is ReceiverState.Status.Playing
        binding.idlePanel.visibility = if (mirroring || playing) View.GONE else View.VISIBLE
        // Solo musica: niente FLAG_KEEP_SCREEN_ON (sotto); la TV può andare in salvaschermo
        // mentre la musica continua, ed evita di lasciare la copertina fissa su un OLED.
        val wasPlaying = binding.nowPlayingPanel.visibility == View.VISIBLE
        binding.nowPlayingPanel.visibility = if (playing) View.VISIBLE else View.GONE
        // All'apertura della schermata del brano il fuoco va su play/pausa: OK funziona subito.
        if (playing && !wasPlaying) binding.npPlayPause.requestFocus()
        if (playing) binding.npSource.text = getString(R.string.np_source, displayName((status as ReceiverState.Status.Playing).clientName))
        if (status is ReceiverState.Status.Connected || mirroring) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        if (!mirroring) {
            videoWidth = 0
            videoHeight = 0
        }
        binding.status.text = when (status) {
            ReceiverState.Status.Starting -> getString(R.string.status_starting)
            ReceiverState.Status.Ready -> getString(R.string.status_ready)
            is ReceiverState.Status.Connected -> getString(R.string.status_connected, displayName(status.clientName))
            is ReceiverState.Status.Mirroring -> getString(R.string.status_mirroring, displayName(status.clientName))
            is ReceiverState.Status.Playing -> getString(R.string.status_playing, displayName(status.clientName))
            ReceiverState.Status.Failed -> getString(R.string.status_failed)
        }
        val (short, color) = when (status) {
            ReceiverState.Status.Starting -> R.string.status_short_starting to R.color.status_starting
            ReceiverState.Status.Ready -> R.string.status_short_ready to R.color.status_ready
            is ReceiverState.Status.Connected -> R.string.status_short_connected to R.color.status_active
            is ReceiverState.Status.Mirroring -> R.string.status_short_mirroring to R.color.status_active
            is ReceiverState.Status.Playing -> R.string.status_short_playing to R.color.status_active
            ReceiverState.Status.Failed -> R.string.status_short_failed to R.color.status_error
        }
        binding.statusShort.setText(short)
        binding.statusDot.backgroundTintList = ColorStateList.valueOf(getColor(color))
    }

    /**
     * Riga "Apertura automatica": OK sul telecomando la attiva o disattiva. Se è attiva ma
     * manca il permesso "mostra sopra altre app", OK apre la schermata di sistema per concederlo:
     * senza permesso Android blocca l'apertura dal background e l'impostazione non avrebbe effetto.
     */
    private fun onAutoOpenPressed() {
        val enabled = AppSettings.autoOpen(this)
        if (enabled && !Settings.canDrawOverlays(this)) {
            requestOverlayPermission()
            return
        }
        AppSettings.setAutoOpen(this, !enabled)
        if (!enabled && !Settings.canDrawOverlays(this)) requestOverlayPermission()
        renderAutoOpen()
    }

    private fun renderName() {
        val name = ReceiverState.publishedName.value ?: AppSettings.tvName(this)
        binding.receiverName.text = name
        binding.step3.text = getString(R.string.step_3, name)
    }

    /**
     * Finestra per cambiare il nome. Il testo viene ripulito da AppSettings.sanitizeName (stessa
     * regola del codice nativo) e il servizio ripubblica l'annuncio: subito se nessuno sta
     * trasmettendo, altrimenti a fine sessione, per non interrompere la duplicazione.
     */
    private fun showRenameDialog() {
        val view = layoutInflater.inflate(R.layout.dialog_rename, null)
        val input = view.findViewById<EditText>(R.id.name_input).apply {
            filters = arrayOf(InputFilter.LengthFilter(AppSettings.MAX_NAME_CHARS))
            setText(AppSettings.tvName(this@MainActivity))
            setSelection(text.length)
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.rename_title)
            .setView(view)
            .setPositiveButton(R.string.rename_save, null)   // gestito sotto: con nome non valido resta aperta
            .setNegativeButton(R.string.rename_cancel, null)
            .setNeutralButton(R.string.rename_reset) { _, _ -> applyName(null) }
            .create()
        dialog.setOnShowListener {
            val save = dialog.getButton(AlertDialog.BUTTON_POSITIVE)
            val submit = {
                if (applyName(input.text.toString())) dialog.dismiss()
                else input.error = getString(R.string.rename_invalid)
            }
            save.setOnClickListener { submit() }
            input.setOnEditorActionListener { _, action, _ ->
                if (action == EditorInfo.IME_ACTION_DONE) { submit(); true } else false
            }
            input.requestFocus()
        }
        dialog.show()
    }

    /** Salva il nome (null = quello di sistema) e chiede al servizio di applicarlo. */
    private fun applyName(name: String?): Boolean {
        if (!AppSettings.setTvName(this, name)) return false
        renderName()
        if (ReceiverState.isInSession) Toast.makeText(this, R.string.rename_deferred, Toast.LENGTH_LONG).show()
        AirPlayService.reload(this)
        return true
    }

    /** Comando all'app che suona sull'iPhone (audio AirPlay). */
    private fun sendRemote(command: DacpRemote.Command) {
        val remote = AirPlayService.remote
        if (remote != null && remote.isAvailable) remote.send(command)
        else Toast.makeText(this, R.string.remote_unavailable, Toast.LENGTH_SHORT).show()
    }

    private fun renderNowPlaying(np: ReceiverState.NowPlaying) {
        binding.npTitle.text = np.title.ifBlank { getString(R.string.np_unknown_title) }
        binding.npLabel.setText(if (np.paused) R.string.np_paused else R.string.np_label)
        binding.npPlayPause.setImageResource(if (np.paused) R.drawable.ic_np_play else R.drawable.ic_np_pause)
        binding.npArtist.text = np.artist
        binding.npAlbum.text = np.album
        binding.npArtwork.setImageBitmap(np.artwork)
        binding.npPlaceholder.visibility = if (np.artwork == null) View.VISIBLE else View.GONE
        renderProgress(np)
    }

    // ------------------------------------------------------------ spostamento nel brano

    // Salto in preparazione: più pressioni ravvicinate diventano un solo comando all'iPhone.
    private var seekTargetMs = -1L
    // Dopo l'invio la barra resta sul punto scelto finché iOS non conferma la nuova posizione.
    private var seekShownUntil = 0L
    private val commitSeek = Runnable { sendSeek() }

    private fun onSeekKey(keyCode: Int, event: KeyEvent): Boolean {
        val step = when (keyCode) {
            KeyEvent.KEYCODE_DPAD_RIGHT -> SEEK_STEP_MS
            KeyEvent.KEYCODE_DPAD_LEFT -> -SEEK_STEP_MS
            else -> return false
        }
        if (event.action != KeyEvent.ACTION_DOWN) return true
        val np = ReceiverState.nowPlaying.value
        if (np.durationMs <= 0) return true
        val now = ReceiverState.elapsedNow()
        val base = if (seekTargetMs >= 0) seekTargetMs else np.currentPositionMs(now)
        // Mai oltre la fine: un salto lì farebbe partire il brano successivo.
        seekTargetMs = (base + step).coerceIn(0, (np.durationMs - SEEK_END_MARGIN_MS).coerceAtLeast(0))
        seekShownUntil = now + SEEK_DEBOUNCE_MS + SEEK_CONFIRM_MS
        renderProgress(np)
        binding.npSeek.removeCallbacks(commitSeek)
        binding.npSeek.postDelayed(commitSeek, SEEK_DEBOUNCE_MS)
        return true
    }

    private fun sendSeek() {
        val target = seekTargetMs
        if (target < 0) return
        val remote = AirPlayService.remote
        if (remote == null || !remote.isAvailable) {
            seekTargetMs = -1
            Toast.makeText(this, R.string.remote_unavailable, Toast.LENGTH_SHORT).show()
            return
        }
        val now = ReceiverState.elapsedNow()
        val from = ReceiverState.nowPlaying.value.currentPositionMs(now)
        seekShownUntil = now + SEEK_CONFIRM_MS + kotlin.math.abs(target - from) / 2
        remote.seekBy(from, target - from) { ok ->
            runOnUiThread {
                if (!ok) {
                    seekTargetMs = -1
                    seekShownUntil = 0
                    Toast.makeText(this, R.string.seek_unavailable, Toast.LENGTH_SHORT).show()
                    renderProgress(ReceiverState.nowPlaying.value)
                }
            }
        }
    }

    /** Posizione da mostrare: il salto scelto finché iOS non lo conferma, poi quella reale. */
    private fun shownPositionMs(np: ReceiverState.NowPlaying, now: Long): Long {
        val real = np.currentPositionMs(now)
        if (seekTargetMs < 0) return real
        if (now > seekShownUntil || kotlin.math.abs(real - seekTargetMs) < SEEK_CONFIRM_TOLERANCE_MS) {
            seekTargetMs = -1
            return real
        }
        return seekTargetMs
    }

    private fun renderProgress(np: ReceiverState.NowPlaying) {
        if (np.durationMs <= 0) {
            binding.npProgress.visibility = View.INVISIBLE
            binding.npPosition.text = ""
            binding.npDuration.text = ""
            return
        }
        val pos = shownPositionMs(np, ReceiverState.elapsedNow())
        binding.npProgress.visibility = View.VISIBLE
        binding.npProgress.progress = (pos * 1000 / np.durationMs).toInt()   // nessun lavoro se uguale
        setIfChanged(binding.npPosition, formatTime(pos))
        setIfChanged(binding.npDuration, formatTime(np.durationMs))
    }

    /** setText solo se il testo cambia: evita di ricalcolare l'impaginazione a ogni giro. */
    private fun setIfChanged(view: TextView, text: String) {
        if (view.text.toString() != text) view.text = text
    }

    private val lyricViews by lazy {
        listOf(binding.npL0, binding.npL1, binding.npL2, binding.npL3, binding.npL4)
    }

    /**
     * Testo del brano: 5 righe con quella in corso al centro. Con il testo sincronizzato la riga
     * è quella il cui tempo è appena passato (con 300 ms di anticipo, perché leggere richiede un
     * attimo); con il solo testo la posizione è stimata in proporzione alla durata.
     * L'album si nasconde per fare spazio.
     */
    // Ultimo testo disegnato: si riscrivono le righe solo quando cambiano (ogni setText
    // ricalcola l'impaginazione, e il ciclo gira 5 volte al secondo).
    private var drawnLyrics: Lyrics.Result? = null
    private var drawnLyricsLines: List<String> = emptyList()
    private var drawnLyricIndex = Int.MIN_VALUE

    private fun renderLyrics() {
        val result = ReceiverState.lyrics.value
        val np = ReceiverState.nowPlaying.value
        val pos = np.currentPositionMs(ReceiverState.elapsedNow())
        val (lines, index) = when (result) {
            is Lyrics.Result.Synced -> {
                val i = result.lines.indexOfLast { it.timeMs <= pos + 300 }
                val texts = if (result === drawnLyrics) drawnLyricsLines else result.lines.map { it.text.ifEmpty { "♪" } }
                texts to i
            }
            is Lyrics.Result.Plain -> {
                val i = if (np.durationMs > 0) (pos * result.lines.size / np.durationMs).toInt() else 0
                result.lines to i.coerceIn(0, result.lines.size - 1)
            }
            Lyrics.Result.Instrumental -> listOf(getString(R.string.lyrics_instrumental)) to 0
            else -> {
                binding.npLyrics.visibility = View.GONE
                binding.npAlbum.visibility = View.VISIBLE
                drawnLyrics = null
                return
            }
        }
        binding.npLyrics.visibility = View.VISIBLE
        binding.npAlbum.visibility = View.GONE
        if (result === drawnLyrics && index == drawnLyricIndex) return
        drawnLyrics = result
        drawnLyricsLines = lines
        drawnLyricIndex = index
        for (k in 0 until 5) {
            val i = index - 2 + k
            lyricViews[k].text = if (i in lines.indices) lines[i] else ""
        }
    }

    private fun renderLyricsSetting() {
        binding.lyricsSummary.setText(if (AppSettings.lyricsEnabled(this)) R.string.lyrics_on else R.string.lyrics_off)
    }

    private fun formatTime(ms: Long): String {
        val s = ms / 1000
        return if (s >= 3600) "%d:%02d:%02d".format(s / 3600, (s / 60) % 60, s % 60) else "%d:%02d".format(s / 60, s % 60)
    }

    /** Frequenza usata per i controlli del decoder: la stessa che il servizio chiede a iOS. */
    private val decoderFps: Int
        get() = if (VideoQuality.hardwareSupports(android.media.MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080, 60)) 60 else 30

    private fun renderVideoQuality() {
        val mode = VideoQuality.effective(AppSettings.videoQuality(this), decoderFps)
        binding.videoQualitySummary.setText(
            when (mode.quality) {
                VideoQuality.MAX -> R.string.video_quality_max
                VideoQuality.HIGH -> R.string.video_quality_high
                else -> R.string.video_quality_standard
            }
        )
    }

    /** Solo le scelte che il decoder di questa TV regge. */
    private fun showVideoQualityDialog() {
        val choices = VideoQuality.available(decoderFps)
        val labels = choices.map {
            getString(
                when (it) {
                    VideoQuality.MAX -> R.string.video_quality_max_desc
                    VideoQuality.HIGH -> R.string.video_quality_high_desc
                    else -> R.string.video_quality_standard_desc
                }
            )
        }.toTypedArray()
        val current = VideoQuality.effective(AppSettings.videoQuality(this), decoderFps).quality
        AlertDialog.Builder(this)
            .setTitle(R.string.video_quality_dialog_title)
            .setSingleChoiceItems(labels, choices.indexOf(current).coerceAtLeast(0)) { dialog, which ->
                val chosen = choices[which]
                if (chosen != current) {
                    AppSettings.setVideoQuality(this, chosen)
                    renderVideoQuality()
                    AirPlayService.reload(this)   // nuovo annuncio a iOS: formato e risoluzione
                    Toast.makeText(this, R.string.video_quality_changed, Toast.LENGTH_SHORT).show()
                }
                dialog.dismiss()
            }
            .setNegativeButton(R.string.rename_cancel, null)
            .show()
    }

    private fun renderPin() {
        binding.pinSummary.setText(
            when (AppSettings.pinMode(this)) {
                AppSettings.PIN_FIRST_TIME -> R.string.pin_summary_first
                AppSettings.PIN_ALWAYS -> R.string.pin_summary_always
                else -> R.string.pin_summary_off
            }
        )
    }

    /**
     * Scelta della modalità PIN. Cambiarla modifica il record mDNS (pw=true/false), quindi il
     * servizio ripubblica l'annuncio. "Dimentica" cancella le chiavi dei dispositivi associati:
     * al prossimo collegamento dovranno inserire di nuovo il PIN.
     */
    private fun showPinDialog() {
        val options = arrayOf(
            getString(R.string.pin_option_off),
            getString(R.string.pin_option_first),
            getString(R.string.pin_option_always),
        )
        val builder = AlertDialog.Builder(this)
            .setTitle(R.string.pin_dialog_title)
            .setSingleChoiceItems(options, AppSettings.pinMode(this)) { dialog, which ->
                if (which != AppSettings.pinMode(this)) {
                    AppSettings.setPinMode(this, which)
                    renderPin()
                    AirPlayService.reload(this)
                }
                dialog.dismiss()
            }
            .setNegativeButton(R.string.rename_cancel, null)
        val paired = TrustedDevices.count(this)
        if (paired > 0) {
            builder.setNeutralButton(getString(R.string.pin_forget, paired)) { _, _ ->
                TrustedDevices.clear(this)
                Toast.makeText(this, R.string.pin_forgotten, Toast.LENGTH_LONG).show()
                AirPlayService.reload(this)   // il server nativo ha una copia dell'elenco in memoria
            }
        }
        builder.show()
    }

    private fun renderAutoOpen() {
        val enabled = AppSettings.autoOpen(this)
        binding.autoOpenSummary.setText(
            when {
                !enabled -> R.string.auto_open_off
                !Settings.canDrawOverlays(this) -> R.string.auto_open_needs_permission
                else -> R.string.auto_open_on
            }
        )
    }

    private fun displayName(name: String) = name.ifBlank { getString(R.string.client_unknown) }

    /** Adatta la SurfaceView all'aspect ratio del video (bande nere ai lati o sopra e sotto). */
    private fun onVideoSize(width: Int, height: Int) {
        videoWidth = width
        videoHeight = height
        fitVideo()
    }

    private fun fitVideo() {
        val cw = binding.root.width
        val ch = binding.root.height
        val lp = binding.video.layoutParams as FrameLayout.LayoutParams
        val (w, h) = if (videoWidth <= 0 || videoHeight <= 0 || cw <= 0 || ch <= 0) {
            FrameLayout.LayoutParams.MATCH_PARENT to FrameLayout.LayoutParams.MATCH_PARENT
        } else if (cw.toLong() * videoHeight > ch.toLong() * videoWidth) {
            (ch.toLong() * videoWidth / videoHeight).toInt() to ch
        } else {
            cw to (cw.toLong() * videoHeight / videoWidth).toInt()
        }
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            binding.video.layoutParams = lp
        }
    }

    /**
     * Indirizzo IPv4 della TV sulla rete locale, cioè quello che vede l'iPhone.
     * Non si usa la rete "attiva": con una VPN accesa sulla TV quella è il tunnel (es. 10.x),
     * un indirizzo che nessun dispositivo della casa può raggiungere. Si cerca invece la rete
     * Ethernet o Wi-Fi fisica. Restituisce l'IP e se la connessione è via cavo.
     */
    @Suppress("DEPRECATION")   // allNetworks: alternativa non deprecata solo da API 31 con callback
    private fun localIpv4(): Pair<String, Boolean>? {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return null
        var best: Pair<String, Boolean>? = null
        for (network in cm.allNetworks) {
            val caps = cm.getNetworkCapabilities(network) ?: continue
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue
            val ethernet = caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
            if (!ethernet && !caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
            val ip = cm.getLinkProperties(network)?.linkAddresses
                ?.map { it.address }
                ?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress && !it.isLinkLocalAddress }
                ?.hostAddress ?: continue
            if (best == null || ethernet) best = ip to ethernet   // il cavo ha la precedenza
        }
        return best
    }

    private fun requestOverlayPermission() {
        try {
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
        } catch (e: ActivityNotFoundException) {
            // Molte TV non hanno questa schermata: l'unica alternativa è aprire iRPlay a mano.
            Toast.makeText(this, R.string.overlay_unavailable, Toast.LENGTH_LONG).show()
        }
    }

    companion object {
        private const val SEEK_STEP_MS = 10_000L
        private const val SEEK_DEBOUNCE_MS = 700L          // pausa tra le pressioni prima dell'invio
        private const val SEEK_CONFIRM_MS = 3000L          // attesa massima della conferma di iOS
        private const val SEEK_CONFIRM_TOLERANCE_MS = 2000L
        private const val SEEK_END_MARGIN_MS = 3000L
        /** Letto dal servizio per decidere se portare l'app in primo piano. Solo main thread scrive. */
        @Volatile
        var isVisible = false
            private set
    }
}
