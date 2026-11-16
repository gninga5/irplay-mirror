/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 4).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp, che confronta
 * il risultato del parser Rust con questo su milioni di input validi e alterati.
 */
/*
 * hls_playlist.cpp
 * Vedi hls_playlist.h. Tutto il lavoro è per righe (string_view sul testo d'origine):
 * nessun indice viene calcolato da dati della rete senza un controllo di lunghezza.
 */
#include "reference.h"

#include <algorithm>

namespace ref::hls {
    using namespace irplay::hls;

    namespace {

        constexpr size_t kMaxLines = 400000;          // ~ 2 giorni di segmenti da 0,5 s
        constexpr size_t kMaxParams = 32;

        bool startsWith(std::string_view s, std::string_view p) {
            return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
        }

        // Divide il testo in righe (LF o CRLF). false se le righe sono troppe.
        bool splitLines(std::string_view text, std::vector<std::string_view>& out) {
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string_view::npos) nl = text.size();
                std::string_view line = text.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                out.push_back(line);
                if (out.size() > kMaxLines) return false;
                pos = nl + 1;
            }
            return true;
        }

        // Un URI accettabile: ASCII stampabile, niente spazi né virgolette, lunghezza limitata.
        bool isCleanUri(std::string_view u) {
            if (u.empty() || u.size() > kMaxUriBytes) return false;
            for (unsigned char c : u) {
                if (c <= 0x20 || c >= 0x7F || c == '"') return false;
            }
            return true;
        }

        // Valore dell'attributo NAME="..." in una riga di tag; nullopt se assente o non chiuso.
        std::optional<std::string_view> quotedAttr(std::string_view line, std::string_view name) {
            size_t from = 0;
            while (true) {
                const size_t p = line.find(name, from);
                if (p == std::string_view::npos) return std::nullopt;
                // Il nome deve essere un attributo intero: preceduto da ':' o ',' (es. "URI" non in "BASE-URI").
                const bool boundary = p > 0 && (line[p - 1] == ':' || line[p - 1] == ',');
                const size_t q = p + name.size();
                if (boundary && q + 1 < line.size() && line[q] == '=' && line[q + 1] == '"') {
                    const size_t start = q + 2;
                    const size_t end = line.find('"', start);
                    if (end == std::string_view::npos) return std::nullopt;
                    return line.substr(start, end - start);
                }
                from = p + 1;
            }
        }

        void addUnique(std::vector<std::string>& v, std::string_view s) {
            if (std::find(v.begin(), v.end(), s) == v.end()) v.emplace_back(s);
        }

        bool isHeAac(std::string_view codecs) {
            return codecs.find("mp4a.40.5") != std::string_view::npos ||
                   codecs.find("mp4a.40.29") != std::string_view::npos;
        }

        /*
         * YouTube offre due gruppi audio, entrambi DEFAULT=YES: HE-AAC a ~48 kbps (varianti
         * piccole) e AAC-LC a 128 kbps. Senza bitrate sulle tracce audio ExoPlayer prende il
         * primo elencato, quello HE-AAC, e su un impianto buono si sente ovattato.
         * Se esiste almeno una variante con AAC-LC si tolgono le varianti HE-AAC e le tracce
         * audio dei gruppi rimasti senza varianti. Altrimenti la playlist resta com'è.
         * Restituisce, per ogni riga, se va tenuta.
         */
        std::vector<bool> preferAacLc(const std::vector<std::string_view>& lines) {
            std::vector<bool> keep(lines.size(), true);
            bool haveLc = false;
            for (std::string_view line : lines) {
                if (!startsWith(line, "#EXT-X-STREAM-INF:")) continue;
                const auto codecs = quotedAttr(line, "CODECS");
                if (codecs && codecs->find("mp4a.40.2") != std::string_view::npos && !isHeAac(*codecs)) haveLc = true;
            }
            if (!haveLc) return keep;

            std::vector<std::string_view> usedGroups;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (!startsWith(lines[i], "#EXT-X-STREAM-INF:")) continue;
                const auto codecs = quotedAttr(lines[i], "CODECS");
                if (codecs && isHeAac(*codecs)) {
                    keep[i] = false;
                    // La riga URI della variante è la prossima che non è un tag né vuota.
                    for (size_t j = i + 1; j < lines.size(); ++j) {
                        if (lines[j].empty() || lines[j].front() == '#') continue;
                        keep[j] = false;
                        break;
                    }
                } else if (const auto group = quotedAttr(lines[i], "AUDIO")) {
                    usedGroups.push_back(*group);
                }
            }
            for (size_t i = 0; i < lines.size(); ++i) {
                if (!startsWith(lines[i], "#EXT-X-MEDIA:") || lines[i].find("TYPE=AUDIO") == std::string_view::npos) continue;
                const auto group = quotedAttr(lines[i], "GROUP-ID");
                if (group && std::find(usedGroups.begin(), usedGroups.end(), *group) == usedGroups.end()) keep[i] = false;
            }
            return keep;
        }

    } // namespace

    bool isPlaylist(std::string_view text) {
        return startsWith(text, "#EXTM3U") && text.find('\0') == std::string_view::npos;
    }

    bool isSafeHttpUrl(std::string_view url) {
        // Gli URL diretti (con token di firma) possono superare i 4 KB di isCleanUri: limite a 8 KB.
        if (url.empty() || url.size() > 8192) return false;
        for (unsigned char c : url) if (c <= 0x20 || c >= 0x7F || c == '"') return false;
        size_t rest;
        if (startsWith(url, "https://")) rest = 8;
        else if (startsWith(url, "http://")) rest = 7;
        else return false;
        const size_t hostEnd = url.find_first_of("/?#", rest);
        const std::string_view host = url.substr(rest, hostEnd == std::string_view::npos ? url.size() - rest : hostEnd - rest);
        // Niente credenziali nell'URL ("user:pass@host"): nascondono il vero host a chi legge i log.
        return !host.empty() && host.find('@') == std::string_view::npos;
    }

    std::optional<std::string> remotePrefixOf(std::string_view location) {
        constexpr std::string_view suffix = "/master.m3u8";
        if (!isCleanUri(location)) return std::nullopt;
        const size_t p = location.find(suffix);
        if (p == std::string_view::npos || p == 0) return std::nullopt;
        // Lo schema non deve essere http/https (quelli si aprono direttamente) né locale.
        const size_t colon = location.find("://");
        if (colon == std::string_view::npos || colon == 0 || colon > p) return std::nullopt;
        return std::string(location.substr(0, p));
    }

    std::string toLocal(std::string_view remoteUri, std::string_view remotePrefix) {
        std::string out(kLocalPrefix);
        out.append(remoteUri.substr(std::min(remotePrefix.size(), remoteUri.size())));
        return out;
    }

    std::optional<Master> rewriteMaster(std::string_view text, std::string_view remotePrefix) {
        if (!isPlaylist(text) || remotePrefix.empty()) return std::nullopt;

        // Una "master" che contiene segmenti è in realtà una media playlist: si passa così com'è.
        if (text.find("#EXTINF") != std::string_view::npos) {
            auto media = expandCondensed(text);
            if (!media) return std::nullopt;
            return Master{std::move(*media), {}};
        }

        std::vector<std::string_view> lines;
        if (!splitLines(text, lines)) return std::nullopt;
        const std::vector<bool> keep = preferAacLc(lines);
        Master m;
        m.text.reserve(text.size() + 1024);

        for (size_t li = 0; li < lines.size(); ++li) {
            if (!keep[li]) continue;
            const std::string_view line = lines[li];
            if (line.empty()) {
                m.text.push_back('\n');
                continue;
            }
            if (line.front() == '#') {
                // Tracce alternative: #EXT-X-MEDIA:TYPE=AUDIO,...,URI="mlhls://localhost/..."
                const auto uri = quotedAttr(line, "URI");
                if (uri && startsWith(*uri, remotePrefix)) {
                    if (!isCleanUri(*uri)) return std::nullopt;
                    addUnique(m.mediaUris, *uri);
                    const size_t at = static_cast<size_t>(uri->data() - line.data());
                    m.text.append(line.substr(0, at));
                    m.text.append(toLocal(*uri, remotePrefix));
                    m.text.append(line.substr(at + uri->size()));
                } else {
                    m.text.append(line);
                }
            } else if (startsWith(line, remotePrefix)) {
                // Riga URI di una variante (#EXT-X-STREAM-INF la precede).
                if (!isCleanUri(line)) return std::nullopt;
                addUnique(m.mediaUris, line);
                m.text.append(toLocal(line, remotePrefix));
            } else {
                m.text.append(line);   // URI di altro tipo: il lettore li rifiuterà, non serve scartarli qui
            }
            m.text.push_back('\n');
            if (m.mediaUris.size() > kMaxMediaPlaylists || m.text.size() > kMaxOutputBytes) return std::nullopt;
        }
        if (m.mediaUris.empty()) return std::nullopt;
        return m;
    }

    std::optional<std::string> expandCondensed(std::string_view media) {
        if (!isPlaylist(media)) return std::nullopt;
        const size_t tag = media.find("#YT-EXT-CONDENSED-URL");
        if (tag == std::string_view::npos) {
            std::string out(media);
            if (out.back() != '\n') out.push_back('\n');   // RFC 8216: ogni riga termina con un a capo
            return out;
        }

        // #YT-EXT-CONDENSED-URL:BASE-URI="https://...",PARAMS="id,itag,...",PREFIX="s/"
        size_t tagEnd = media.find('\n', tag);
        if (tagEnd == std::string_view::npos) tagEnd = media.size();
        const std::string_view tagLine = media.substr(tag, tagEnd - tag);
        const auto base = quotedAttr(tagLine, "BASE-URI");
        const auto paramsAttr = quotedAttr(tagLine, "PARAMS");
        const auto prefix = quotedAttr(tagLine, "PREFIX");
        if (!base || !paramsAttr || !prefix || prefix->empty() || paramsAttr->empty()) return std::nullopt;
        // I segmenti espansi diventano indirizzi scaricati dalla TV: devono essere https/http.
        if (!isSafeHttpUrl(*base)) return std::nullopt;

        std::vector<std::string_view> params;
        size_t p = 0;
        while (p <= paramsAttr->size()) {
            size_t comma = paramsAttr->find(',', p);
            if (comma == std::string_view::npos) comma = paramsAttr->size();
            const std::string_view param = paramsAttr->substr(p, comma - p);
            if (!isCleanUri(param)) return std::nullopt;
            params.push_back(param);
            if (params.size() > kMaxParams) return std::nullopt;
            p = comma + 1;
        }

        std::vector<std::string_view> lines;
        if (!splitLines(media, lines)) return std::nullopt;
        std::string out;
        out.reserve(std::min(kMaxOutputBytes, media.size() * 4));

        for (std::string_view line : lines) {
            if (!line.empty() && line.front() != '#' && startsWith(line, *prefix)) {
                // "s/<v1>/<v2>/.../<vN>" -> BASE/<p1>/<v1>/<p2>/<v2>/.../<pN>/<vN>
                // L'ultimo valore prende tutto il resto della riga (può contenere '/').
                std::string_view rest = line.substr(prefix->size());
                out.append(*base);
                out.push_back('/');
                for (size_t i = 0; i < params.size(); ++i) {
                    std::string_view value;
                    if (i + 1 < params.size()) {
                        const size_t slash = rest.find('/');
                        if (slash == std::string_view::npos) return std::nullopt;
                        value = rest.substr(0, slash);
                        rest.remove_prefix(slash + 1);
                    } else {
                        value = rest;
                    }
                    out.append(params[i]);
                    out.push_back('/');
                    out.append(value);
                    if (i + 1 < params.size()) out.push_back('/');
                }
            } else {
                out.append(line);
            }
            out.push_back('\n');
            if (out.size() > kMaxOutputBytes) return std::nullopt;
        }
        return out;
    }

} // namespace ref::hls
