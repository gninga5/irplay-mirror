/*
 * Lyrics.kt
 * Scopo: testi delle canzoni per la schermata "In riproduzione".
 *
 * AirPlay non trasmette i testi (iOS invia solo titolo, artista, album, copertina e
 * avanzamento), quindi li si cerca su LRCLIB (https://lrclib.net), un archivio aperto e
 * gratuito che offre anche testi sincronizzati in formato LRC ("[mm:ss.cc] riga").
 *
 * Privacy: è l'unica funzione dell'app che contatta un servizio esterno. È disattivata di
 * default (AppSettings.lyricsEnabled) e invia soltanto titolo, artista, album e durata del brano,
 * nessun dato del dispositivo o dell'utente.
 *
 * Sicurezza: solo HTTPS verso un host fisso, timeout brevi, risposta letta fino a 512 KB,
 * testo ripulito da caratteri di controllo e limitato in righe e lunghezza prima di arrivare
 * alla UI.
 */
package com.irplay.tv.core

import android.net.Uri
import android.util.Log
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import javax.net.ssl.HttpsURLConnection
import org.json.JSONArray
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
    private const val HOST = "lrclib.net"
    private const val USER_AGENT = "iRPlay-Mirror/1.1 (https://github.com/gninga5/irplay-mirror)"
    private const val MAX_BYTES = 512 * 1024
    private const val MAX_LINES = 400
    private const val MAX_LINE_CHARS = 160
    private const val DURATION_TOLERANCE_S = 3.0

    // Cache dei brani già cercati: in pausa/ripresa e cambi di brano rapidi non si ripete la richiesta.
    private val cache = object : LinkedHashMap<String, Result>(32, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Result>?) = size > 32
    }

    /** Da chiamare fuori dal main thread. Non lancia eccezioni: in caso di errore restituisce NotFound. */
    fun fetch(title: String, artist: String, album: String, durationMs: Long): Result {
        if (title.isBlank() || artist.isBlank()) return Result.NotFound
        val key = "$artist\u0000$title\u0000$album\u0000${durationMs / 2000}"
        synchronized(cache) { cache[key]?.let { return it } }
        val result = try {
            lookup(title, artist, album, durationMs)
        } catch (e: Exception) {
            Log.w(TAG, "Ricerca del testo non riuscita: ${e.javaClass.simpleName}")
            return Result.NotFound   // errore di rete: non si salva in cache, si riproverà
        }
        synchronized(cache) { cache[key] = result }
        return result
    }

    private fun lookup(title: String, artist: String, album: String, durationMs: Long): Result {
        // 1) corrispondenza esatta (con la durata, LRCLIB distingue le versioni dello stesso brano)
        val get = Uri.Builder().scheme("https").authority(HOST).path("/api/get")
            .appendQueryParameter("track_name", title)
            .appendQueryParameter("artist_name", artist)
            .apply {
                if (album.isNotBlank()) appendQueryParameter("album_name", album)
                if (durationMs > 0) appendQueryParameter("duration", (durationMs / 1000).toString())
            }.build()
        request(get)?.let { body -> return parse(JSONObject(body)) }

        // 2) ricerca per titolo e artista, preferendo un risultato con durata compatibile
        val search = Uri.Builder().scheme("https").authority(HOST).path("/api/search")
            .appendQueryParameter("track_name", title)
            .appendQueryParameter("artist_name", artist)
            .build()
        val body = request(search) ?: return Result.NotFound
        val items = JSONArray(body)
        var best: JSONObject? = null
        for (i in 0 until minOf(items.length(), 20)) {
            val item = items.optJSONObject(i) ?: continue
            val durationOk = durationMs <= 0 ||
                kotlin.math.abs(item.optDouble("duration", 0.0) - durationMs / 1000.0) <= DURATION_TOLERANCE_S
            if (!durationOk) continue
            if (best == null || (!best.has("syncedLyrics") || best.isNull("syncedLyrics")) && !item.isNull("syncedLyrics")) {
                best = item
            }
        }
        return best?.let(::parse) ?: Result.NotFound
    }

    /** GET HTTPS; null se la risorsa non esiste (404). */
    private fun request(uri: Uri): String? {
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

    private fun parse(o: JSONObject): Result {
        if (o.optBoolean("instrumental", false)) return Result.Instrumental
        if (!o.isNull("syncedLyrics")) {
            val lines = parseLrc(o.optString("syncedLyrics"))
            if (lines.isNotEmpty()) return Result.Synced(lines)
        }
        if (!o.isNull("plainLyrics")) {
            val lines = o.optString("plainLyrics").lineSequence().map(::clean).take(MAX_LINES).toList()
            if (lines.any { it.isNotEmpty() }) return Result.Plain(lines)
        }
        return Result.NotFound
    }

    // "[01:23.45] testo" (anche più tempi sulla stessa riga: "[00:10.00][01:10.00] ritornello")
    private val tag = Regex("""^\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?]""")

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
            val text = clean(rest)
            for (t in times) out += Line(t, text)
            if (out.size >= MAX_LINES) break
        }
        return out.sortedBy { it.timeMs }
    }

    private fun clean(s: String): String =
        s.filter { !Character.isISOControl(it) && Character.getType(it) != Character.FORMAT.toInt() }
            .trim().take(MAX_LINE_CHARS)
}
