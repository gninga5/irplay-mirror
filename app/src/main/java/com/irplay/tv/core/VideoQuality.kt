/*
 * VideoQuality.kt
 * Scopo: formato e risoluzione della duplicazione dello schermo, scelti fra quelli che il
 * decoder hardware della TV regge davvero.
 *
 *  - Standard: H.264 a 1080p, la scelta con la latenza più bassa su tutte le TV provate.
 *  - Alta: HEVC a 1080p. Comprime meglio di H.264: a parità di dati l'immagine è più pulita.
 *  - Massima: HEVC in 4K. Più nitido con iPad e Mac, ma alcuni decoder (es. Realtek) non
 *    reggono il 4K a bassa latenza: fotogrammi irregolari e qualche decina di ms in più.
 *
 * Una scelta che il decoder non regge ricade su quella inferiore: la TV non annuncia mai
 * a iOS qualcosa che poi non saprebbe decodificare.
 */
package com.irplay.tv.core

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build

object VideoQuality {

    const val STANDARD = 0
    const val HIGH = 1
    const val MAX = 2

    /** Quello che si annuncia a iOS. */
    data class Mode(val quality: Int, val hevc: Boolean, val width: Int, val height: Int)

    private val standard = Mode(STANDARD, false, 1920, 1080)

    /** Scelte disponibili su questa TV, dalla più bassa alla più alta. */
    fun available(fps: Int): List<Int> = buildList {
        add(STANDARD)
        if (hardwareSupports(MediaFormat.MIMETYPE_VIDEO_HEVC, 1920, 1080, fps)) add(HIGH)
        if (hardwareSupports(MediaFormat.MIMETYPE_VIDEO_HEVC, 3840, 2160, fps)) add(MAX)
    }

    /** La scelta salvata, ridotta a quella più alta che questa TV regge. */
    fun effective(setting: Int, fps: Int): Mode {
        val ok = available(fps)
        val q = ok.filter { it <= setting }.maxOrNull() ?: STANDARD
        return when (q) {
            MAX -> Mode(MAX, true, 3840, 2160)
            HIGH -> Mode(HIGH, true, 1920, 1080)
            else -> standard
        }
    }

    /** Un decoder hardware per mime che dichiara la risoluzione alla frequenza indicata. */
    fun hardwareSupports(mime: String, width: Int, height: Int, fps: Int): Boolean = try {
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            !info.isEncoder && isHardwareDecoder(info) &&
                info.supportedTypes.any { it.equals(mime, ignoreCase = true) } &&
                info.getCapabilitiesForType(mime).videoCapabilities?.areSizeAndRateSupported(width, height, fps.toDouble()) == true
        }
    } catch (e: Exception) {
        false
    }

    /**
     * Da Android 10 lo dice il sistema. Su Android 9 (Fire OS 7) si riconoscono dal nome i
     * decoder software di Android: OMX.google.*, c2.android.* e quelli che dichiarano "sw".
     */
    fun isHardwareDecoder(info: MediaCodecInfo): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return info.isHardwareAccelerated
        val name = info.name.lowercase()
        return !name.startsWith("omx.google.") && !name.startsWith("c2.android.") && !name.contains(".sw.")
    }
}
