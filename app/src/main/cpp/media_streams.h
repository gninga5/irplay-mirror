/*
 * media_streams.h
 * Scopo: i canali dati di una sessione di mirroring, aperti con SETUP.
 *  - MirrorStream: TCP, video H.264 cifrato AES-CTR. Pacchetti con header di 128 byte.
 *  - AudioStream:  UDP, RTP con AAC-ELD cifrato AES-CBC, più la porta di controllo RTCP.
 *  - TimingClient: UDP, richieste di timing NTP verso l'iPhone (protocollo AirPlay 1).
 *  - Session:      possiede i tre canali, la chiave di sessione e il timestamp di attività
 *                  usato dal watchdog.
 *
 * Ogni canale accetta dati solo dall'indirizzo IP del client RTSP che ha fatto SETUP:
 * un altro host della LAN non può iniettare video o audio conoscendo la porta.
 * Le porte sono effimere (assegnate dal sistema): niente conflitti con altri servizi della TV.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <netinet/in.h>

#include "airplay_crypto.h"
#include "session_sink.h"

namespace irplay {

    /** Millisecondi da un orologio monotono (non risente dei cambi d'ora della TV). */
    int64_t monotonicMs();

    class MirrorStream {
    public:
        MirrorStream(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16],
                     uint64_t streamConnectionId, std::atomic<int64_t>& activity);
        ~MirrorStream();
        /** Restituisce la porta TCP in ascolto, o -1. */
        int start();
        void stop();
        bool connected() const { return connected_; }

    private:
        void run();
        bool readExact(int fd, uint8_t* buf, size_t len);

        SessionSink& sink_;
        sockaddr_in6 peer_;
        std::unique_ptr<crypto::AesCtr> aes_;
        std::atomic<int64_t>& activity_;
        std::mutex mutex_;
        int listenFd_ = -1;
        int connFd_ = -1;              // protetto da mutex_
        std::atomic<bool> running_{false};
        std::atomic<bool> connected_{false};
        std::thread thread_;
    };

    class AudioStream {
    public:
        AudioStream(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16],
                    const uint8_t iv[16], int codecType, std::atomic<int64_t>& activity);
        ~AudioStream();
        bool start(uint16_t& dataPort, uint16_t& controlPort);
        void stop();

    private:
        void run();
        void onData(uint8_t* buf, size_t len);

        SessionSink& sink_;
        sockaddr_in6 peer_;
        crypto::AudioDecryptor decryptor_;
        int codecType_;
        std::atomic<int64_t>& activity_;
        int dataFd_ = -1, controlFd_ = -1;
        std::atomic<bool> running_{false};
        bool haveSeq_ = false;
        uint16_t lastSeq_ = 0;
        std::thread thread_;
    };

    class TimingClient {
    public:
        TimingClient(const sockaddr_in6& peer, uint16_t remotePort);
        ~TimingClient();
        /** Restituisce la porta UDP locale, o -1. */
        int start();
        void stop();

    private:
        void run();

        sockaddr_in6 peer_;
        int fd_ = -1;
        std::atomic<bool> running_{false};
        std::thread thread_;
    };

    class Session {
    public:
        Session(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16], const uint8_t iv[16]);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        int startTiming(uint16_t remotePort);
        int startMirror(uint64_t streamConnectionId);
        bool startAudio(int codecType, uint16_t& dataPort, uint16_t& controlPort);
        void stopMirror();
        void stopAudio();

        void touch() { activity_ = monotonicMs(); }
        int64_t idleMs() const { return monotonicMs() - activity_.load(); }
        bool mirrorConnected() const { return mirror_ && mirror_->connected(); }

    private:
        SessionSink& sink_;
        sockaddr_in6 peer_;
        uint8_t key_[16];
        uint8_t iv_[16];
        std::atomic<int64_t> activity_;
        std::unique_ptr<TimingClient> timing_;
        std::unique_ptr<MirrorStream> mirror_;
        std::unique_ptr<AudioStream> audio_;
    };

} // namespace irplay
