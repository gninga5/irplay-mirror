/*
 * server_conn.h
 * Scopo: strutture interne di AirPlayServer condivise tra airplay_server.cpp (mirroring,
 * audio, PIN) e airplay_media.cpp (AirPlay video). Non va incluso da altri file.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "airplay_server.h"
#include "media_streams.h"
#include "rtsp_message.h"

namespace irplay {

    /** Stato di una connessione TCP: muore con lei, chiavi comprese. */
    struct AirPlayServer::ConnState {
        Client* client = nullptr;
        int fd = -1;
        sockaddr_in6 peer{};
        crypto::PairingSession pairing;
        crypto::FairPlay fairplay;
        std::unique_ptr<Session> session;
        // Dopo POST /reverse la connessione cambia verso (PTTH/1.0): siamo noi a inviare
        // richieste (POST /event) e iOS risponde. Da quel momento si leggono solo risposte.
        bool reverse = false;
        bool remoteSent = false;   // DACP-ID/Active-Remote già passati a Kotlin per questa connessione
    };

    /**
     * Riproduzione AirPlay video in corso (un solo video alla volta). Protetta da mediaMutex_.
     *
     * Due forme di /play:
     *  - URL http(s) diretto (Safari, molte app): lo apre ExoPlayer, senza altro lavoro qui;
     *  - HLS via iPhone ("mlhls://localhost/master.m3u8", YouTube): le playlist non sono
     *    raggiungibili dalla TV. Si chiedono una alla volta a iOS con richieste FCUP sul
     *    canale inverso; iOS risponde con POST /action. Quando ci sono tutte passano a
     *    Kotlin, che le serve a ExoPlayer dalla memoria (nessuna porta HTTP aperta sulla TV).
     */
    struct AirPlayServer::MediaSession {
        std::string sessionId;            // X-Apple-Session-ID dell'iPhone che ha chiesto /play
        std::string uuid;                 // elemento (uuid di /play o di playlistInsert): iOS lo cerca in /playback-info
        bool announceCurrent = false;     // playlistInsert: avviso currentItemChanged quando le playlist sono arrivate
        double startSec = 0;              // posizione iniziale in secondi (plist binario)
        double startFraction = -1;        // oppure frazione 0..1 della durata (forma testuale)
        int64_t lastSeenMs = 0;           // ultima richiesta con questo sessionId (watchdog)

        // Solo HLS via FCUP.
        std::string remotePrefix;         // es. "mlhls://localhost"
        std::string pendingUrl;           // URL chiesto con l'ultima FCUP, atteso in POST /action
        uint32_t requestId = 0;
        bool fetching = false;
        bool haveMaster = false;
        std::string master;               // già riscritta con il prefisso locale
        std::vector<std::string> mediaUris;
        std::vector<std::string> mediaPlaylists;
        size_t next = 0;                  // indice della prossima media playlist da chiedere
        size_t totalBytes = 0;

        std::string lastEvent;            // ultimo stato inviato a iOS con POST /event
    };

    /** Risposta a GET /server-info (plist XML), costruita una volta in start(). */
    std::vector<uint8_t> buildServerInfo(const ReceiverInfo& info);

} // namespace irplay
