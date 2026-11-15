/*
 * alac_decoder.h
 * Scopo: decodifica dei pacchetti Apple Lossless (ALAC) dell'audio AirPlay "solo musica".
 *
 * Usa il decoder di FFmpeg (libavcodec, LGPL-2.1+), compilato con il solo decoder ALAC da
 * tools/build_ffmpeg.sh. Sostituisce il decoder di riferimento di Apple, fermo dal 2012 e con
 * vulnerabilità note mai corrette (CVE-2021-0674, CVE-2021-0675, CVE-2021-30351).
 *
 * AirPlay usa sempre la stessa configurazione, che iOS non invia nel SETUP (era nel campo
 * "fmtp" dell'ANNOUNCE di AirPlay 1): 352 campioni per pacchetto, 16 bit, stereo, 44,1 kHz
 * -> "96 352 0 16 40 10 14 2 255 0 0 44100". La configurazione è nostra, non arriva dalla
 * rete; il pacchetto invece sì: FFmpeg ne controlla i limiti, e qui si accettano solo frame
 * con al più kFrame campioni nel formato atteso.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace irplay {

    class AlacDecoder {
    public:
        static constexpr int kFrame = 352;
        static constexpr int kChannels = 2;
        static constexpr size_t kMaxPacket = 8192;

        AlacDecoder() = default;
        AlacDecoder(const AlacDecoder&) = delete;
        AlacDecoder& operator=(const AlacDecoder&) = delete;
        ~AlacDecoder();

        bool init();

        /** false se il pacchetto non è decodificabile; out = PCM 16 bit little-endian interleaved. */
        bool decode(const uint8_t* in, size_t len, std::vector<uint8_t>& out);

    private:
        AVCodecContext* ctx_ = nullptr;
        AVFrame* frame_ = nullptr;
        AVPacket* packet_ = nullptr;
    };

} // namespace irplay
