/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 4).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp, che confronta
 * il risultato del parser Rust con questo su milioni di input validi e alterati.
 */
/*
 * now_playing.cpp
 */
#include "reference.h"

#include <cstring>
#include <string_view>

namespace ref::nowplaying {
    using irplay::nowplaying::Metadata;
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

    bool parseProgressRtp(const uint8_t* data, size_t len, uint32_t& start, uint32_t& current, uint32_t& end) {
        constexpr std::string_view prefix = "progress:";
        if (data == nullptr || len < prefix.size() || len > 128) return false;
        std::string_view s(reinterpret_cast<const char*>(data), len);
        if (s.substr(0, prefix.size()) != prefix) return false;
        s.remove_prefix(prefix.size());
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        if (!parseUint(s, start) || s.empty() || s.front() != '/') return false;
        s.remove_prefix(1);
        if (!parseUint(s, current) || s.empty() || s.front() != '/') return false;
        s.remove_prefix(1);
        return parseUint(s, end);
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

} // namespace ref::nowplaying
