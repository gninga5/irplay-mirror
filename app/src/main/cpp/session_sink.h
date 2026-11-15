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
        virtual void onAudioFrame(const uint8_t* data, size_t len) = 0;
        /** Volume AirPlay in dB: da -30 (minimo) a 0; -144 = muto. */
        virtual void onVolume(float db) = 0;
    };

} // namespace irplay
