/*
 * CodecManager.kt
 * Scopo: decodifica hardware H.264 o HEVC con MediaCodec in modalità asincrona e rendering
 * diretto sulla Surface della SurfaceView.
 *
 * Perché così:
 *  - SurfaceView e non TextureView: il buffer decodificato va al compositore hardware
 *    (overlay) senza passare dalla GPU dell'app. TextureView aggiunge una copia per frame
 *    e, sulle chiavette Amlogic/Realtek, perde frame a 1080p.
 *  - Modalità asincrona con un HandlerThread dedicato: tutte le callback del codec e tutto
 *    lo stato di questa classe vivono su un solo thread ("iRPlay-Video"), quindi niente lock.
 *  - Ogni output viene rilasciato subito con render=true: nel mirroring conta la latenza,
 *    non il ritmo preciso dei frame.
 *  - KEY_LOW_LATENCY (Android 11+) disattiva il riordino dei frame nel decoder, dove
 *    supportato. Alcuni decoder MediaTek rifiutano la chiave: si riprova senza.
 *
 * Trabocchetti hardware:
 *  - Dopo configure() il decoder deve ricevere prima un IDR: i frame P precedenti vengono
 *    scartati, altrimenti alcuni chip mostrano macroblocchi verdi fino al keyframe successivo.
 *  - Se la Surface non c'è ancora (Activity in apertura) si conservano i frame dall'ultimo
 *    IDR, entro un limite di memoria: all'arrivo della Surface l'immagine compare subito
 *    invece di attendere il prossimo keyframe di iOS (che può tardare molti secondi).
 *  - Le callback di un codec già rilasciato possono arrivare in ritardo: il contatore
 *    "generation" le fa ignorare.
 */
