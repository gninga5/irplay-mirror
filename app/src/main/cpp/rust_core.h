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
    inline constexpr uint32_t kRustAbiVersion = 6;

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
    struct IrplayIdentity;
    struct IrplayPairing;
    struct IrplayCtr;
    struct IrplayCbc;

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

    // H.264 (rust/src/h264.rs).
    bool irplay_h264_annexb(uint8_t* buf, size_t len, bool check_b, bool* keyframe, bool* b_slice);
    ByteList* irplay_h264_parse_avcc(const uint8_t* data, size_t len);
    ByteList* irplay_h264_patch_sps(const uint8_t* sps, size_t len);
    bool irplay_h264_slice_is_b(const uint8_t* nal, size_t len);

    // Crittografia (rust/src/ffi_crypto.rs). Segreti e stati restano in Rust.
    bool irplay_random(uint8_t* buf, size_t len);
    bool irplay_random_below(uint32_t n, uint32_t* out);
    void irplay_secure_zero(uint8_t* p, size_t len);
    bool irplay_ct_equal(const uint8_t* a, const uint8_t* b, size_t len);
    bool irplay_aes_arm_hardware();

    IrplayIdentity* irplay_identity_new(const uint8_t* seed32);
    bool irplay_identity_public(const IrplayIdentity* id, uint8_t* out32);
    bool irplay_identity_sign(const IrplayIdentity* id, const uint8_t* msg, size_t len, uint8_t* sig64);
    void irplay_identity_free(IrplayIdentity* id);

    IrplayPairing* irplay_pairing_new();
    void irplay_pairing_free(IrplayPairing* p);
    bool irplay_pairing_setup(IrplayPairing* p, const uint8_t* body, size_t len);
    bool irplay_pairing_verify1(IrplayPairing* p, const IrplayIdentity* id, const uint8_t* body, size_t len, uint8_t* out96);
    bool irplay_pairing_verify2(IrplayPairing* p, const uint8_t* body, size_t len);
    bool irplay_pairing_verified(IrplayPairing* p);
    bool irplay_pairing_client_ed_key(IrplayPairing* p, uint8_t* out32);
    bool irplay_pairing_pin_client_key(IrplayPairing* p, uint8_t* out32);
    bool irplay_pairing_pin_authenticated(IrplayPairing* p);
    bool irplay_pairing_session_key(IrplayPairing* p, const uint8_t* fp_key16, uint8_t* out16);
    bool irplay_pairing_pin_begin(IrplayPairing* p, const uint8_t* user, size_t user_len, const uint8_t* pin, size_t pin_len,
                                  uint8_t* salt16, uint8_t* b, size_t b_cap, size_t* b_len);
    bool irplay_pairing_pin_proof(IrplayPairing* p, const uint8_t* a, size_t a_len, const uint8_t* m1, size_t m1_len, uint8_t* m2_20);
    bool irplay_pairing_pin_exchange(IrplayPairing* p, const IrplayIdentity* id, const uint8_t* epk, size_t epk_len,
                                     const uint8_t* tag, size_t tag_len, uint8_t* out_epk32, uint8_t* out_tag16);

    bool irplay_derive_mirror_key(const uint8_t* session_key16, uint64_t stream_connection_id, uint8_t* key16, uint8_t* iv16);
    IrplayCtr* irplay_ctr_new(const uint8_t* key16, const uint8_t* iv16);
    bool irplay_ctr_apply(IrplayCtr* h, uint8_t* data, size_t len);
    bool irplay_ctr_skip(IrplayCtr* h, size_t len);
    void irplay_ctr_free(IrplayCtr* h);
    IrplayCbc* irplay_cbc_new(const uint8_t* key16, const uint8_t* iv16);
    bool irplay_cbc_decrypt(const IrplayCbc* h, uint8_t* data, size_t len);
    void irplay_cbc_free(IrplayCbc* h);

    // HEVC (rust/src/hevc.rs): configurazione "hvc1" di iOS -> VPS/SPS/PPS Annex-B per csd-0,
    // VPS/SPS riscritti per bassa latenza se richiesto.
    int irplay_hevc_config(const uint8_t* payload, size_t len, bool low_latency,
                           uint8_t* out, size_t out_cap, size_t* out_len, uint8_t* extra_bits);
    // Fotogramma a prefissi di lunghezza -> Annex-B sul posto; false = da scartare.
    bool irplay_hevc_frame(uint8_t* buf, size_t len, uint8_t extra_bits, bool* keyframe, bool* b_slice);
}
