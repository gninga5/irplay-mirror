/*
 * Lyrics.kt
 * Scopo: testi delle canzoni per la schermata "In riproduzione".
 *
 * AirPlay non trasmette i testi (iOS invia solo titolo, artista, album, copertina e
 * avanzamento), quindi li si cerca online, in quest'ordine, fermandosi al primo che risponde:
 *  1. lrcmux.dev  - aggregatore (Deezer, KuGou, YouTube Music); testi sincronizzati riga per riga
 *  2. Karalyr     - archivio aperto di testi sincronizzati parola per parola (API stile LRCLIB)
 *  3. lyrics.ovh  - solo testo, senza tempi
 *
 * Privacy: è l'unica funzione dell'app che contatta servizi esterni; invia soltanto titolo,
 * artista, album e durata del brano, nessun dato del dispositivo o dell'utente. È indicato nella
 * schermata Licenze e si può disattivare dalla home.
 *
 * Sicurezza: solo HTTPS verso tre host fissi, nessun redirect, timeout brevi, risposta letta fino
 * a 512 KB, testo ripulito da caratteri di controllo e limitato in righe e lunghezza prima di
 * arrivare alla UI.
 */
package com.irplay.tv.core

import android.net.Uri
import android.util.Log
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import javax.net.ssl.HttpsURLConnection
import org.json.JSONObject

object Lyrics {

    sealed interface Result {
        /** Righe con il tempo di inizio: la UI evidenzia quella in corso. */
        data class Synced(val lines: List<Line>) : Result
        /** Solo testo: la UI lo fa scorrere in proporzione all'avanzamento del brano. */
        data class Plain(val lines: List<String>) : Result
        data object Instrumental : Result
        data object NotFound : Result
    }

    data class Line(val timeMs: Long, val text: String)

    private const val TAG = "iRPlay_Lyrics"
    private const val HOST_LRCMUX = "api.lrcmux.dev"
    private const val HOST_KARALYR = "www.karalyr.com"
    private const val HOST_LYRICS_OVH = "api.lyrics.ovh"
    private const val USER_AGENT = "iRPlay-Mirror/1.1 (https://github.com/gninga5/irplay-mirror)"
    private const val MAX_BYTES = 512 * 1024
    private const val MAX_LINES = 400
    private const val MAX_LINE_CHARS = 160

    /** Esito di una richiesta: testo trovato, assente (si passa al servizio successivo) o errore. */
    private sealed interface Outcome {
        data class Found(val result: Result) : Outcome
        data object Missing : Outcome
        data object Failed : Outcome
    }

