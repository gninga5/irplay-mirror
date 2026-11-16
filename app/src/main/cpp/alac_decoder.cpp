/*
 * alac_decoder.cpp
 * Vedi alac_decoder.h.
 */
#include "alac_decoder.h"

#include "rust_core.h"

namespace irplay {

    AlacDecoder::~AlacDecoder() {
        irplay_alac_free(handle_);
    }

    bool AlacDecoder::init() {
        if (!handle_) handle_ = irplay_alac_new();
        return handle_ != nullptr;
    }

    bool AlacDecoder::decode(const uint8_t* in, size_t len, std::vector<uint8_t>& out) {
        if (!handle_ || !in || len == 0 || len > kMaxPacket) return false;
        int16_t pcm[kFrame * kChannels];
        size_t frames = 0;
        if (!irplay_alac_decode(handle_, in, len, pcm, kFrame * kChannels, &frames)) return false;
        if (frames == 0 || frames > static_cast<size_t>(kFrame)) return false;
        // PCM 16 bit little-endian per AudioTrack, indipendente dall'ordine dei byte della CPU.
        out.resize(frames * kChannels * 2);
        uint8_t* o = out.data();
        for (size_t i = 0; i < frames * kChannels; ++i) {
            const auto v = static_cast<uint16_t>(pcm[i]);
            *o++ = static_cast<uint8_t>(v);
            *o++ = static_cast<uint8_t>(v >> 8);
        }
        return true;
    }

} // namespace irplay
