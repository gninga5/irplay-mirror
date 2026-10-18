/*
 * airplay_server.h
 * Scopo: server TCP che riceve le richieste RTSP/HTTP del protocollo AirPlay
 * (porta annunciata via mDNS in _airplay._tcp e _raop._tcp).
 * Standard: RTSP/1.0 (RFC 2326) per i metodi della sessione, HTTP/1.1 per
 * GET /info e POST /pair-*, /fp-setup, sulla stessa connessione TCP.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace icast {

    class AirPlayServer {
    public:
        AirPlayServer() = default;
        ~AirPlayServer();
        AirPlayServer(const AirPlayServer&) = delete;
        AirPlayServer& operator=(const AirPlayServer&) = delete;

        /** Mette il server in ascolto su preferredPort; se è occupata ripiega su una porta
         *  assegnata dal sistema. Restituisce la porta reale, oppure -1 in caso di errore. */
        int start(uint16_t preferredPort);

        /** Chiude il socket di ascolto e tutte le connessioni, e attende la fine dei thread. Idempotente. */
        void stop();

    private:
        struct Client {
            int fd = -1;            // protetto da mutex_: chiuso solo dal suo thread
            bool done = false;      // protetto da mutex_
            std::thread thread;
        };

        void acceptLoop();
        void handleConnection(Client* client, std::string peer);
        void reapFinishedLocked();

        int listenFd_ = -1;
        std::atomic<bool> running_{false};
        std::thread acceptThread_;
        std::mutex mutex_;
        std::vector<std::unique_ptr<Client>> clients_;
    };

} // namespace icast