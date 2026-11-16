/*
 * rust_core.h
 * Scopo: dichiarazioni delle funzioni del componente Rust (app/src/main/rust), libreria
 * statica collegata dentro libirplay_native.so. Ogni funzione qui deve corrispondere a una
 * `extern "C"` in Rust; kRustAbiVersion va aumentata insieme a ABI_VERSION in lib.rs.
 * Nessuna di queste funzioni lascia uscire un panic: in Rust diventa un valore di errore.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace irplay {
    inline constexpr uint32_t kRustAbiVersion = 3;

    // Esiti di irplay_hevc_config (HEVC_CONFIG_* in lib.rs).
    inline constexpr int kHevcConfigInvalid = 0;
    inline constexpr int kHevcConfigUnchanged = 1;
    inline constexpr int kHevcConfigLowLatency = 2;
}

extern "C" {
    struct AlacHandle;

    uint32_t irplay_rust_abi_version();

    // ALAC (rust/src/alac.rs, Symphonia): decoder AirPlay 352 campioni, 16 bit, stereo.
    // out_cap >= 704 campioni i16; out_frames = campioni per canale. Un handle per thread.
    AlacHandle* irplay_alac_new();
    bool irplay_alac_decode(AlacHandle* handle, const uint8_t* input, size_t len,
                            int16_t* out, size_t out_cap, size_t* out_frames);
    void irplay_alac_free(AlacHandle* handle);

    // HEVC (rust/src/hevc.rs): configurazione "hvc1" di iOS -> VPS/SPS/PPS Annex-B per csd-0,
    // VPS/SPS riscritti per bassa latenza se richiesto.
    int irplay_hevc_config(const uint8_t* payload, size_t len, bool low_latency,
                           uint8_t* out, size_t out_cap, size_t* out_len, uint8_t* extra_bits);
    // Fotogramma a prefissi di lunghezza -> Annex-B sul posto; false = da scartare.
    bool irplay_hevc_frame(uint8_t* buf, size_t len, uint8_t extra_bits, bool* keyframe, bool* b_slice);
}
