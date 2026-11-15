/*
 * AudioRenderer.kt
 * Scopo: riproduzione dell'audio AirPlay con AudioTrack, in due modalità.
 *
 *  - Duplicazione schermo (AAC-ELD, 44,1 kHz, stereo, 480 campioni per frame): conta la
 *    latenza. Coda di ~100 ms, AudioTrack in PERFORMANCE_MODE_LOW_LATENCY, buffer piccolo.
 *  - Musica (la TV scelta come altoparlante): conta la continuità. iOS manda ALAC (che il
 *    codice nativo ha già trasformato in PCM 16 bit) o AAC-LC. Coda di qualche secondo,
 *    perché all'inizio del brano iOS invia i pacchetti a raffica, e buffer AudioTrack ampio:
 *    sui SoC TV lenti un buffer minimo produce crepitii quando la CPU è occupata.
 *
 * Perché così:
 *  - MediaCodec "audio/mp4a-latm" con csd-0 = AudioSpecificConfig per AAC: il decoder AAC
 *    software di Android (presente su tutte le TV da Android 10) supporta anche l'ELD.
 *    I byte F8 E8 50 00 descrivono ELD, 44,1 kHz, 2 canali, 480 campioni per frame.
 *  - Un thread dedicato con coda limitata: i pacchetti arrivano dal thread di rete, che non
 *    deve mai attendere il decoder. Oltre il limite si scartano i più vecchi.
 *  - Scrittura bloccante su AudioTrack in MODE_STREAM: è la scrittura stessa a dare il ritmo.
 *    Per la musica ogni pacchetto ha anche un istante di uscita (sincronizzazione di iOS):
 *    la TV suona insieme all'orologio dell'iPhone, e pausa e ripresa coincidono con il telefono.
 *  - Audio focus richiesto all'avvio dello stream e rilasciato alla fine: se un'altra app lo
 *    prende, l'audio dell'iPhone viene silenziato invece di sovrapporsi.
 *
 * Volume AirPlay: iOS invia dB da -30 (minimo) a 0; -144 significa muto.
 */
package com.irplay.tv.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.SystemClock
import android.util.Log
import com.irplay.tv.core.NativeBridge
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.pow

class AudioRenderer(context: Context) : NativeBridge.AudioSink {

    private val audioManager = context.getSystemService(AudioManager::class.java)
    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
        .build()
    /** Pacchetto e istante di uscita (SystemClock.uptimeMillis; 0 = subito). */
    private class Frame(val data: ByteArray, val dueMs: Long)

    private val queue = LinkedBlockingQueue<Frame>(QUEUE_CAPACITY)
    private val lock = Any()
    private val flushRequested = AtomicBoolean(false)

    private var worker: Thread? = null              // protetto da lock
    private var focusRequest: AudioFocusRequest? = null   // protetto da lock
    @Volatile private var maxBacklog = BACKLOG_MIRRORING
    @Volatile private var track: AudioTrack? = null
    @Volatile private var gain = 1f
    @Volatile private var focusFactor = 1f
    // Scorrimento veloce chiesto dalla TV: iOS manda l'audio accelerato, che non si fa sentire.
    @Volatile private var scanMuted = false
    // Pacchetti scartati perché la coda era piena (solo dal thread di rete), per il log di fine stream.
    @Volatile private var dropped = 0L
    // Frame PCM scritti nell'AudioTrack corrente (thread audio), per stimare l'audio non ancora suonato.
    @Volatile private var framesWritten = 0L

    /** csd = null: PCM 16 bit, nessun decoder. */
    private class Config(val csd: ByteArray?, val profile: Int, val music: Boolean)

    override fun onAudioStarted(codecType: Int) {
        val config = when (codecType) {
            CT_PCM -> Config(null, 0, music = true)
            CT_AAC_LC -> Config(AAC_LC_CSD, MediaCodecInfo.CodecProfileLevel.AACObjectLC, music = true)
            CT_AAC_ELD -> Config(AAC_ELD_CSD, MediaCodecInfo.CodecProfileLevel.AACObjectELD, music = false)
            else -> {
                Log.w(TAG, "Formato audio non supportato (ct=$codecType)")
                return
            }
        }
        synchronized(lock) {
            stopWorkerLocked()
            queue.clear()
            flushRequested.set(false)
            dropped = 0
            maxBacklog = if (config.music) BACKLOG_MUSIC else BACKLOG_MIRRORING
            requestFocusLocked()
            worker = Thread({ run(config) }, "iRPlay-Audio").apply {
                priority = Thread.MAX_PRIORITY
                start()
            }
        }
    }