    // Cache dei brani già cercati: in pausa/ripresa e cambi di brano rapidi non si ripete la richiesta.
    private val cache = object : LinkedHashMap<String, Result>(32, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Result>?) = size > 32
    }

    /** Da chiamare fuori dal main thread. Non lancia eccezioni. */
    fun fetch(title: String, artist: String, album: String, durationMs: Long): Result {
        if (title.isBlank() || artist.isBlank()) return Result.NotFound
        val key = "$artist\u0000$title\u0000$album\u0000${durationMs / 2000}"
        synchronized(cache) { cache[key]?.let { return it } }

        var anyFailed = false
        val providers = listOf(
            { fromLrcmux(title, artist, album, durationMs) },
            { fromKaralyr(title, artist, album, durationMs) },
            { fromLyricsOvh(title, artist) },
        )
        for (provider in providers) {
            when (val o = safely(provider)) {
                is Outcome.Found -> {
                    synchronized(cache) { cache[key] = o.result }
                    return o.result
                }
                Outcome.Missing -> Unit
                Outcome.Failed -> anyFailed = true
            }
        }
        // "Non trovato" si ricorda solo se tutti hanno risposto: dopo un errore di rete si riproverà.
        if (!anyFailed) synchronized(cache) { cache[key] = Result.NotFound }
        return Result.NotFound
    }

    private inline fun safely(provider: () -> Outcome): Outcome = try {
        provider()
    } catch (e: Exception) {
        Log.w(TAG, "Servizio testi non disponibile: ${e.javaClass.simpleName}")
        Outcome.Failed
    }

    /** lrcmux: prima il testo sincronizzato (LRC); se esiste solo quello semplice risponde 400 e lo si chiede in txt. */
    private fun fromLrcmux(title: String, artist: String, album: String, durationMs: Long): Outcome {
        fun uri(format: String) = Uri.Builder().scheme("https").authority(HOST_LRCMUX).path("/get")
            .appendQueryParameter("artist", artist)
            .appendQueryParameter("title", title)
            .apply {
                if (album.isNotBlank()) appendQueryParameter("album", album)
                if (durationMs > 0) appendQueryParameter("duration", (durationMs / 1000).toString())
            }
            .appendQueryParameter("level", "line")
            .appendQueryParameter("format", format)
            .build()
        val lrc = request(uri("lrc"), acceptBadRequest = true)
        when {
            lrc == null -> return Outcome.Missing
            lrc != BAD_REQUEST -> {
                val lines = parseLrc(lrc)
                return if (lines.isNotEmpty()) Outcome.Found(Result.Synced(lines)) else Outcome.Missing
            }
        }
        val txt = request(uri("txt")) ?: return Outcome.Missing
        return plain(txt)
    }

    /** Karalyr: risposta nel formato di LRCLIB; i tempi parola per parola vengono rimossi da parseLrc. */
    private fun fromKaralyr(title: String, artist: String, album: String, durationMs: Long): Outcome {
        val uri = Uri.Builder().scheme("https").authority(HOST_KARALYR).path("/api/get")
            .appendQueryParameter("track_name", title)
            .appendQueryParameter("artist_name", artist)
            .apply {
                if (album.isNotBlank()) appendQueryParameter("album_name", album)
                if (durationMs > 0) appendQueryParameter("duration", (durationMs / 1000).toString())
            }.build()
        val body = request(uri) ?: return Outcome.Missing
        val o = JSONObject(body)
        if (o.optBoolean("instrumental", false)) return Outcome.Found(Result.Instrumental)
        if (!o.isNull("syncedLyrics")) {
            val lines = parseLrc(o.optString("syncedLyrics"))
            if (lines.isNotEmpty()) return Outcome.Found(Result.Synced(lines))
        }
        return if (o.isNull("plainLyrics")) Outcome.Missing else plain(o.optString("plainLyrics"))
    }

    /** lyrics.ovh: solo testo. L'artista e il titolo sono segmenti del percorso (codificati da Uri). */
    private fun fromLyricsOvh(title: String, artist: String): Outcome {
        val uri = Uri.Builder().scheme("https").authority(HOST_LYRICS_OVH)
            .appendPath("v1").appendPath(artist).appendPath(title).build()
        val body = request(uri) ?: return Outcome.Missing
        val o = JSONObject(body)
        if (o.isNull("lyrics")) return Outcome.Missing
        // A volte la prima riga è un'intestazione del sito ("Paroles de la chanson ... par ...").
        val text = o.optString("lyrics").lineSequence()
            .dropWhile { it.isBlank() || it.startsWith("Paroles de la chanson") }
            .joinToString("\n")
        return plain(text)
    }

    private fun plain(text: String): Outcome {
        val lines = text.lineSequence().map(::clean).take(MAX_LINES).toList()
            .dropWhile { it.isEmpty() }.dropLastWhile { it.isEmpty() }
        return if (lines.isNotEmpty()) Outcome.Found(Result.Plain(lines)) else Outcome.Missing
    }

    private const val BAD_REQUEST = "\u0000400"

    /**
     * GET HTTPS; null se la risorsa non esiste (404). Con acceptBadRequest un 400 restituisce
     * BAD_REQUEST invece di un errore (lrcmux lo usa per "c'è solo il testo semplice").
     */
    private fun request(uri: Uri, acceptBadRequest: Boolean = false): String? {
        val conn = URL(uri.toString()).openConnection() as HttpsURLConnection
        try {
            conn.connectTimeout = 4000
            conn.readTimeout = 6000
            conn.instanceFollowRedirects = false   // niente redirect verso altri host
            conn.setRequestProperty("User-Agent", USER_AGENT)
            conn.setRequestProperty("Accept", "application/json")
            when (conn.responseCode) {
                HttpURLConnection.HTTP_OK -> Unit
                HttpURLConnection.HTTP_NOT_FOUND -> return null
                HttpURLConnection.HTTP_BAD_REQUEST -> if (acceptBadRequest) return BAD_REQUEST else
                    throw IllegalStateException("HTTP 400")
                else -> throw IllegalStateException("HTTP ${conn.responseCode}")
            }
            conn.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                val buf = ByteArray(8192)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    if (out.size() + n > MAX_BYTES) throw IllegalStateException("risposta troppo grande")
                    out.write(buf, 0, n)
                }
                return out.toString(Charsets.UTF_8.name())
            }
        } finally {
            conn.disconnect()
        }
    }

    // "[01:23.45] testo" (anche più tempi sulla stessa riga: "[00:10.00][01:10.00] ritornello")
    private val tag = Regex("""^\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?]""")
    // LRC esteso (Karalyr, Deezer): "<00:01.74>" prima di ogni parola; qui interessano solo le righe.
    private val wordTag = Regex("""<\d{1,3}:\d{2}(?:[.:]\d{1,3})?>""")

    internal fun parseLrc(lrc: String): List<Line> {
        val out = ArrayList<Line>()
        for (raw in lrc.lineSequence()) {
            var rest = raw.trim()
            val times = ArrayList<Long>()
            while (true) {
                val m = tag.find(rest) ?: break
                val min = m.groupValues[1].toLong()
                val sec = m.groupValues[2].toLong()
                val frac = m.groupValues[3]
                val ms = when (frac.length) { 0 -> 0L; 1 -> frac.toLong() * 100; 2 -> frac.toLong() * 10; else -> frac.toLong() }
                times += (min * 60 + sec) * 1000 + ms
                rest = rest.substring(m.range.last + 1)
            }
            val text = clean(rest.replace(wordTag, "").replace(Regex("\\s{2,}"), " "))
            for (t in times) out += Line(t, text)
            if (out.size >= MAX_LINES) break
        }
        return out.sortedBy { it.timeMs }
    }

    private fun clean(s: String): String =
        s.filter { !Character.isISOControl(it) && Character.getType(it) != Character.FORMAT.toInt() }
            .trim().take(MAX_LINE_CHARS)
}
