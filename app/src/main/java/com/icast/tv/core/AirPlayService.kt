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
 */
package com.icast.tv.core

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.IBinder
import android.util.Log
import com.icast.tv.R
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class AirPlayService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var multicastLock: WifiManager.MulticastLock? = null
    private var nsd: NsdManager? = null
    private val registrations = mutableListOf<NsdManager.RegistrationListener>()
    private var started = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForeground va chiamato subito: Android termina il servizio se non arriva entro pochi secondi.
        startInForeground()
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
                    stopSelf()
                    return@launch
                }
                registerServices(identity)
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        nsd?.let { n ->
            // unregisterService lancia IllegalArgumentException se la registrazione era già fallita.
            registrations.forEach { runCatching { n.unregisterService(it) } }
        }
        registrations.clear()
        multicastLock?.takeIf { it.isHeld }?.release()
        multicastLock = null
        super.onDestroy()
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
        val notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.receiver_waiting))
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setOngoing(true)
            .build()
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
    }

    /**
     * Alcuni driver Wi-Fi delle TV (soprattutto Realtek e Amlogic) scartano i pacchetti
     * multicast in risparmio energetico: l'iPhone smetterebbe di "vedere" la TV dopo
     * qualche minuto. Il lock mantiene attiva la ricezione multicast mentre il servizio vive.
     * Non contato per riferimenti: un solo acquire, un solo release.
     */
    private fun acquireMulticastLock() {
        val wifi = applicationContext.getSystemService(WifiManager::class.java) ?: return
        multicastLock = wifi.createMulticastLock("IcAst_MulticastLock").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun registerServices(id: DeviceIdentity) {
        val manager = getSystemService(NsdManager::class.java) ?: return
        nsd = manager

        val airplay = NsdServiceInfo().apply {
            serviceName = DEVICE_NAME
            serviceType = "_airplay._tcp"
            port = RTSP_PORT
            setAttribute("deviceid", id.deviceId)   // identità stabile del ricevitore
            setAttribute("features", FEATURES)      // bitmask delle funzioni supportate (vedi sotto)
            setAttribute("flags", "0x84")
            setAttribute("model", MODEL)            // iOS sceglie la versione del protocollo in base al modello
            setAttribute("pi", id.pairingId)
            setAttribute("pk", id.publicKeyHex)     // chiave pubblica Ed25519 per pair-verify
            setAttribute("srcvers", SRCVERS)
            setAttribute("vv", "2")
        }

        val raop = NsdServiceInfo().apply {
            serviceName = "${id.raopPrefix}@$DEVICE_NAME"
            serviceType = "_raop._tcp"
            port = RTSP_PORT
            mapOf(
                "txtvers" to "1",
                "ch" to "2",            // canali audio
                "cn" to "0,1,2,3",      // codec: PCM, ALAC, AAC, AAC-ELD
                "da" to "true",
                "et" to "0,3,5",        // cifratura: nessuna, FairPlay, FairPlay SAPv2.5
                "vv" to "2",
                "ft" to FEATURES,
                "am" to MODEL,
                "md" to "0,1,2",        // metadati: testo, copertina, avanzamento
                "rhd" to "5.6.0.0",
                "pw" to "false",        // nessun PIN per ora: verrà aggiunto nella Fase 6
                "sf" to "0x4",
                "sr" to "44100",
                "ss" to "16",
                "sv" to "false",
                "tp" to "UDP",
                "vs" to SRCVERS,
                "vn" to "65537",
                "pk" to id.publicKeyHex,
            ).forEach { (k, v) -> setAttribute(k, v) }
        }

        register(manager, airplay)
        register(manager, raop)
    }

    private fun register(manager: NsdManager, info: NsdServiceInfo) {
        val type = info.serviceType
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(s: NsdServiceInfo) {
                // In caso di conflitto di nome il sistema rinomina in automatico ("IcAst (2)").
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
        private const val TAG = "IcAst_Service"
        private const val CHANNEL_ID = "icast_receiver"
        private const val NOTIFICATION_ID = 1

        private const val DEVICE_NAME = "IcAst"
        const val RTSP_PORT = 7000

        // Feature flags di un ricevitore per mirroring dello schermo (gli stessi di UxPlay).
        // Il bit 27 ("legacy pairing") è attivo: fa usare a iOS il pair-setup/verify
        // Ed25519 che implementeremo, invece del pairing HomeKit di AirPlay 2.
        private const val FEATURES = "0x5A7FFEE6"
        private const val MODEL = "AppleTV3,2"
        private const val SRCVERS = "220.68"

        /** Va chiamato con l'app in primo piano: da Android 12 non si può avviare dal background. */
        fun start(context: Context) {
            context.startForegroundService(Intent(context, AirPlayService::class.java))
        }
    }
}