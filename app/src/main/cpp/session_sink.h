/*
 * session_sink.h
 * Scopo: confine tra il motore di rete C++ e il resto dell'app.
 * Il server e i flussi chiamano questi metodi dai loro thread (RTSP, video, audio):
 * l'implementazione (jni_bridge.cpp) deve essere thread-safe e non bloccare a lungo,
 * altrimenti il socket TCP del video si riempie e iOS rallenta o interrompe il mirroring.
 * Separare l'interfaccia dal JNI permette di testare il motore su host senza la JVM.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace irplay {

    enum class EndReason : int {
        Teardown = 0,     // TEARDOWN completo dal client
        Disconnected = 1, // connessione RTSP chiusa
        Watchdog = 2,     // nessun dato per 3 s (iPhone spento o fuori portata)
        Local = 3,        // fermata dall'utente (tasto BACK)
    };

    class SessionSink {
    public:
        virtual ~SessionSink() = default;

        /** clientName: nome del dispositivo in UTF-8 (dato personale: solo da mostrare, mai nel log). */
        virtual void onSessionStarted(const std::string& clientName) = 0;
        virtual void onSessionEnded(EndReason reason) = 0;

        virtual void onVideoStarted() = 0;
        virtual void onVideoStopped() = 0;
        /** SPS e PPS con start code Annex-B. */
        virtual void onVideoConfig(const uint8_t* sps, size_t spsLen, const uint8_t* pps, size_t ppsLen) = 0;
        /** Frame Annex-B completo. ptsUs: orologio del mittente, monotono. */
        virtual void onVideoFrame(const uint8_t* data, size_t len, int64_t ptsUs, bool keyframe) = 0;

        /** codecType: valore "ct" di SETUP (2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD). */
        virtual void onAudioStarted(int codecType) = 0;
        virtual void onAudioStopped() = 0;
        /**
         * dueMs: istante (CLOCK_MONOTONIC, ms) in cui il pacchetto deve uscire dagli altoparlanti,
         * secondo la sincronizzazione di iOS; 0 = appena possibile (duplicazione schermo).
         */
        virtual void onAudioFrame(const uint8_t* data, size_t len, int64_t dueMs) = 0;
        /** FLUSH dal client (pausa o salto nel brano): l'audio già in coda va scartato. */
        virtual void onAudioFlush() = 0;
        /** Brano in riproduzione (audio AirPlay): stringhe UTF-8, possono essere vuote. */
        virtual void onMetadata(const std::string& title, const std::string& artist, const std::string& album) = 0;
        /** Copertina JPEG o PNG (già controllata nella firma e nella dimensione); len 0 = nessuna. */
        virtual void onArtwork(const uint8_t* data, size_t len) = 0;
        /** Avanzamento del brano. */
        /** atMs: istante (CLOCK_MONOTONIC) in cui la posizione si sente; 0 = adesso. */
        virtual void onProgress(int64_t positionMs, int64_t durationMs, int64_t atMs) = 0;
        /** Volume AirPlay in dB: da -30 (minimo) a 0; -144 = muto. */
        virtual void onVolume(float db) = 0;
        /**
         * Telecomando DACP dell'app che suona (Musica, Spotify ...): la TV può chiedere all'iPhone
         * play/pausa e brano successivo/precedente. dacpId: 16 cifre esadecimali (il servizio mDNS
         * è "iTunes_Ctrl_<dacpId>"), activeRemote: solo cifre, è il "token" da rimandare
         * nell'intestazione Active-Remote. peerAddr: indirizzo IPv6 (o IPv4 mappato) dell'iPhone.
         * Il token permette di comandare l'iPhone: mai nel log.
         */
        virtual void onRemoteControl(const std::string& dacpId, const std::string& activeRemote,
                                     const std::string& peerAddr) = 0;

        /** Un dispositivo chiede il PIN: va mostrato a schermo (mai nel log). */
        virtual void onPinRequested(const std::string& pin) = 0;
        /** PIN usato, scaduto o sbagliato: va tolto dallo schermo. */
        virtual void onPinDismissed() = 0;
        /** Nuovo dispositivo associato con il PIN: la sua chiave Ed25519 va salvata. */
        virtual void onClientTrusted(const uint8_t publicKey[32]) = 0;

        // ---- AirPlay video: l'iPhone passa un indirizzo e la TV lo riproduce da sé.
        /**
         * url: indirizzo http(s) già validato, oppure hls::kLocalMaster quando le playlist sono
         * arrivate dall'iPhone (YouTube); in quel caso playlists contiene le coppie
         * (URI locale, testo m3u8), master compresa. startFraction >= 0 sostituisce startSec:
         * va moltiplicata per la durata, nota solo al lettore.
         */
        virtual void onMediaPlay(const std::string& url, double startSec, double startFraction,
                                 const std::vector<std::pair<std::string, std::string>>& playlists) = 0;
        /** 0 = pausa, 1 = riproduzione. */
        virtual void onMediaRate(float rate) = 0;
        virtual void onMediaScrub(double positionSec) = 0;
        /** L'iPhone ha chiuso il video (o è sparito): il lettore va fermato. */
        virtual void onMediaStop() = 0;
    };

} // namespace irplay
