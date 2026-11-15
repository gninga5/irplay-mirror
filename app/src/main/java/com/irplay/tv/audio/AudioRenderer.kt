/*
 * AudioRenderer.kt
 * Scopo: decodifica dell'audio del mirroring (AAC-ELD, 44,1 kHz, stereo, 480 campioni
 * per frame) e riproduzione con AudioTrack a bassa latenza.
 *
 * Perché così:
 *  - MediaCodec "audio/mp4a-latm" con csd-0 = AudioSpecificConfig: il decoder AAC
 *    software di Android (presente su tutte le TV da Android 10) supporta l'ELD.
 *    I byte F8 E8 50 00 descrivono ELD, 44,1 kHz, 2 canali, 480 campioni per frame.
 *  - Un thread dedicato con coda limitata: i pacchetti arrivano dal thread di rete,
 *    che non deve mai attendere il decoder. Se la coda si riempie si scartano i
 *    pacchetti più vecchi: meglio un piccolo salto che una latenza che cresce.
 *  - AudioTrack in MODE_STREAM con scrittura bloccante: è la scrittura stessa a dare il
 *    ritmo alla riproduzione. PERFORMANCE_MODE_LOW_LATENCY chiede il percorso audio
 *    veloce, dove la TV lo offre.
 *  - Audio focus: richiesto all'avvio dello stream, rilasciato alla fine. Se un'altra app
 *    prende il focus, l'audio dell'iPhone viene silenziato invece di sovrapporsi.
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
import android.util.Log
import com.irplay.tv.core.NativeBridge
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.math.pow

class AudioRenderer(context: Context) : NativeBridge.AudioSink {

    private val audioManager = context.getSystemService(AudioManager::class.java)
    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
        .build()
    private val queue = LinkedBlockingQueue<ByteArray>(QUEUE_CAPACITY)
    private val lock = Any()

    private var worker: Thread? = null              // protetto da lock
    private var focusRequest: AudioFocusRequest? = null   // protetto da lock
    @Volatile private var track: AudioTrack? = null
    @Volatile private var gain = 1f
    @Volatile private var focusFactor = 1f

    override fun onAudioStarted(codecType: Int) {
        val config = when (codecType) {
            CT_AAC_ELD -> Config(AAC_ELD_CSD, MediaCodecInfo.CodecProfileLevel.AACObjectELD)
            CT_AAC_LC -> Config(AAC_LC_CSD, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            else -> {
                Log.w(TAG, "Formato audio non supportato (ct=$codecType): solo AAC-ELD e AAC-LC")
                return
            }
        }
        synchronized(lock) {
            stopWorkerLocked()
            queue.clear()
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
     * è un po' più veloce, la coda cresce lentamente e con lei il ritardo. Oltre MAX_BACKLOG
     * frame (~100 ms) si scartano i più vecchi, così la latenza resta costante.
     */
    override fun onAudioFrame(data: ByteArray) {
        while (queue.size >= MAX_BACKLOG) queue.poll()
        queue.offer(data)
    }

    override fun onVolume(db: Float) {
        gain = if (db <= MIN_DB) 0f else 10f.pow(db / 20f)
        applyVolume()
    }

    fun release() = onAudioStopped()

    // ------------------------------------------------------------ thread "iRPlay-Audio"

    private class Config(val csd: ByteArray, val profile: Int)

    private fun run(config: Config) {
        var codec: MediaCodec? = null
        try {
            codec = MediaCodec.createDecoderByType(MIME)
            val format = MediaFormat.createAudioFormat(MIME, SAMPLE_RATE, CHANNELS).apply {
                setByteBuffer("csd-0", ByteBuffer.wrap(config.csd))
                setInteger(MediaFormat.KEY_AAC_PROFILE, config.profile)
                setInteger(MediaFormat.KEY_IS_ADTS, 0)
            }
            codec.configure(format, null, null, 0)
            codec.start()
            var out = createTrack(SAMPLE_RATE, CHANNELS)
            track = out
            applyVolume()
            out.play()

            val info = MediaCodec.BufferInfo()
            while (!Thread.currentThread().isInterrupted) {
                val frame = queue.poll(POLL_MS, TimeUnit.MILLISECONDS)
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
                            out.write(pcm, info.size, AudioTrack.WRITE_BLOCKING)
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
                            out = createTrack(rate, ch)
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

    private fun createTrack(rate: Int, channels: Int): AudioTrack {
        val mask = if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        // Il doppio del minimo: con il minimo esatto, i SoC TV più lenti vanno in underrun
        // (crepitii) quando il decoder video occupa la CPU.
        val size = if (min > 0) min * 2 else rate * channels * 2 / 10
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
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
    }

    private fun applyVolume() {
        val v = gain * focusFactor
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
        private const val CT_AAC_LC = 4
        private const val CT_AAC_ELD = 8
        private val AAC_ELD_CSD = byteArrayOf(0xF8.toByte(), 0xE8.toByte(), 0x50, 0x00)
        private val AAC_LC_CSD = byteArrayOf(0x12, 0x10)   // LC, 44,1 kHz, stereo
        private const val QUEUE_CAPACITY = 64
        private const val MAX_BACKLOG = 10                 // 10 frame ELD da 480 campioni = ~109 ms
        private const val POLL_MS = 20L
        private const val INPUT_TIMEOUT_US = 10_000L
        private const val JOIN_TIMEOUT_MS = 500L
        private const val MIN_DB = -30f
        private const val DUCK_FACTOR = 0.2f
    }
}
