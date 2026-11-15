/*
 * alac_decoder.cpp
 * Vedi alac_decoder.h.
 */
#include "alac_decoder.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

namespace irplay {

    AlacDecoder::~AlacDecoder() {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&ctx_);
    }

    bool AlacDecoder::init() {
        // FFmpeg scrive i suoi messaggi su stderr/logcat con dettagli dei pacchetti: zitto.
        av_log_set_level(AV_LOG_QUIET);
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!ctx_ || !frame_ || !packet_) return false;
        // Atomo "alac" (36 byte) come nei file MP4: dimensione, tipo, versione, poi la
        // configurazione ALACSpecificConfig di 24 byte (big-endian).
        static constexpr uint8_t kMagicCookie[36] = {
                0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x60,   // frameLength 352
                0x00,                     // compatibleVersion
                0x10,                     // bitDepth 16
                0x28, 0x0A, 0x0E,         // pb 40, mb 10, kb 14
                0x02,                     // numChannels
                0x00, 0xFF,               // maxRun 255
                0x00, 0x00, 0x00, 0x00,   // maxFrameBytes
                0x00, 0x00, 0x00, 0x00,   // avgBitRate
                0x00, 0x00, 0xAC, 0x44,   // sampleRate 44100
        };
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(sizeof kMagicCookie + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx_->extradata) return false;
        std::memcpy(ctx_->extradata, kMagicCookie, sizeof kMagicCookie);
        ctx_->extradata_size = sizeof kMagicCookie;
        ctx_->sample_rate = 44100;
        av_channel_layout_default(&ctx_->ch_layout, kChannels);
        return avcodec_open2(ctx_, codec, nullptr) == 0;
    }

    bool AlacDecoder::decode(const uint8_t* in, size_t len, std::vector<uint8_t>& out) {
        if (!ctx_ || len == 0 || len > kMaxPacket) return false;
        // av_new_packet aggiunge AV_INPUT_BUFFER_PADDING_SIZE byte a zero in coda, come FFmpeg richiede.
        av_packet_unref(packet_);
        if (av_new_packet(packet_, static_cast<int>(len)) != 0) return false;
        std::memcpy(packet_->data, in, len);
        if (avcodec_send_packet(ctx_, packet_) != 0) {
            avcodec_flush_buffers(ctx_);
            return false;
        }
        const int r = avcodec_receive_frame(ctx_, frame_);
        if (r != 0) return false;
        const int n = frame_->nb_samples;
        const bool ok = n > 0 && n <= kFrame && frame_->format == AV_SAMPLE_FMT_S16P &&
                        frame_->ch_layout.nb_channels == kChannels && frame_->data[0] && frame_->data[1];
        if (ok) {
            // Planare (un piano per canale) -> interleaved little-endian per AudioTrack.
            out.resize(static_cast<size_t>(n) * kChannels * 2);
            const auto* l = reinterpret_cast<const int16_t*>(frame_->data[0]);
            const auto* rch = reinterpret_cast<const int16_t*>(frame_->data[1]);
            uint8_t* o = out.data();
            for (int i = 0; i < n; ++i) {
                const auto a = static_cast<uint16_t>(l[i]), b = static_cast<uint16_t>(rch[i]);
                *o++ = static_cast<uint8_t>(a); *o++ = static_cast<uint8_t>(a >> 8);
                *o++ = static_cast<uint8_t>(b); *o++ = static_cast<uint8_t>(b >> 8);
            }
        }
        av_frame_unref(frame_);
        return ok;
    }

} // namespace irplay
