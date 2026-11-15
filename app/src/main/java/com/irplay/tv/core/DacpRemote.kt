/*
 * DacpRemote.kt
 * Scopo: telecomando della TV verso l'app che suona sull'iPhone (Musica, Spotify, Podcast ...)
 * durante l'audio AirPlay: play/pausa, brano successivo e precedente.
 *
 * Come funziona (DACP, lo stesso meccanismo delle Apple TV e dei ricevitori AirPlay 1):
 *  - nelle richieste RTSP l'iPhone manda DACP-ID (16 cifre esadecimali) e Active-Remote (un
 *    numero che fa da token);
 *  - sulla rete annuncia il servizio mDNS "_dacp._tcp" con nome "iTunes_Ctrl_<DACP-ID>":
 *    da lì si ricava la porta;
 *  - un comando è una GET /ctrl-int/1/<comando> verso <iPhone>:<porta> con l'intestazione
 *    "Active-Remote: <token>". iOS risponde 204.
 *
 * Sicurezza: il token permette di comandare l'iPhone, quindi non va nel log e la richiesta
 * parte solo verso l'indirizzo della sessione AirPlay in corso (non verso l'host annunciato
 * via mDNS, che chiunque sulla rete potrebbe annunciare con lo stesso nome). Comandi solo da
 * un elenco fisso.
 */
package com.irplay.tv.core

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.SystemClock
import android.util.Log
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors

class DacpRemote(context: Context) {

    enum class Command(val path: String) {
        PLAY_PAUSE("playpause"),
        NEXT("nextitem"),
        PREVIOUS("previtem"),
    }

    private class Target(val dacpId: String, val token: String, val address: InetAddress) {
        @Volatile var port = 0
        // Indirizzo annunciato via mDNS (spesso l'IPv4 dell'iPhone), solo se della rete locale.
        @Volatile var announced: InetAddress? = null
        // Ultimo indirizzo che ha risposto: i comandi successivi partono da lì.
        @Volatile var working: InetAddress? = null
        // L'app ha rifiutato il salto diretto: si passa subito allo scorrimento veloce.
        @Volatile var directSeekRefused = false
    }

    private val nsd = context.getSystemService(NsdManager::class.java)
    // Un solo thread: i comandi partono in ordine e la rete non tocca mai il main thread.
    private val executor = Executors.newSingleThreadExecutor { Thread(it, "iRPlay-DACP") }
    private val lock = Any()
    private var target: Target? = null                         // protetto da lock
    private var discovery: NsdManager.DiscoveryListener? = null   // protetto da lock

    /**
     * Pausa chiesta dalla TV e confermata dall'iPhone (204). iOS manda il FLUSH solo dopo un
     * po', e nel frattempo la TV suonerebbe l'audio già ricevuto (circa 2 s) con la barra che
     * avanza: chi lo imposta (il servizio) ferma subito audio e avanzamento.
     */
    @Volatile var onPausedByTv: (() -> Unit)? = null

    /** True quando l'iPhone è stato trovato e i comandi possono partire. */
    val isAvailable: Boolean
        get() = (target?.port ?: 0) > 0

    /** Nuova sessione audio con DACP: si cerca la porta del servizio dell'iPhone. */
    fun attach(dacpId: String, token: String, peerAddr: String) {
        val address = try {
            // Solo un indirizzo IP letterale (arriva dal C++ via inet_ntop): nessuna risoluzione DNS.
            if (peerAddr.isEmpty() || peerAddr.any { !(it.isLetterOrDigit() || it == ':' || it == '.') }) return
            InetAddress.getByName(peerAddr)
        } catch (e: Exception) {
            return
        }
        synchronized(lock) {
            val current = target
            if (current != null && current.dacpId == dacpId && current.token == token && current.address == address) return
            stopDiscoveryLocked()
            target = Target(dacpId, token, address)
            startDiscoveryLocked()
        }
    }

    /** Fine sessione: niente più comandi verso quell'iPhone. */
    fun detach() {
        synchronized(lock) {
            stopDiscoveryLocked()
            target = null
        }
    }

    fun send(command: Command) {
        val t = target ?: return
        if (t.port <= 0 || executor.isShutdown) return
        // Stato prima del comando: play/pausa da "in riproduzione" vuol dire pausa.
        val pausing = command == Command.PLAY_PAUSE && !ReceiverState.nowPlaying.value.paused
        executor.execute {
            val code = request(t, command.path)
            if (code in 200..299) {
                if (pausing && target === t) onPausedByTv?.invoke()
            } else if (code > 0) {
                Log.w(TAG, "Comando ${command.path}: risposta $code")
            }
        }
    }

