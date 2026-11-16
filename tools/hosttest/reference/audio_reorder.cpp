/*
 * Vedi audio_reorder.h. Corpo delle funzioni copiato da media_streams.cpp (AudioStream).
 */
#include "audio_reorder.h"

namespace ref::audio {
    namespace {
        constexpr int64_t kGapWaitMs = 150;
        constexpr uint16_t kMaxResend = 64;
        uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
        uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
    }

    void Reorder::resetBuffer() {
        for (auto& s : slots_) { s.used = false; s.data.clear(); }
        buffered_ = 0;
        gapSinceMs_ = 0;
    }

    void Reorder::flush(int pending) {
        resetBuffer();
        if (pending >= 0) { expected_ = static_cast<uint16_t>(pending); haveSeq_ = true; }
        else haveSeq_ = false;
    }

    void Reorder::insert(uint16_t seq, uint32_t rtp, const uint8_t* payload, size_t len) {
        if (!haveSeq_) { haveSeq_ = true; expected_ = seq; }
        const auto delta = static_cast<int16_t>(seq - expected_);
        if (delta < 0) {
            if (delta > -1000) return;
            resetBuffer();
            expected_ = seq;
        } else if (delta >= static_cast<int>(kSlots)) {
            resetBuffer();
            expected_ = seq;
        }
        Slot& slot = slots_[seq % kSlots];
        if (slot.used) return;
        slot.used = true;
        slot.seq = seq;
        slot.rtp = rtp;
        slot.data.assign(payload, payload + len);
        ++buffered_;
    }

    void Reorder::drain(int64_t now, std::vector<Action>& out) {
        for (;;) {
            Slot& slot = slots_[expected_ % kSlots];
            if (slot.used && slot.seq == expected_) {
                out.push_back({1, slot.rtp, 0, slot.data});
                slot.used = false;
                slot.data.clear();
                --buffered_;
                ++expected_;
                gapSinceMs_ = 0;
                continue;
            }
            if (buffered_ == 0) return;
            uint16_t next = expected_;
            for (size_t i = 1; i < kSlots; ++i) {
                const auto s = static_cast<uint16_t>(expected_ + i);
                if (slots_[s % kSlots].used && slots_[s % kSlots].seq == s) { next = s; break; }
            }
            if (next == expected_) { resetBuffer(); return; }
            if (gapSinceMs_ == 0) {
                gapSinceMs_ = now;
                const auto missing = static_cast<uint16_t>(next - expected_);
                out.push_back({2, expected_, missing < kMaxResend ? missing : kMaxResend, {}});
                ++statResendAsked_;
                return;
            }
            if (now - gapSinceMs_ < kGapWaitMs) return;
            statLost_ += static_cast<uint16_t>(next - expected_);
            expected_ = next;
            gapSinceMs_ = 0;
        }
    }

    bool Reorder::acceptLive(uint16_t seq) {
        if (haveSeq_) {
            const auto delta = static_cast<int16_t>(seq - lastSeq_);
            if (delta <= 0 && delta > -1000) return false;
            if (delta > 1 && delta < 1000) statLost_ += static_cast<uint64_t>(delta - 1);
        }
        haveSeq_ = true;
        lastSeq_ = seq;
        return true;
    }

    bool parseRtp(const uint8_t* buf, size_t len, uint16_t& seq, uint32_t& rtp) {
        if (len <= 12 || (buf[0] & 0xC0) != 0x80 || (buf[1] & 0x7F) != 0x60) return false;
        seq = be16(buf + 2);
        rtp = be32(buf + 4);
        return true;
    }

    int classifyControl(const uint8_t* buf, size_t n, uint32_t& rtp) {
        if (n > 4 + 12 && (buf[1] & 0x7F) == 0x56) return 1;
        if (n >= 20 && (buf[1] & 0x7F) == 0x54) { rtp = be32(buf + 4); return 2; }
        return 0;
    }

} // namespace ref::audio
