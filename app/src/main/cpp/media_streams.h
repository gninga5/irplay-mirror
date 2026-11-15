/*
 * media_streams.h
 * Scopo: i canali dati di una sessione di mirroring, aperti con SETUP.
 *  - MirrorStream: TCP, video H.264 cifrato AES-CTR. Pacchetti con header di 128 byte.
 *  - AudioStream:  UDP, RTP cifrato AES-CBC, più la porta di controllo. Due modalità:
 *                  duplicazione (AAC-ELD, riproduzione immediata) e musica (ALAC/AAC/PCM, con
 *                  riordino dei pacchetti e richiesta di ritrasmissione di quelli persi).
 *  - TimingClient: UDP, richieste di timing NTP verso l'iPhone (protocollo AirPlay 1).
 *  - Session:      possiede i tre canali, la chiave di sessione e il timestamp di attività
 *                  usato dal watchdog.
 *
 * Ogni canale accetta dati solo dall'indirizzo IP del client RTSP che ha fatto SETUP:
 * un altro host della LAN non può iniettare video o audio conoscendo la porta.
 * Le porte sono effimere (assegnate dal sistema): niente conflitti con altri servizi della TV.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

    /** Valori di "ct" in SETUP (formato audio scelto da iOS). */
    enum AudioCodec : int { kCodecPcm = 1, kCodecAlac = 2, kCodecAacLc = 4, kCodecAacEld = 8 };

    class AlacDecoder;   // involucro del decoder Apple, definito in media_streams.cpp

    class AudioStream {
    public:
        AudioStream(SessionSink& sink, const sockaddr_in6& peer, const uint8_t sessionKey[16],
                    const uint8_t iv[16], int codecType, uint16_t remoteControlPort,
                    std::atomic<int64_t>& activity);
        ~AudioStream();
        bool start(uint16_t& dataPort, uint16_t& controlPort);
        void stop();
        /** FLUSH (pausa, salto nel brano): scarta l'audio in coda. nextSeq < 0 = sconosciuto. */
        void requestFlush(int nextSeq);
        /**
         * Istante (CLOCK_MONOTONIC, ms) in cui il frame RTP indicato esce dagli altoparlanti,
         * secondo l'ultima sincronizzazione di iOS; 0 se non c'è o se rtp è troppo lontano.
         */
        int64_t dueMs(uint32_t rtp) const;

    private:
        struct Slot {
            bool used = false;
            uint16_t seq = 0;
            uint32_t rtp = 0;
            std::vector<uint8_t> data;
        };
        static constexpr size_t kSlots = 256;   // ~2 s di pacchetti ALAC da 352 campioni

        void run();
        void onRtp(uint8_t* buf, size_t len);
        void insert(uint16_t seq, uint32_t rtp, const uint8_t* payload, size_t len);
        void drain(int64_t now);
        void resetBuffer();
        void requestResend(uint16_t first, uint16_t count);
        void deliver(const uint8_t* payload, size_t len, uint32_t rtp);

        SessionSink& sink_;
        sockaddr_in6 peer_;
        sockaddr_in6 remoteControl_;
        crypto::AudioDecryptor decryptor_;
        int codecType_;
        bool music_;                     // tutto tranne AAC-ELD (duplicazione schermo)
        std::atomic<int64_t>& activity_;
        int dataFd_ = -1, controlFd_ = -1;
        std::atomic<bool> running_{false};
        std::atomic<int> flushSeq_{-2};  // -2 = nessun flush in attesa
        std::thread thread_;

        // Stato confinato al thread audio.
        bool haveSeq_ = false;
        uint16_t lastSeq_ = 0;           // duplicazione: ultimo numero di sequenza riprodotto
        uint16_t expected_ = 0;          // musica: prossimo pacchetto da riprodurre
        std::array<Slot, kSlots> slots_;
        size_t buffered_ = 0;
        int64_t gapSinceMs_ = 0;
        uint16_t resendCounter_ = 0;
        // Ultima sincronizzazione di iOS: frame RTP in uscita adesso e istante di arrivo.
        // Scritta dal thread audio, letta anche dal thread RTSP (avanzamento del brano).
        std::atomic<bool> haveSync_{false};
        std::atomic<uint32_t> syncRtp_{0};
        std::atomic<int64_t> syncAtMs_{0};
        // Prima della prima sincronizzazione (avvio e ripresa dopo una pausa, quando iOS riapre
        // il flusso): primo pacchetto ricevuto, che si suona dopo l'anticipo tipico di iOS.
        std::atomic<bool> haveFirst_{false};
        std::atomic<uint32_t> firstRtp_{0};
        std::atomic<int64_t> firstAtMs_{0};
        // Statistiche di qualità, scritte nel log a fine stream (solo conteggi).
        uint64_t statDelivered_ = 0, statLost_ = 0, statResendAsked_ = 0;
        std::unique_ptr<AlacDecoder> alac_;
        std::vector<uint8_t> pcm_;
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
        bool startAudio(int codecType, uint16_t remoteControlPort, uint16_t& dataPort, uint16_t& controlPort);
        void flushAudio(int nextSeq);
        /** Vedi AudioStream::dueMs; 0 senza flusso audio. */
        int64_t audioDueMs(uint32_t rtp) const;
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