    override fun onAudioStopped() {
        synchronized(lock) {
            stopWorkerLocked()
            abandonFocusLocked()
        }
        queue.clear()
    }

    /**
     * L'orologio audio dell'iPhone e quello della TV non coincidono mai esattamente: se l'iPhone
     * è un po' più veloce la coda cresce lentamente, e con lei il ritardo. Oltre maxBacklog si
     * scartano i pacchetti più vecchi, così la latenza resta limitata.
     */
    override fun onAudioFrame(data: ByteArray, dueMs: Long) {
        while (queue.size >= maxBacklog) {
            if (queue.poll() != null) dropped++
        }
        queue.offer(Frame(data, dueMs))
    }

    override fun onAudioFlush() {
        queue.clear()
        flushRequested.set(true)   // il thread audio svuota anche AudioTrack e decoder
    }

    override fun onVolume(db: Float) {
        gain = if (db <= MIN_DB) 0f else 10f.pow(db / 20f)
        applyVolume()
    }

    fun release() = onAudioStopped()

    /** Audio scritto nell'AudioTrack ma non ancora suonato, in ms (thread audio). */
    private fun trackPendingMs(t: AudioTrack): Long = try {
        val head = t.playbackHeadPosition.toLong() and 0xFFFFFFFFL
        (framesWritten - head).coerceAtLeast(0) * 1000 / t.sampleRate.coerceAtLeast(1)
    } catch (_: IllegalStateException) {
        0
    }

    /**
     * Musica: iOS manda ogni pacchetto circa 1,8 s prima dell'istante in cui va suonato. Si
     * aspetta finché, sommato all'audio già nell'AudioTrack, il pacchetto uscirebbe in orario.
     * false se nel frattempo è arrivato un FLUSH (pausa, salto): il pacchetto va scartato.
     */
    private fun waitUntilDue(frame: Frame, t: AudioTrack): Boolean {
        if (frame.dueMs <= 0) return true
        while (true) {
            if (flushRequested.get()) return false
            val wait = frame.dueMs - SystemClock.uptimeMillis() - trackPendingMs(t)
            if (wait <= 0) return true
            Thread.sleep(minOf(wait, MAX_WAIT_STEP_MS))   // a passi brevi: un FLUSH non aspetta
        }
    }

    // ------------------------------------------------------------ thread "iRPlay-Audio"

    private fun run(config: Config) {
        var codec: MediaCodec? = null
        try {
            if (config.csd != null) {
                codec = MediaCodec.createDecoderByType(MIME).apply {
                    configure(MediaFormat.createAudioFormat(MIME, SAMPLE_RATE, CHANNELS).apply {
                        setByteBuffer("csd-0", ByteBuffer.wrap(config.csd))
                        setInteger(MediaFormat.KEY_AAC_PROFILE, config.profile)
                        setInteger(MediaFormat.KEY_IS_ADTS, 0)
                    }, null, null, 0)
                    start()
                }
            }
            var out = createTrack(SAMPLE_RATE, CHANNELS, config.music)
            framesWritten = 0
            track = out
            applyVolume()
            out.play()

            val info = MediaCodec.BufferInfo()
            while (!Thread.currentThread().isInterrupted) {
                if (flushRequested.getAndSet(false)) {
                    // Pausa o salto nel brano: via anche quello che AudioTrack ha già in buffer,
                    // altrimenti si sentirebbe ancora mezzo secondo del punto precedente.
                    out.pause()
                    out.flush()
                    framesWritten = 0   // flush azzera anche playbackHeadPosition
                    codec?.flush()
                    out.play()
                }
                val polled = queue.poll(POLL_MS, TimeUnit.MILLISECONDS)
                val frame = if (polled != null && waitUntilDue(polled, out)) polled.data else null
                if (codec == null) {
                    if (frame != null && frame.isNotEmpty()) {
                        val n = out.write(frame, 0, frame.size, AudioTrack.WRITE_BLOCKING)
                        if (n > 0) framesWritten += n / (2 * out.channelCount)
                    }
                    continue
                }
                if (frame != null) {
                    val index = codec.dequeueInputBuffer(INPUT_TIMEOUT_US)
                    if (index >= 0) {
                        val buf = codec.getInputBuffer(index)
                        if (buf != null && frame.size <= buf.capacity()) {
                            buf.clear()
                            buf.put(frame)
                            codec.queueInputBuffer(index, 0, frame.size, 0, 0)
                        } else {
                            codec.queueInputBuffer(index, 0, 0, 0, 0)
                        }
                    }
                }
                while (true) {
                    val outIndex = codec.dequeueOutputBuffer(info, 0)
                    if (outIndex >= 0) {
                        val pcm = codec.getOutputBuffer(outIndex)
                        if (pcm != null && info.size > 0) {
                            pcm.position(info.offset)
                            pcm.limit(info.offset + info.size)
                            val n = out.write(pcm, info.size, AudioTrack.WRITE_BLOCKING)
                            if (n > 0) framesWritten += n / (2 * out.channelCount)
                        }
                        codec.releaseOutputBuffer(outIndex, false)
                    } else if (outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        // Alcuni decoder dichiarano il formato reale solo qui: se differisce
                        // da quello previsto, l'AudioTrack va ricreata per non suonare accelerata.
                        val f = codec.outputFormat
                        val rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        val ch = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                        if (rate != out.sampleRate || ch != out.channelCount) {
                            out.release()
                            out = createTrack(rate, ch, config.music)
                            framesWritten = 0
                            track = out
                            applyVolume()
                            out.play()
                        }
                    } else {
                        break
                    }
                }
            }
        } catch (e: InterruptedException) {
            // fine dello stream
        } catch (e: Exception) {
            Log.e(TAG, "Pipeline audio interrotta: ${e.javaClass.simpleName}")
        } finally {
            track?.let {
                Log.i(TAG, "Audio: ${it.sampleRate} Hz, underrun ${it.underrunCount}, pacchetti scartati $dropped")
                try {
                    it.pause()
                    it.flush()
                    it.release()
                } catch (_: Exception) {
                }
            }
            track = null
            try {
                codec?.stop()
            } catch (_: Exception) {
            }
            codec?.release()
        }
    }