    /**
     * Inizio e fine di uno scorrimento veloce (vedi seekBy), chiamati sul thread DACP: il servizio
     * silenzia l'audio accelerato e, alla fine, riparte dal punto nuovo come dopo una pausa.
     */
    @Volatile var onScanStarted: (() -> Unit)? = null
    @Volatile var onScanEnded: (() -> Unit)? = null

    /**
     * Sposta il brano di deltaMs dalla posizione fromMs. Prima il salto diretto (DACP
     * "dacp.playingtime"); l'app Musica di iOS lo rifiuta (400), e allora si usa lo scorrimento
     * veloce (beginff/beginrew): iOS manda l'audio accelerato (circa 3x, variabile) e la posizione
     * 5 volte al secondo, quindi si guarda la posizione e ci si ferma (playresume) al punto scelto.
     * onResult(false) se nessuno dei due è accettato. onResult è chiamato sul thread DACP.
     */
    fun seekBy(fromMs: Long, deltaMs: Long, onResult: (Boolean) -> Unit) {
        val t = target
        if (t == null || t.port <= 0 || executor.isShutdown || deltaMs == 0L) {
            onResult(false)
            return
        }
        executor.execute {
            val goal = (fromMs + deltaMs).coerceIn(0, MAX_SEEK_MS)
            if (!t.directSeekRefused) {
                val code = request(t, "setproperty?dacp.playingtime=$goal")
                if (code in 200..299) {
                    onResult(true)
                    return@execute
                }
                if (code == 400) t.directSeekRefused = true   // non riprovare a ogni salto
            }
            val forward = deltaMs > 0
            onScanStarted?.invoke()
            if (request(t, if (forward) "beginff" else "beginrew") !in 200..299) {
                onScanEnded?.invoke()
                onResult(false)
                return@execute
            }
            // Ci si ferma un attimo prima del punto: la posizione arriva ogni ~200 ms e intanto
            // lo scorrimento va avanti di circa mezzo secondo.
            val deadline = SystemClock.elapsedRealtime() + MAX_SCAN_MS
            while (SystemClock.elapsedRealtime() < deadline) {
                val pos = ReceiverState.nowPlaying.value.currentPositionMs(ReceiverState.elapsedNow())
                if (if (forward) pos >= goal - SCAN_STOP_EARLY_MS else pos <= goal + SCAN_STOP_EARLY_MS) break
                Thread.sleep(SCAN_POLL_MS)
            }
            val ok = request(t, "playresume") in 200..299
            onScanEnded?.invoke()
            onResult(ok)
        }
    }

    /**
     * Una GET /ctrl-int/1/<path> verso l'iPhone; restituisce il codice HTTP, 0 se non inviata.
     * Socket diretto e non HttpURLConnection: la politica di Android sul traffico in chiaro
     * resta chiusa per tutta l'app, e qui serve una sola richiesta minima. Solo thread DACP.
     */
    private fun request(t: Target, path: String): Int {
        return try {
            val socket = connect(t) ?: return 0
            socket.use {
                socket.soTimeout = TIMEOUT_MS
                val request = "GET /ctrl-int/1/$path HTTP/1.1\r\n" +
                    "Host: iRPlay\r\nActive-Remote: ${t.token}\r\nConnection: close\r\n\r\n"
                socket.getOutputStream().apply {
                    write(request.toByteArray(Charsets.US_ASCII))
                    flush()
                }
                // Solo la riga di stato ("HTTP/1.1 204 No Content"), al massimo 128 byte.
                val input = socket.getInputStream()
                val line = StringBuilder()
                while (line.length < 128) {
                    val c = input.read()
                    if (c < 0 || c == '\n'.code) break
                    line.append(c.toChar())
                }
                line.split(' ').getOrNull(1)?.toIntOrNull() ?: 0
            }
        } catch (e: Exception) {
            Log.w(TAG, "Comando non inviato: ${e.javaClass.simpleName}")
            0
        }
    }

