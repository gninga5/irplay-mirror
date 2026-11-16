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
    inline constexpr uint32_t kRustAbiVersion = 4;

    // Esiti di irplay_hevc_config (HEVC_CONFIG_* in lib.rs).
    inline constexpr int kHevcConfigInvalid = 0;
    inline constexpr int kHevcConfigUnchanged = 1;
    inline constexpr int kHevcConfigLowLatency = 2;

    // Tipi di nodo dei plist (plist::kind in plist.rs).
    inline constexpr uint8_t kPlistBool = 0;
    inline constexpr uint8_t kPlistUInt = 1;
    inline constexpr uint8_t kPlistReal = 2;
    inline constexpr uint8_t kPlistString = 3;
    inline constexpr uint8_t kPlistData = 4;
    inline constexpr uint8_t kPlistArray = 5;
    inline constexpr uint8_t kPlistDict = 6;
}

extern "C" {
    struct AlacHandle;
    struct PlistDoc;
    struct ByteList;

    // Porzione [off, off+len) di un buffer passato a Rust.
    struct IrplaySpan { size_t off; size_t len; };
    struct IrplayDmap { IrplaySpan title, artist, album; };
    struct IrplayPlistNode { uint8_t kind; uint64_t uint; double real; const uint8_t* bytes; size_t len; size_t count; };
    struct IrplayRtspHead { size_t consumed; IrplaySpan method, uri, protocol, body; size_t header_count; };

    uint32_t irplay_rust_abi_version();

    // ALAC (rust/src/alac.rs, Symphonia): decoder AirPlay 352 campioni, 16 bit, stereo.
    // out_cap >= 704 campioni i16; out_frames = campioni per canale. Un handle per thread.
    AlacHandle* irplay_alac_new();
    bool irplay_alac_decode(AlacHandle* handle, const uint8_t* input, size_t len,
                            int16_t* out, size_t out_cap, size_t* out_frames);
    void irplay_alac_free(AlacHandle* handle);

    // Parser (rust/src/ffi.rs). Le posizioni restituite (IrplaySpan) si riferiscono al
    // buffer passato; i dati di PlistDoc e ByteList restano validi fino al rispettivo _free.
    PlistDoc* irplay_plist_parse(const uint8_t* data, size_t len);
    bool irplay_plist_node(const PlistDoc* doc, uint32_t id, IrplayPlistNode* out);
    bool irplay_plist_child(const PlistDoc* doc, uint32_t id, size_t i, uint32_t* child, const uint8_t** key, size_t* key_len);
    void irplay_plist_free(PlistDoc* doc);

    bool irplay_dmap_parse(const uint8_t* data, size_t len, IrplayDmap* out);
    bool irplay_progress_parse(const uint8_t* data, size_t len, uint32_t* start, uint32_t* current, uint32_t* end);

    // -1 malformato, 0 servono altri byte, 1 completo. headers_cap (in span) >= 128.
    int irplay_rtsp_parse_request(const uint8_t* buf, size_t len, IrplayRtspHead* out, IrplaySpan* headers, size_t headers_cap);
    int irplay_rtsp_discard_response(const uint8_t* buf, size_t len, size_t* consumed);

    size_t irplay_bytes_count(const ByteList* list);
    bool irplay_bytes_get(const ByteList* list, size_t i, const uint8_t** data, size_t* len);
    void irplay_bytes_free(ByteList* list);
    bool irplay_hls_is_playlist(const uint8_t* text, size_t len);
    bool irplay_hls_is_safe_http_url(const uint8_t* url, size_t len);
    bool irplay_hls_remote_prefix_len(const uint8_t* location, size_t len, size_t* out);
    ByteList* irplay_hls_rewrite_master(const uint8_t* text, size_t len, const uint8_t* prefix, size_t prefix_len);
    ByteList* irplay_hls_expand_condensed(const uint8_t* text, size_t len);

    // HEVC (rust/src/hevc.rs): configurazione "hvc1" di iOS -> VPS/SPS/PPS Annex-B per csd-0,
    // VPS/SPS riscritti per bassa latenza se richiesto.
    int irplay_hevc_config(const uint8_t* payload, size_t len, bool low_latency,
                           uint8_t* out, size_t out_cap, size_t* out_len, uint8_t* extra_bits);
    // Fotogramma a prefissi di lunghezza -> Annex-B sul posto; false = da scartare.
    bool irplay_hevc_frame(uint8_t* buf, size_t len, uint8_t extra_bits, bool* keyframe, bool* b_slice);
}
