/*
 * now_playing.cpp
 */
#include "now_playing.h"

#include <cstring>
#include <string_view>

namespace irplay::nowplaying {
    namespace {
        constexpr size_t kMaxField = 256;   // titoli più lunghi vengono troncati
        constexpr uint32_t kRtpRate = 44100;

        uint32_t be32(const uint8_t* p) {
            return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        }

        // Tronca al confine di un carattere UTF-8, così Kotlin non riceve mezze sequenze.
        std::string clip(const uint8_t* p, size_t n) {
            if (n > kMaxField) {
                n = kMaxField;
                while (n > 0 && (p[n] & 0xC0) == 0x80) --n;
            }
            return std::string(reinterpret_cast<const char*>(p), n);
        }

        // Scorre gli elementi DMAP in [p, p+len). Se trova "mlit" ne esamina il contenuto
        // (un solo livello); restituisce false alla prima lunghezza incoerente.
        bool walk(const uint8_t* p, size_t len, Metadata& out, bool allowContainer) {
            size_t pos = 0;
            while (pos < len) {
                if (len - pos < 8) return false;
                const uint8_t* tag = p + pos;
                const uint32_t n = be32(p + pos + 4);
                if (n > len - pos - 8) return false;
                const uint8_t* d = p + pos + 8;
                if (std::memcmp(tag, "mlit", 4) == 0) {
                    if (!allowContainer || !walk(d, n, out, false)) return false;
                } else if (std::memcmp(tag, "minm", 4) == 0) {
                    out.title = clip(d, n);
                } else if (std::memcmp(tag, "asar", 4) == 0) {
                    out.artist = clip(d, n);
                } else if (std::memcmp(tag, "asal", 4) == 0) {
                    out.album = clip(d, n);
                }
                pos += 8 + n;
            }
            return true;
        }

        bool parseUint(std::string_view& s, uint32_t& out) {
            uint64_t v = 0;
            size_t i = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + static_cast<uint64_t>(s[i] - '0');
                if (v > 0xFFFFFFFFu) return false;
                ++i;
            }
            if (i == 0) return false;
            out = static_cast<uint32_t>(v);
            s.remove_prefix(i);
            return true;
        }
    } // namespace

    bool parseDmap(const uint8_t* data, size_t len, Metadata& out) {
        out = {};
        return data != nullptr && walk(data, len, out, true);
    }

    bool parseProgress(const uint8_t* data, size_t len, int64_t& positionMs, int64_t& durationMs) {
        constexpr std::string_view prefix = "progress:";
        if (data == nullptr || len < prefix.size() || len > 128) return false;
        std::string_view s(reinterpret_cast<const char*>(data), len);
        if (s.substr(0, prefix.size()) != prefix) return false;
        s.remove_prefix(prefix.size());
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        uint32_t start, cur, end;
        if (!parseUint(s, start) || s.empty() || s.front() != '/') return false;
        s.remove_prefix(1);
        if (!parseUint(s, cur) || s.empty() || s.front() != '/') return false;
        s.remove_prefix(1);
        if (!parseUint(s, end)) return false;
        // Timestamp RTP a 32 bit: le differenze vanno calcolate modulo 2^32 (possono "girare").
        const uint32_t pos = cur - start, dur = end - start;
        if (pos > dur) return false;
        positionMs = static_cast<int64_t>(pos) * 1000 / kRtpRate;
        durationMs = static_cast<int64_t>(dur) * 1000 / kRtpRate;
        return true;
    }

} // namespace irplay::nowplaying
