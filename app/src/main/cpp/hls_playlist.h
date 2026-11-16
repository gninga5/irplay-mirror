/*
 * hls_playlist.h
 * Scopo: controllo e riscrittura delle playlist HLS (RFC 8216) ricevute per l'AirPlay video.
 *
 * Con YouTube l'iPhone non passa un indirizzo raggiungibile dalla TV ma "mlhls://localhost/
 * master.m3u8": le playlist esistono solo sull'iPhone e arrivano una alla volta (FCUP).
 * Qui:
 *  - nella master playlist si cercano gli URI delle media playlist con il prefisso remoto e
 *    si sostituisce il prefisso con kLocalPrefix, che il lettore di Kotlin serve dalla memoria;
 *  - nelle media playlist di YouTube si espande il tag #YT-EXT-CONDENSED-URL, con cui ogni
 *    segmento è scritto in forma abbreviata ("s/<v1>/<v2>/...") invece dell'URL completo.
 * I segmenti video restano indirizzi https di YouTube, scaricati direttamente dal lettore.
 *
 * Il testo arriva dalla rete: lo esamina Rust (rust/src/hls.rs), con limiti espliciti su
 * numero di URI, lunghezza delle righe e dimensione del risultato.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace irplay::hls {

    /** Schema con cui il lettore di Kotlin riconosce le playlist servite dalla memoria. */
    inline constexpr std::string_view kLocalPrefix = "irplay-hls://local";
    inline constexpr std::string_view kLocalMaster = "irplay-hls://local/master.m3u8";

    // Limiti (URI, playlist, dimensione del risultato): in rust/src/hls.rs.

    /** Testo che inizia con "#EXTM3U", senza byte NUL. */
    bool isPlaylist(std::string_view text);

    /**
     * URL http o https assoluto, ASCII stampabile senza spazi, al massimo 8 KB, con un host.
     * Altri schemi (file:, content:, javascript: ...) sono rifiutati: il lettore non deve
     * poter aprire file locali della TV su richiesta di un dispositivo della rete.
     */
    bool isSafeHttpUrl(std::string_view url);

    /** Prefisso remoto di una location HLS: "mlhls://localhost/master.m3u8" -> "mlhls://localhost". */
    std::optional<std::string> remotePrefixOf(std::string_view location);

    struct Master {
        std::string text;                     // playlist riscritta per il lettore
        std::vector<std::string> mediaUris;   // URI remoti da chiedere all'iPhone, senza doppioni
    };

    /**
     * Riscrive la master playlist. Se il testo è già una media playlist (contiene #EXTINF)
     * viene restituito com'è, espanso, con mediaUris vuoto. nullopt se non è valido.
     */
    std::optional<Master> rewriteMaster(std::string_view text, std::string_view remotePrefix);

    /** URI locale corrispondente a un URI remoto che inizia con remotePrefix. */
    std::string toLocal(std::string_view remoteUri, std::string_view remotePrefix);

    /** Espande #YT-EXT-CONDENSED-URL; senza il tag restituisce il testo invariato. nullopt se il tag è malformato. */
    std::optional<std::string> expandCondensed(std::string_view media);

} // namespace irplay::hls
