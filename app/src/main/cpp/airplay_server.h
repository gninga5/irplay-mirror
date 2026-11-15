/*
 * airplay_server.h
 * Scopo: server TCP che riceve le richieste RTSP/HTTP del protocollo AirPlay
 * (porta annunciata via mDNS in _airplay._tcp e _raop._tcp).
 * Standard: RTSP/1.0 (RFC 2326) per i metodi della sessione, HTTP/1.1 per
 * GET /info e POST /pair-*, /fp-setup, sulla stessa connessione TCP.
 *
 * Flusso di una sessione di mirroring (una connessione RTSP):
 *   GET /info -> POST /pair-setup -> POST /pair-verify (x2) -> POST /fp-setup (x2)
 *   -> SETUP (chiavi, timing) -> RECORD -> SETUP (stream 110 video) [-> SETUP (stream 96 audio)]
 *   -> POST /feedback ogni 2 s ... -> TEARDOWN
 * Lo stato (pairing, FairPlay, sessione) appartiene alla connessione: quando si chiude,
 * tutto viene distrutto e le chiavi azzerate.
 *
 * Una sola sessione di mirroring alla volta: un secondo iPhone riceve 453 finché il
 * primo non si disconnette (la TV ha un solo decoder video hardware disponibile).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include "airplay_crypto.h"
#include "airplay_info.h"
#include "session_sink.h"

namespace irplay {

    struct RtspRequest;

    class AirPlayServer {
    public:
        explicit AirPlayServer(std::shared_ptr<SessionSink> sink) : sink_(std::move(sink)) {}
        ~AirPlayServer();
        AirPlayServer(const AirPlayServer&) = delete;
        AirPlayServer& operator=(const AirPlayServer&) = delete;

        /** Mette il server in ascolto su preferredPort; se è occupata ripiega su una porta
         *  assegnata dal sistema. Restituisce la porta reale, oppure -1 in caso di errore
         *  (anche se info non è valido o la chiave non corrisponde a info.publicKey). */
        int start(uint16_t preferredPort, const ReceiverInfo& info, const uint8_t seed[32]);

        /** Chiude il socket di ascolto e tutte le connessioni, e attende la fine dei thread. Idempotente. */
        void stop();

        /** Interrompe la sessione di mirroring attiva (tasto BACK). iOS vede la connessione
         *  chiudersi e mostra "duplicazione interrotta". */
        void stopActiveSession();

    private:
        struct Client {
            int fd = -1;                  // protetto da mutex_: chiuso solo dal suo thread
            bool done = false;            // protetto da mutex_
            bool stopRequested = false;   // protetto da mutex_
            std::thread thread;
        };
        struct ConnState;   // stato per connessione, definito in airplay_server.cpp

        void acceptLoop();
        void handleConnection(Client* client, std::string peer, sockaddr_storage peerAddr);
        void reapFinishedLocked();

        bool dispatch(ConnState& st, const RtspRequest& req);
        bool handleSetup(ConnState& st, const RtspRequest& req);
        bool handleTeardown(ConnState& st, const RtspRequest& req);
        void endSession(ConnState& st, EndReason reason);

        std::shared_ptr<SessionSink> sink_;
        std::unique_ptr<crypto::Ed25519Identity> identity_;
        // Scritti solo in start(), prima che partano i thread: poi di sola lettura, senza mutex.
        InfoPlists info_;

        int listenFd_ = -1;
        std::atomic<bool> running_{false};
        std::thread acceptThread_;
        std::mutex mutex_;
        std::vector<std::unique_ptr<Client>> clients_;
        Client* sessionOwner_ = nullptr;   // protetto da mutex_
        std::atomic<float> volumeDb_{0.0f};
    };

} // namespace irplay