    private fun createTrack(rate: Int, channels: Int, music: Boolean): AudioTrack {
        val mask = if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        // Duplicazione: il doppio del minimo, compromesso tra latenza e crepitii sui SoC lenti.
        // Musica: quattro volte, la latenza non conta e la continuità sì.
        val factor = if (music) 4 else 2
        val size = if (min > 0) min * factor else rate * channels * 2 / 10
        return AudioTrack.Builder()
            .setAudioAttributes(attributes)
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(rate)
                    .setChannelMask(mask)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build()
            )
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(size)
            .setPerformanceMode(
                if (music) AudioTrack.PERFORMANCE_MODE_NONE else AudioTrack.PERFORMANCE_MODE_LOW_LATENCY
            )
            .build()
    }

    fun setScanMuted(muted: Boolean) {
        scanMuted = muted
        applyVolume()
    }

    private fun applyVolume() {
        val v = if (scanMuted) 0f else gain * focusFactor
        try {
            track?.setVolume(v)
        } catch (_: IllegalStateException) {
        }
    }

    // ------------------------------------------------------------ audio focus

    private fun requestFocusLocked() {
        if (focusRequest != null) return
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener { change ->
                focusFactor = when (change) {
                    AudioManager.AUDIOFOCUS_GAIN -> 1f
                    AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> DUCK_FACTOR
                    else -> 0f
                }
                applyVolume()
            }
            .build()
        focusFactor = if (audioManager?.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) 1f else 0f
        focusRequest = request
    }

    private fun abandonFocusLocked() {
        focusRequest?.let { audioManager?.abandonAudioFocusRequest(it) }
        focusRequest = null
        focusFactor = 1f
    }

    private fun stopWorkerLocked() {
        worker?.let {
            it.interrupt()
            it.join(JOIN_TIMEOUT_MS)
        }
        worker = null
    }

    companion object {
        private const val TAG = "iRPlay_Audio"
        private const val MIME = MediaFormat.MIMETYPE_AUDIO_AAC
        private const val SAMPLE_RATE = 44100
        private const val CHANNELS = 2
        private const val CT_PCM = 1
        private const val CT_AAC_LC = 4
        private const val CT_AAC_ELD = 8
        private val AAC_ELD_CSD = byteArrayOf(0xF8.toByte(), 0xE8.toByte(), 0x50, 0x00)
        private val AAC_LC_CSD = byteArrayOf(0x12, 0x10)   // LC, 44,1 kHz, stereo
        private const val QUEUE_CAPACITY = 1024
        private const val BACKLOG_MIRRORING = 10           // 10 frame ELD da 480 campioni = ~109 ms
        private const val BACKLOG_MUSIC = 512              // 512 pacchetti ALAC da 352 campioni = ~4 s
        private const val POLL_MS = 20L
        private const val MAX_WAIT_STEP_MS = 10L
        private const val INPUT_TIMEOUT_US = 10_000L
        private const val JOIN_TIMEOUT_MS = 500L
        private const val MIN_DB = -30f
        private const val DUCK_FACTOR = 0.2f
    }
}
