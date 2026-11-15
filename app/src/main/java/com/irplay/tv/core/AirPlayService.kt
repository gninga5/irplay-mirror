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
import android.media.MediaCodecList
import android.media.MediaFormat
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.IBinder
import android.util.Log
import java.io.ByteArrayOutputStream
import com.irplay.tv.R
import com.irplay.tv.audio.AudioRenderer
import com.irplay.tv.ui.MainActivity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
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
    private var identity: DeviceIdentity? = null
    private var deviceName = ""
    // Cambio di nome richiesto durante una sessione: applicato quando finisce, per non
    // interrompere la duplicazione in corso.
    @Volatile private var renamePending = false

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

                audio = AudioRenderer(applicationContext).also { NativeBridge.audioSink = it }
                NativeBridge.sessionListener = sessionListener
                NativeBridge.pinListener = pinListener
                this@AirPlayService.identity = identity
                publish()
            }
        }
        return START_STICKY
    }

    /** Avvia il server e annuncia la TV con il nome corrente. */
    private suspend fun publish() {
        val id = identity ?: return
        deviceName = AppSettings.tvName(applicationContext)
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
        ReceiverState.setPin(null)
        NativeBridge.stopServer()     // prima si ritira l'annuncio, poi si chiudono le connessioni
        NativeBridge.audioSink = null
        audio?.release()
        audio = null
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
            ReceiverState.set(ReceiverState.Status.Ready)
            updateNotification(getString(R.string.receiver_waiting))
            if (renamePending) scope.launch { republish() }
        }
    }

    /**
     * Eventi del PIN (thread nativi). Il PIN va mostrato anche se l'apertura automatica è
     * disattivata: senza vederlo l'utente non può completare l'associazione. Compare sia
     * nell'app sia nella notifica, così è leggibile anche se Android blocca l'apertura.
     */
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
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(s: NsdServiceInfo) {
                // In caso di conflitto di nome il sistema rinomina in automatico ("iRPlay (2)").
                Log.i(TAG, "Annunciato: ${s.serviceName} ($type)")
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
        private const val CHANNEL_ID = "irplay_receiver"
        private const val NOTIFICATION_ID = 1


        private const val PREFERRED_RTSP_PORT = 7000

        // Feature flags di un ricevitore per mirroring dello schermo (gli stessi di UxPlay).
        // Il bit 27 ("legacy pairing") è attivo: fa usare a iOS il pair-setup/verify
        // Ed25519 che implementeremo, invece del pairing HomeKit di AirPlay 2.
        private const val FEATURES = 0x5A7FFEE6L
        // Forma dei TXT "bit 0-31,bit 32-63", come UxPlay: "0x5A7FFEE6,0x0".
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
    }
}