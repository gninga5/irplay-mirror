/*
 * now_playing.cpp
 */
#include "now_playing.h"

#include "rust_core.h"

namespace irplay::nowplaying {
    namespace {
        constexpr uint32_t kRtpRate = 44100;
    } // namespace

    // I byte di rete li legge Rust (rust/src/dmap.rs): qui arrivano solo posizioni nel buffer.
    bool parseDmap(const uint8_t* data, size_t len, Metadata& out) {
        out = {};
        if (data == nullptr) return false;
        IrplayDmap m{};
        const bool ok = irplay_dmap_parse(data, len, &m);
        auto field = [&](IrplaySpan s) {
            return s.off <= len && s.len <= len - s.off ? std::string(reinterpret_cast<const char*>(data) + s.off, s.len) : std::string();
        };
        out.title = field(m.title);
        out.artist = field(m.artist);
        out.album = field(m.album);
        return ok;
    }

    bool parseProgressRtp(const uint8_t* data, size_t len, uint32_t& start, uint32_t& current, uint32_t& end) {
        return data != nullptr && irplay_progress_parse(data, len, &start, &current, &end);
    }

    bool progressMs(uint32_t start, uint32_t current, uint32_t end, int64_t& positionMs, int64_t& durationMs) {
        // Timestamp RTP a 32 bit: le differenze vanno calcolate modulo 2^32 (possono "girare").
        const uint32_t pos = current - start, dur = end - start;
        if (pos > dur) return false;
        positionMs = static_cast<int64_t>(pos) * 1000 / kRtpRate;
        durationMs = static_cast<int64_t>(dur) * 1000 / kRtpRate;
        return true;
    }

    bool parseProgress(const uint8_t* data, size_t len, int64_t& positionMs, int64_t& durationMs) {
        uint32_t start, cur, end;
        return parseProgressRtp(data, len, start, cur, end) && progressMs(start, cur, end, positionMs, durationMs);
    }

} // namespace irplay::nowplaying
