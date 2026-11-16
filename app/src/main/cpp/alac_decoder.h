/*
 * alac_decoder.h
 * Scopo: decodifica dei pacchetti Apple Lossless (ALAC) dell'audio AirPlay "solo musica".
 *
 * Involucro C++ del decoder in Rust (app/src/main/rust/src/alac.rs), basato su Symphonia:
 * scritto interamente in Rust senza codice unsafe, verificato bit per bit contro l'encoder
 * di riferimento di Apple (tools/hosttest). Ha preso il posto di FFmpeg, che a sua volta
 * aveva sostituito il decoder di Apple (fermo dal 2012, con CVE-2021-0674, CVE-2021-0675 e
 * CVE-2021-30351 mai corrette).
 *
 * La configurazione AirPlay (352 campioni, 16 bit, stereo, 44,1 kHz) è fissa e sta nel
 * modulo Rust; dalla rete arriva solo il pacchetto, di cui il decoder controlla i limiti.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
    struct AlacHandle;
}

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
        AlacHandle* handle_ = nullptr;
    };

} // namespace irplay
