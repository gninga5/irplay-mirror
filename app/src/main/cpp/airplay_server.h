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

#include <array>
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
#include "plist_writer.h"
#include "session_sink.h"

namespace irplay {

    struct RtspRequest;

    /** Controllo di accesso con PIN (pair-pin-start / pair-setup-pin). */
    enum class PinMode : int {
        Off = 0,        // chiunque sulla rete può trasmettere
        FirstTime = 1,  // PIN alla prima connessione; poi il dispositivo è riconosciuto dalla sua chiave
        Always = 2,     // PIN a ogni connessione
    };

    struct AccessConfig {
        PinMode pin = PinMode::Off;
        std::vector<std::array<uint8_t, 32>> trusted;   // chiavi Ed25519 dei dispositivi già associati
    };

    class AirPlayServer {
    public:
        // Costruttore definito nel .cpp: media_ è un unique_ptr a un tipo dichiarato solo qui.
        explicit AirPlayServer(std::shared_ptr<SessionSink> sink);
        ~AirPlayServer();
        AirPlayServer(const AirPlayServer&) = delete;
        AirPlayServer& operator=(const AirPlayServer&) = delete;

        /** Mette il server in ascolto su preferredPort; se è occupata ripiega su una porta
         *  assegnata dal sistema. Restituisce la porta reale, oppure -1 in caso di errore
         *  (anche se info non è valido o la chiave non corrisponde a info.publicKey). */
        int start(uint16_t preferredPort, const ReceiverInfo& info, const uint8_t seed[32],
                  const AccessConfig& access = {});

        /** Chiude il socket di ascolto e tutte le connessioni, e attende la fine dei thread. Idempotente. */
        void stop();

        /** Interrompe la sessione di mirroring attiva (tasto BACK). iOS vede la connessione
         *  chiudersi e mostra "duplicazione interrotta". */
        void stopActiveSession();

        /** Stato del lettore AirPlay video (ExoPlayer), comunicato da Kotlin. */
        enum class MediaState : int { Idle = 0, Loading = 1, Playing = 2, Paused = 3, Ended = 4 };

        /** Posizione, durata e stato del video in riproduzione: iOS li legge con GET /playback-info
         *  e i cambi di stato gli vengono notificati sul canale inverso. Non blocca oltre 200 ms. */
        void updateMediaPlayback(double positionSec, double durationSec, float rate, MediaState state);

        /** Video fermato dalla TV (tasto BACK): iOS viene avvisato e la riproduzione chiusa. */
        void stopMedia();

    private:
        struct Client {
            int fd = -1;                  // protetto da mutex_: chiuso solo dal suo thread
            bool done = false;            // protetto da mutex_
            bool stopRequested = false;   // protetto da mutex_
            std::array<uint8_t, 16> addr{};   // indirizzo IPv6 (IPv4 mappato) del dispositivo
            // Chiave Ed25519 autenticata con pair-verify su questa connessione (protetta da mutex_).
            // Identifica il dispositivo anche quando usa indirizzi diversi (IPv4 e IPv6).
            std::array<uint8_t, 32> edKey{};
            bool hasEdKey = false;
            std::thread thread;
        };
        struct ConnState;      // stato per connessione, definito in server_conn.h
        struct MediaSession;   // AirPlay video, definito in server_conn.h

        void acceptLoop();
        void handleConnection(Client* client, std::string peer, sockaddr_storage peerAddr);
        void reapFinishedLocked();

        bool dispatch(ConnState& st, const RtspRequest& req);
        bool handleSetup(ConnState& st, const RtspRequest& req);
        void notifyRemoteControl(ConnState& st, const RtspRequest& req);
        bool handlePinStart(ConnState& st, const RtspRequest& req);
        bool handlePinSetup(ConnState& st, const RtspRequest& req);
        bool clientAllowed(ConnState& st, const uint8_t clientEdKey[32]);
        std::string currentOrNewPinLocked(bool& isNew);
        bool handleTeardown(ConnState& st, const RtspRequest& req);
        void endSession(ConnState& st, EndReason reason);

        // ---- AirPlay video (airplay_media.cpp)
        /** Gestisce le richieste HTTP dell'AirPlay video. false = non è una di queste richieste. */
        bool dispatchMedia(ConnState& st, const RtspRequest& req, bool& keepOpen);
        bool handleReverse(ConnState& st, const RtspRequest& req, const std::string& sid);
        bool handlePlay(ConnState& st, const RtspRequest& req, const std::string& sid);
        void resetMediaPlayback(MediaState state);
        bool startMedia(ConnState& st, const RtspRequest& req, const std::string& sid,
                        const std::string& location, double startSec, double startFraction);
        bool handleAction(ConnState& st, const RtspRequest& req, const std::string& sid);
        bool handlePlaybackInfo(ConnState& st, const RtspRequest& req);
        bool mediaAuthorized(const ConnState& st);
        void notePeerVerified(const ConnState& st);
        /** Chiamata a ogni ciclo del watchdog e alla chiusura di una connessione. */
        void mediaHousekeeping(ConnState* closing);
        bool sendFcupLocked(const std::string& url);
        void sendEventLocked(const char* state);
        bool isActiveReverse(int fd);

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

        // Stato del PIN, condiviso tra connessioni: protetto da pinMutex_.
        // Il limite ai tentativi è globale e non per connessione, altrimenti basterebbe
        // aprire connessioni nuove per provare tutti i 10.000 PIN.
        std::mutex pinMutex_;
        PinMode pinMode_ = PinMode::Off;
        std::vector<std::array<uint8_t, 32>> trusted_;
        std::string pin_;               // PIN attivo, vuoto se nessuno
        int64_t pinExpiresMs_ = 0;
        int pinFailures_ = 0;          // tentativi sbagliati sul PIN attivo
        int lockLevel_ = 0;
        int64_t lockedUntilMs_ = 0;
        // Dispositivi che hanno completato pair-verify (indirizzo, istante): con il PIN attivo
        // solo loro possono inviare un video. Protetto da pinMutex_.
        std::vector<std::pair<std::array<uint8_t, 16>, int64_t>> verifiedPeers_;

        // AirPlay video: protetti da mediaMutex_ (mai preso insieme a mutex_ o pinMutex_).
        std::mutex mediaMutex_;
        std::unique_ptr<MediaSession> media_;
        int reverseFd_ = -1;              // canale inverso per FCUP ed eventi
        std::string reverseSession_;      // X-Apple-Session-ID di chi l'ha aperto
        // Proprietà del lettore impostate da iOS con PUT /setProperty (es. selectedMediaArray,
        // le tracce audio/sottotitoli scelte) e restituite da /getProperty. Valgono per tutta la
        // sessione video, anche attraverso playlistInsert; azzerate da un nuovo /play.
        std::vector<std::pair<std::string, plist::Value>> mediaProps_;
        // Scritti da Kotlin, letti da GET /playback-info: atomici, senza mutex.
        std::atomic<double> mediaPos_{0.0};
        std::atomic<double> mediaDur_{0.0};
        std::atomic<float> mediaRate_{0.0f};
        std::atomic<int> mediaState_{0};
        std::vector<uint8_t> serverInfo_;  // risposta a GET /server-info (XML), costruita in start()
    };

} // namespace irplay
