/*
 * hls_playlist.cpp
 * Vedi hls_playlist.h. Il testo di rete lo esamina e riscrive Rust (rust/src/hls.rs);
 * qui si copia il risultato nei tipi C++.
 */
#include "hls_playlist.h"

#include <algorithm>
#include <memory>

#include "rust_core.h"

namespace irplay::hls {

    namespace {
        struct ListFree {
            void operator()(ByteList* l) const { irplay_bytes_free(l); }
        };
        using List = std::unique_ptr<ByteList, ListFree>;

        const uint8_t* bytes(std::string_view s) { return reinterpret_cast<const uint8_t*>(s.data()); }

        std::optional<std::string> item(const List& l, size_t i) {
            const uint8_t* d = nullptr;
            size_t n = 0;
            if (!irplay_bytes_get(l.get(), i, &d, &n)) return std::nullopt;
            return std::string(reinterpret_cast<const char*>(d), n);
        }
    } // namespace

    bool isPlaylist(std::string_view text) {
        return irplay_hls_is_playlist(bytes(text), text.size());
    }

    bool isSafeHttpUrl(std::string_view url) {
        return irplay_hls_is_safe_http_url(bytes(url), url.size());
    }

    std::optional<std::string> remotePrefixOf(std::string_view location) {
        size_t n = 0;
        if (!irplay_hls_remote_prefix_len(bytes(location), location.size(), &n) || n > location.size()) return std::nullopt;
        return std::string(location.substr(0, n));
    }

    std::string toLocal(std::string_view remoteUri, std::string_view remotePrefix) {
        std::string out(kLocalPrefix);
        out.append(remoteUri.substr(std::min(remotePrefix.size(), remoteUri.size())));
        return out;
    }

    std::optional<Master> rewriteMaster(std::string_view text, std::string_view remotePrefix) {
        const List l(irplay_hls_rewrite_master(bytes(text), text.size(), bytes(remotePrefix), remotePrefix.size()));
        auto t = l ? item(l, 0) : std::nullopt;
        if (!t) return std::nullopt;
        Master m{std::move(*t), {}};
        for (size_t i = 1; i < irplay_bytes_count(l.get()); ++i) {
            auto u = item(l, i);
            if (!u) return std::nullopt;
            m.mediaUris.push_back(std::move(*u));
        }
        return m;
    }

    std::optional<std::string> expandCondensed(std::string_view media) {
        const List l(irplay_hls_expand_condensed(bytes(media), media.size()));
        return l ? item(l, 0) : std::nullopt;
    }

} // namespace irplay::hls
