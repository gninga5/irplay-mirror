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
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.irplay.tv.R
import com.irplay.tv.core.AirPlayService
import com.irplay.tv.core.AppSettings
import com.irplay.tv.core.CodecManager
import com.irplay.tv.core.NativeBridge
import com.irplay.tv.core.ReceiverState
import com.irplay.tv.databinding.ActivityMainBinding
import java.net.Inet4Address
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
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
        binding.licensesButton.setOnClickListener { startActivity(Intent(this, LicensesActivity::class.java)) }
        binding.autoOpenRow.setOnClickListener { onAutoOpenPressed() }

        AirPlayService.start(this)
        scope.launch { ReceiverState.status.collect { render(it) } }
    }

    override fun onStart() {
        super.onStart()
        isVisible = true
        binding.ipAddress.text = localIpv4()?.let { (ip, ethernet) ->
            getString(if (ethernet) R.string.ip_ethernet else R.string.ip_wifi, ip)
        } ?: getString(R.string.receiver_no_network)
        renderAutoOpen()   // anche al ritorno dalla schermata di sistema dei permessi
        renderName()
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
        return super.onKeyDown(keyCode, event)
    }

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) = codec.attachSurface(holder.surface)
        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
        override fun surfaceDestroyed(holder: SurfaceHolder) = codec.detachSurface()
    }

    private fun render(status: ReceiverState.Status) {
        val mirroring = status is ReceiverState.Status.Mirroring
        binding.idlePanel.visibility = if (mirroring) View.GONE else View.VISIBLE
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
            ReceiverState.Status.Failed -> getString(R.string.status_failed)
        }
        val (short, color) = when (status) {
            ReceiverState.Status.Starting -> R.string.status_short_starting to R.color.status_starting
            ReceiverState.Status.Ready -> R.string.status_short_ready to R.color.status_ready
            is ReceiverState.Status.Connected -> R.string.status_short_connected to R.color.status_active
            is ReceiverState.Status.Mirroring -> R.string.status_short_mirroring to R.color.status_active
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
        val name = AppSettings.tvName(this)
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

    private fun renderAutoOpen() {
        val enabled = AppSettings.autoOpen(this)
        binding.autoOpenSwitch.isChecked = enabled
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
        /** Letto dal servizio per decidere se portare l'app in primo piano. Solo main thread scrive. */
        @Volatile
        var isVisible = false
            private set
    }
}
