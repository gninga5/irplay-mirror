/*
 * now_playing.h
 * Scopo: interpretare le informazioni sul brano che iOS manda durante l'audio AirPlay
 * (SET_PARAMETER), per la schermata "In riproduzione".
 *
 *  - application/x-dmap-tagged: DMAP (Digital Media Access Protocol, quello di iTunes).
 *    Ogni elemento è  tag (4 caratteri) | lunghezza (4 byte big-endian) | dati.
 *    Il contenitore "mlit" racchiude "minm" (titolo), "asar" (artista), "asal" (album).
 *  - text/parameters "progress: inizio/attuale/fine": tre timestamp RTP a 44,1 kHz.
 *
 * Input di rete: ogni lunghezza è controllata, nessuna ricorsione oltre il primo contenitore,
 * stringhe troncate a una dimensione massima.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace irplay::nowplaying {

    struct Metadata {
        std::string title, artist, album;   // UTF-8 come inviato da iOS, eventualmente troncato
    };

    /** false se il corpo non è un DMAP valido. I campi assenti restano vuoti. */
    bool parseDmap(const uint8_t* data, size_t len, Metadata& out);

    /** "progress: a/b/c" -> posizione e durata in millisecondi. false se il formato non è valido. */
    bool parseProgress(const uint8_t* data, size_t len, int64_t& positionMs, int64_t& durationMs);

    /** "progress: a/b/c" -> i tre timestamp RTP (inizio, posizione secondo iOS, fine). */
    bool parseProgressRtp(const uint8_t* data, size_t len, uint32_t& start, uint32_t& current, uint32_t& end);

    /** Posizione e durata in ms da timestamp RTP; false se current non sta tra start e end. */
    bool progressMs(uint32_t start, uint32_t current, uint32_t end, int64_t& positionMs, int64_t& durationMs);

} // namespace irplay::nowplaying
