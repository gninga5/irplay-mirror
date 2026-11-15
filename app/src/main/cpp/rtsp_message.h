/*
 * rtsp_message.h
 * Scopo: lettura e scrittura dei messaggi RTSP/1.0 e HTTP/1.1 che iOS usa sulla stessa
 * porta TCP. Separato da airplay_server.cpp perché lo usano sia la sessione di mirroring
 * sia l'AirPlay video (airplay_media.cpp).
 *
 * Il parser tratta ogni byte come ostile: limiti a intestazioni e corpo, un solo
 * Content-Length, niente Transfer-Encoding (request smuggling).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace irplay {

    struct RtspRequest {
        std::string method, uri, protocol;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<uint8_t> body;

        /** Primo header con quel nome (senza distinzione tra maiuscole e minuscole), oppure nullptr. */
        const std::string* header(std::string_view name) const;
    };

    using Headers = std::vector<std::pair<std::string, std::string>>;

    namespace rtsp {

        constexpr size_t kMaxHeaderBytes = 16 * 1024;
        constexpr size_t kMaxBodyBytes   = 64 * 1024;          // fp-setup = 164 B, i plist di SETUP pesano pochi KB
        constexpr size_t kMaxArtworkBytes = 2 * 1024 * 1024;   // SET_PARAMETER con la copertina dell'album
        // POST /action porta una playlist HLS intera (YouTube: fino a qualche centinaio di KB
        // per un video lungo senza URI condensati). 4 MB lasciano margine senza permettere
        // a un client di occupare la RAM di una chiavetta TV.
        constexpr size_t kMaxPlaylistBytes = 4 * 1024 * 1024;
        constexpr size_t kMaxMessageBytes = kMaxHeaderBytes + kMaxPlaylistBytes + 4;

        bool iequals(std::string_view a, std::string_view b);
        std::string_view trim(std::string_view s);
        bool isDigits(std::string_view s, size_t maxLen);

        /** 1: richiesta completa estratta da buf; 0: servono altri byte; -1: malformata o oltre i limiti. */
        int parseRequest(std::vector<uint8_t>& buf, RtspRequest& req);

        /**
         * Canale inverso (PTTH): qui iOS invia solo risposte HTTP alle nostre POST /event.
         * Le risposte vengono scartate: 1 se una risposta completa è stata rimossa da buf,
         * 0 se servono altri byte, -1 se il flusso non è una risposta HTTP valida.
         */
        int discardResponse(std::vector<uint8_t>& buf);

        /** Invio bloccante di tutti i byte (socket con SO_SNDTIMEO non impostato). */
        bool sendAll(int fd, const void* data, size_t len);

        /**
         * Invio con un tempo massimo: per i messaggi spontanei verso iOS (eventi sul canale
         * inverso) che partono da thread che non devono restare bloccati, come quello di Kotlin.
         */
        bool sendAllTimed(int fd, const void* data, size_t len, int timeoutMs);

        bool sendResponse(int fd, const RtspRequest* req, int code, const char* reason,
                          const Headers& extra = {}, const std::vector<uint8_t>& body = {});

    } // namespace rtsp

} // namespace irplay