    /**
     * Prima l'indirizzo della sessione AirPlay, poi quello annunciato via mDNS. Il secondo serve
     * quando la TV non può aprire connessioni verso l'IPv6 dell'iPhone (per esempio con una VPN
     * attiva sulla TV, che instrada l'IPv6 nel tunnel) ma lo raggiunge in IPv4.
     */
    private fun connect(t: Target): Socket? {
        val candidates = listOfNotNull(t.working, t.address, t.announced).distinct()
        var last: Exception? = null
        for (address in candidates) {
            val socket = Socket()
            try {
                socket.connect(InetSocketAddress(address, t.port), TIMEOUT_MS)
                t.working = address
                return socket
            } catch (e: Exception) {
                runCatching { socket.close() }
                last = e
            }
        }
        Log.w(TAG, "iPhone non raggiungibile: ${last?.javaClass?.simpleName}")
        return null
    }

    fun release() {
        detach()
        executor.shutdown()
    }

    // ------------------------------------------------------------ mDNS

    private fun startDiscoveryLocked() {
        val manager = nsd ?: return
        val listener = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(info: NsdServiceInfo) {
                val t = target ?: return
                if (!sameDacpId(info.serviceName, t.dacpId)) return
                resolve(manager, info, t)
            }

            override fun onServiceLost(info: NsdServiceInfo) = Unit
            override fun onDiscoveryStarted(serviceType: String) = Unit
            override fun onDiscoveryStopped(serviceType: String) = Unit
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(TAG, "Ricerca del telecomando non avviata ($errorCode)")
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
        }
        try {
            manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
            discovery = listener
        } catch (e: Exception) {
            Log.w(TAG, "Ricerca del telecomando non avviata: ${e.javaClass.simpleName}")
        }
    }

    private fun stopDiscoveryLocked() {
        val listener = discovery ?: return
        discovery = null
        try {
            nsd?.stopServiceDiscovery(listener)
        } catch (_: Exception) {
        }
    }

    @Suppress("DEPRECATION")   // resolveService: l'alternativa (registerServiceInfoCallback) è solo da Android 14
    private fun resolve(manager: NsdManager, info: NsdServiceInfo, t: Target) {
        try {
            manager.resolveService(info, object : NsdManager.ResolveListener {
                override fun onServiceResolved(resolved: NsdServiceInfo) {
                    val port = resolved.port
                    if (port in 1..65535 && target === t) {
                        @Suppress("DEPRECATION")
                        val host = resolved.host
                        if (host != null && isLocalNetwork(host)) t.announced = host
                        t.port = port
                        Log.i(TAG, "Telecomando dell'iPhone disponibile")
                        // La porta basta: la ricerca può fermarsi.
                        synchronized(lock) { if (target === t) stopDiscoveryLocked() }
                    }
                }

                override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
                    Log.w(TAG, "Telecomando dell'iPhone non risolto ($errorCode)")
                }
            })
        } catch (e: Exception) {
            Log.w(TAG, "Telecomando dell'iPhone non risolto: ${e.javaClass.simpleName}")
        }
    }

    /**
     * "iTunes_Ctrl_0CE7F1BDDD2D5646" e DACP-ID "CE7F1BDDD2D5646" sono lo stesso iPhone: l'ID è un
     * numero a 64 bit e gli zeri iniziali possono mancare da una delle due parti. Si confronta il valore.
     */
    private fun sameDacpId(serviceName: String, dacpId: String): Boolean {
        if (!serviceName.startsWith(SERVICE_PREFIX, ignoreCase = true)) return false
        val a = serviceName.substring(SERVICE_PREFIX.length).toULongOrNull(16) ?: return false
        val b = dacpId.toULongOrNull(16) ?: return false
        return a == b
    }

    /**
     * Indirizzi privati o di rete locale. Un nome mDNS lo può annunciare chiunque sulla rete:
     * con questo controllo il token non esce comunque dalla LAN.
     */
    private fun isLocalNetwork(a: InetAddress): Boolean {
        if (a.isSiteLocalAddress || a.isLinkLocalAddress) return true
        val b = a.address
        return b.size == 16 && (b[0].toInt() and 0xFE) == 0xFC   // IPv6 ULA fc00::/7
    }

    private companion object {
        const val TAG = "iRPlay_DACP"
        const val SERVICE_TYPE = "_dacp._tcp"
        const val SERVICE_PREFIX = "iTunes_Ctrl_"
        const val TIMEOUT_MS = 2000
        const val MAX_SEEK_MS = 24L * 3600 * 1000
        const val MAX_SCAN_MS = 20_000L        // limite di sicurezza: mai più di 20 s di scorrimento
        const val SCAN_POLL_MS = 50L
        const val SCAN_STOP_EARLY_MS = 400L
    }
}