package com.irplay.tv.core

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class CodecManager(private val onVideoSize: (width: Int, height: Int) -> Unit) : NativeBridge.VideoSink {

    private class Frame(val data: ByteArray, val ptsUs: Long, val key: Boolean, val arrivalNs: Long)

    private val thread = HandlerThread("iRPlay-Video", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
    private val handler = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())

    // Contropressione tra il thread nativo e "iRPlay-Video": se il decoder si blocca, i frame
    // non devono accumularsi senza limite nella coda dei messaggi.
    private val inFlight = AtomicInteger()
    private val dropUntilKey = AtomicBoolean(false)

    // --- stato confinato al thread "iRPlay-Video" ---
    private var surface: Surface? = null
    private var codec: MediaCodec? = null
    private var sps: ByteArray? = null   // csd-0: SPS (H.264) o VPS+SPS+PPS (HEVC)
    private var pps: ByteArray? = null   // csd-1: PPS (H.264), vuoto per HEVC
    private var codecType = NativeBridge.CODEC_H264
    private val pending = ArrayDeque<Frame>()
    private var pendingBytes = 0L
    private val freeInputs = ArrayDeque<Int>()
    private var waitKeyFrame = true
    private var generation = 0
    private var consecutiveErrors = 0
    private val stats = LatencyStats()

    fun attachSurface(s: Surface) {
        handler.post {
            surface = s
            restartCodec()
        }
    }

    /**
     * Va chiamata da surfaceDestroyed e attende il rilascio del codec: Android richiede
     * che nessuno scriva più sulla Surface quando quel metodo ritorna.
     */
    fun detachSurface() {
        runOnCodecThreadAndWait {
            releaseCodec()
            surface = null
        }
    }

    fun release() {
        detachSurface()
        thread.quitSafely()
    }

    // ------------------------------------------------------------ VideoSink (thread nativi)

    override fun onVideoStarted() {
        handler.post {
            clearPending()
            waitKeyFrame = true
        }
    }

    override fun onVideoStopped() {
        handler.post {
            releaseCodec()
            clearPending()
            sps = null
            pps = null
        }
    }

    override fun onVideoConfig(codec: Int, csd0: ByteArray, csd1: ByteArray) {
        val sps = csd0
        val pps = csd1
        handler.post {
            if (codec != codecType || !sps.contentEquals(this.sps) || !pps.contentEquals(this.pps)) {
                codecType = codec
                this.sps = sps
                this.pps = pps
                clearPending()
                restartCodec()
            }
        }
    }

    override fun onVideoFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) {
        val arrival = System.nanoTime()
        if (inFlight.incrementAndGet() > MAX_IN_FLIGHT) {
            inFlight.decrementAndGet()
            dropUntilKey.set(true)
            return
        }
        handler.post {
            inFlight.decrementAndGet()
            enqueue(Frame(data, ptsUs, keyFrame, arrival))
        }
    }

    // ------------------------------------------------------------ thread "iRPlay-Video"

    private fun enqueue(f: Frame) {
        if (dropUntilKey.get()) {
            if (!f.key) return
            dropUntilKey.set(false)
            clearPending()
        }
        if (f.key) clearPending()   // un IDR rende inutili i frame precedenti
        if (pending.isEmpty() && !f.key && waitKeyFrame) return
        pending.addLast(f)
        pendingBytes += f.data.size

        val limitFrames = if (codec == null) MAX_PENDING_NO_SURFACE else MAX_PENDING_DECODING
        if (pending.size > limitFrames || pendingBytes > MAX_PENDING_BYTES) {
            // Decoder troppo lento o Surface assente da troppo: meglio un'immagine ferma
            // fino al prossimo IDR che una latenza che cresce senza limite.
            clearPending()
            waitKeyFrame = true
            return
        }
        drain()
    }

    private fun clearPending() {
        pending.clear()
        pendingBytes = 0
    }

    private fun drain() {
        val c = codec ?: return
        while (freeInputs.isNotEmpty() && pending.isNotEmpty()) {
            val f = pending.removeFirst()
            pendingBytes -= f.data.size
            if (waitKeyFrame && !f.key) continue
            val index = freeInputs.first()
            val buf: ByteBuffer = try {
                c.getInputBuffer(index)
            } catch (e: IllegalStateException) {
                null
            } ?: return
            if (f.data.size > buf.capacity()) {
                Log.w(TAG, "Frame di ${f.data.size} byte oltre la capacità del decoder, scartato")
                waitKeyFrame = true
                continue
            }
            buf.clear()
            buf.put(f.data)
            freeInputs.removeFirst()
            stats.onQueued(f.ptsUs, f.arrivalNs)
            try {
                c.queueInputBuffer(index, 0, f.data.size, f.ptsUs,
                    if (f.key) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            } catch (e: IllegalStateException) {
                Log.w(TAG, "queueInputBuffer fallita, riavvio il decoder")
                restartCodec()
                return
            }
            waitKeyFrame = false
        }
    }

    /**
     * Livelli di configurazione, dal più aggressivo al più compatibile: se il decoder rifiuta
     * una chiave sconosciuta, configure() fallisce e si riprova col livello successivo.
     *  2: chiave standard KEY_LOW_LATENCY + chiavi proprietarie del produttore + priorità realtime
     *  1: solo KEY_LOW_LATENCY
     *  0: nessuna opzione
     * Le chiavi proprietarie (le stesse usate da Moonlight) disattivano il riordino dei frame
     * nei decoder che non leggono KEY_LOW_LATENCY, come gli OMX MediaTek di Android 11-12.
     */
    private fun buildFormat(sps: ByteArray, pps: ByteArray, level: Int, codecName: String): MediaFormat =
        (if (codecType == NativeBridge.CODEC_HEVC) MediaFormat.createVideoFormat(MIME_HEVC, MAX_WIDTH_HEVC, MAX_HEIGHT_HEVC)
         else MediaFormat.createVideoFormat(MIME, MAX_WIDTH, MAX_HEIGHT)).apply {
            setByteBuffer("csd-0", ByteBuffer.wrap(sps))
            if (pps.isNotEmpty()) setByteBuffer("csd-1", ByteBuffer.wrap(pps))   // HEVC: tutto in csd-0
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_INPUT_SIZE)
            if (level >= 1 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            if (level >= 2) {
                setInteger(MediaFormat.KEY_PRIORITY, 0)   // 0 = tempo reale
                val n = codecName.lowercase()
                when {
                    n.contains("mtk") || n.contains("mediatek") -> setInteger("vdec-lowlatency", 1)
                    n.contains("qcom") || n.contains("qti") -> {
                        setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
                        setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
                    }
                    n.contains("hisi") -> {
                        setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1)
                        setInteger("vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-rdy", -1)
                    }
                    n.contains("rtk") || n.contains("realtek") -> setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
                    n.contains("amlogic") -> setInteger("vendor.low-latency.enable", 1)
                }
            }
        }

    private fun restartCodec() {
        releaseCodec()
        val s = surface ?: return
        val sps = sps ?: return
        val pps = pps ?: return
        if (!s.isValid) return

        val gen = ++generation
        val c = try {
            MediaCodec.createDecoderByType(if (codecType == NativeBridge.CODEC_HEVC) MIME_HEVC else MIME)
        } catch (e: Exception) {
            Log.e(TAG, "Nessun decoder per il formato video: ${e.javaClass.simpleName}")
            return
        }
        val callback = object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
                if (gen != generation) return
                freeInputs.addLast(index)
                drain()
            }

            override fun onOutputBufferAvailable(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                if (gen != generation) return
                consecutiveErrors = 0
                stats.onOutput(info.presentationTimeUs, pending.size)
                try {
                    // Con un timestamp (adesso) il compositore, se trova due frame pronti per lo
                    // stesso refresh, mostra il più recente e scarta l'altro invece di accodarli.
                    if (info.size > 0) mc.releaseOutputBuffer(index, System.nanoTime())
                    else mc.releaseOutputBuffer(index, false)
                } catch (e: IllegalStateException) {
                    // codec in chiusura: niente da fare
                }
            }

            override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
                if (gen != generation) return
                Log.w(TAG, "Errore del decoder (recuperabile=${e.isRecoverable}), riavvio")
                if (++consecutiveErrors <= MAX_CONSECUTIVE_ERRORS) {
                    handler.post { if (gen == generation) restartCodec() }
                } else {
                    Log.e(TAG, "Decoder in errore ripetuto: attendo un nuovo formato video")
                    handler.post { if (gen == generation) releaseCodec() }
                }
            }

            override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
                if (gen != generation) return
                reportSize(format)
            }
        }

        val configured = (2 downTo 0).any { level ->
            // reset() dopo un configure fallito può azzerare la callback: va reimpostata a ogni tentativo.
            c.setCallback(callback, handler)
            configure(c, buildFormat(sps, pps, level, c.name), s).also {
                if (it) Log.i(TAG, "Decoder configurato con livello bassa latenza $level")
            }
        }
        if (!configured) {
            Log.e(TAG, "Configurazione del decoder fallita")
            c.release()
            return
        }
        try {
            c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
            c.start()
        } catch (e: Exception) {
            Log.e(TAG, "Avvio del decoder fallito: ${e.javaClass.simpleName}")
            c.release()
            return
        }
        codec = c
        waitKeyFrame = true
        Log.i(TAG, "Decoder avviato: ${c.name}")
        drain()
    }

    private fun configure(c: MediaCodec, format: MediaFormat, s: Surface): Boolean = try {
        c.configure(format, s, null, 0)
        true
    } catch (e: Exception) {
        try {
            c.reset()   // dopo un configure fallito il codec va riportato allo stato iniziale
        } catch (_: Exception) {
        }
        false
    }

    private fun releaseCodec() {
        generation++
        freeInputs.clear()
        codec?.let {
            try {
                it.stop()
            } catch (_: Exception) {
            }
            try {
                it.release()
            } catch (_: Exception) {
            }
        }
        codec = null
    }

    private fun reportSize(format: MediaFormat) {
        var w = format.getInteger(MediaFormat.KEY_WIDTH)
        var h = format.getInteger(MediaFormat.KEY_HEIGHT)
        // Il crop esclude le righe di padding che H.264 aggiunge per arrivare a multipli di 16
        // (1080 -> 1088): senza, l'aspect ratio risulta leggermente sbagliato.
        if (format.containsKey(KEY_CROP_RIGHT) && format.containsKey(KEY_CROP_LEFT)) {
            w = format.getInteger(KEY_CROP_RIGHT) - format.getInteger(KEY_CROP_LEFT) + 1
        }
        if (format.containsKey(KEY_CROP_BOTTOM) && format.containsKey(KEY_CROP_TOP)) {
            h = format.getInteger(KEY_CROP_BOTTOM) - format.getInteger(KEY_CROP_TOP) + 1
        }
        Log.i(TAG, "Risoluzione del video ricevuto: ${w}x$h")
        if (w > 0 && h > 0) main.post { onVideoSize(w, h) }
    }

    private fun runOnCodecThreadAndWait(block: () -> Unit) {
        if (Looper.myLooper() == thread.looper) {
            block()
            return
        }
        val done = CountDownLatch(1)
        if (!handler.post { block(); done.countDown() }) return
        done.await(WAIT_TIMEOUT_MS, TimeUnit.MILLISECONDS)
    }

    /**
     * Misura della latenza dentro la TV: dall'arrivo del frame dalla rete (onVideoFrame) al
     * momento in cui il decoder lo restituisce pronto per lo schermo. Un riepilogo ogni 5 s
     * nel log (tag iRPlay_Latency): serve a capire se il ritardo è nel decoder o nella coda.
     * Confinata al thread "iRPlay-Video".
     */
    private class LatencyStats {
        private val arrivals = HashMap<Long, Long>()
        private var count = 0
        private var sumMs = 0.0
        private var maxMs = 0.0
        private var maxPending = 0
        private var windowStart = System.nanoTime()

        fun onQueued(ptsUs: Long, arrivalNs: Long) {
            if (arrivals.size > 512) arrivals.clear()
            arrivals[ptsUs] = arrivalNs
        }

        fun onOutput(ptsUs: Long, pending: Int) {
            val now = System.nanoTime()
            arrivals.remove(ptsUs)?.let {
                val ms = (now - it) / 1e6
                count++
                sumMs += ms
                if (ms > maxMs) maxMs = ms
            }
            if (pending > maxPending) maxPending = pending
            if (now - windowStart >= 5_000_000_000L) {
                if (count > 0) {
                    Log.i(TAG_LATENCY, "fps %.1f, rete->schermo media %.1f ms, max %.1f ms, coda max %d"
                        .format((count * 1e9 / (now - windowStart)), sumMs / count, maxMs, maxPending))
                }
                count = 0; sumMs = 0.0; maxMs = 0.0; maxPending = 0; windowStart = now
            }
        }
    }

    companion object {
        private const val TAG = "iRPlay_Codec"
        private const val TAG_LATENCY = "iRPlay_Latency"
        private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val MIME_HEVC = MediaFormat.MIMETYPE_VIDEO_HEVC
        private const val MAX_WIDTH_HEVC = 3840
        private const val MAX_HEIGHT_HEVC = 2160
        private const val MAX_WIDTH = 1920
        private const val MAX_HEIGHT = 1080
        private const val MAX_INPUT_SIZE = 2 * 1024 * 1024
        private const val MAX_IN_FLIGHT = 60
        private const val MAX_PENDING_DECODING = 90          // circa 3 s a 30 fps
        private const val MAX_PENDING_NO_SURFACE = 900       // fino a 30 s dall'ultimo IDR
        private const val MAX_PENDING_BYTES = 24L * 1024 * 1024
        private const val MAX_CONSECUTIVE_ERRORS = 3
        private const val WAIT_TIMEOUT_MS = 2000L
        private const val KEY_CROP_LEFT = "crop-left"
        private const val KEY_CROP_RIGHT = "crop-right"
        private const val KEY_CROP_TOP = "crop-top"
        private const val KEY_CROP_BOTTOM = "crop-bottom"
    }
}
