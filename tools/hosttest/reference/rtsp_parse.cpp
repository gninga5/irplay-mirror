/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 4).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp, che confronta
 * il risultato del parser Rust con questo su milioni di input validi e alterati.
 */
#include "reference.h"

#include <cctype>

namespace ref::rtsp {
    using namespace irplay;
    using namespace irplay::rtsp;
    using irplay::rtsp::iequals;
    using irplay::rtsp::trim;

        namespace {
            constexpr size_t kMaxHeaders = 64;
            // Solo cifre decimali, niente segno né spazi interni, con limite massimo:
            // rifiuta valori come "-1", "1e9" o "123abc" invece di interpretarli a metà.
            bool parseContentLength(std::string_view v, size_t maxBody, size_t& out) {
                if (v.empty() || v.size() > 8) return false;
                size_t n = 0;
                for (char c : v) {
                    if (c < '0' || c > '9') return false;
                    n = n * 10 + static_cast<size_t>(c - '0');
                }
                if (n > maxBody) return false;
                out = n;
                return true;
            }

            // Il limite al corpo dipende dalla richiesta, nota dalla prima riga: solo le
            // copertine (SET_PARAMETER) e le playlist HLS (POST /action) possono essere grandi.
            size_t maxBodyFor(const std::string& method, const std::string& uri) {
                if (method == "SET_PARAMETER") return kMaxArtworkBytes;
                if (method == "POST" && uri == "/action") return kMaxPlaylistBytes;
                return kMaxBodyBytes;
            }

        } // namespace

        int parseRequest(std::vector<uint8_t>& buf, RtspRequest& req) {
            const std::string_view view(reinterpret_cast<const char*>(buf.data()), buf.size());
            const size_t end = view.find("\r\n\r\n");
            if (end == std::string_view::npos) return buf.size() > kMaxHeaderBytes ? -1 : 0;
            if (end > kMaxHeaderBytes) return -1;

            const std::string_view head = view.substr(0, end);
            const size_t eol = head.find("\r\n");
            const std::string_view line = head.substr(0, eol);

            const size_t sp1 = line.find(' ');
            const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
            if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0 || sp2 == sp1 + 1)
                return -1;

            req = RtspRequest{};
            req.method   = std::string(line.substr(0, sp1));
            req.uri      = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
            req.protocol = std::string(line.substr(sp2 + 1));
            if (req.protocol != "RTSP/1.0" && req.protocol != "HTTP/1.1") return -1;
            const size_t maxBody = maxBodyFor(req.method, req.uri);

            size_t contentLength = 0;
            bool haveLength = false;
            size_t pos = eol == std::string_view::npos ? head.size() : eol + 2;
            while (pos < head.size()) {
                size_t next = head.find("\r\n", pos);
                if (next == std::string_view::npos) next = head.size();
                const std::string_view h = head.substr(pos, next - pos);
                const size_t colon = h.find(':');
                if (colon == std::string_view::npos || colon == 0) return -1;
                const std::string_view name = trim(h.substr(0, colon));
                const std::string_view value = trim(h.substr(colon + 1));

                // Due Content-Length, o Transfer-Encoding, permetterebbero di far leggere
                // al server una richiesta "nascosta" dentro un'altra: si scarta tutto.
                if (iequals(name, "Transfer-Encoding")) return -1;
                if (iequals(name, "Content-Length")) {
                    if (haveLength || !parseContentLength(value, maxBody, contentLength)) return -1;
                    haveLength = true;
                }
                if (req.headers.size() >= kMaxHeaders) return -1;
                req.headers.emplace_back(std::string(name), std::string(value));
                pos = next + 2;
            }

            const size_t total = end + 4 + contentLength;   // nessun overflow: entrambi i termini sono limitati
            if (buf.size() < total) return 0;
            req.body.assign(buf.begin() + static_cast<long>(end + 4), buf.begin() + static_cast<long>(total));
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(total));
            return 1;
        }

        namespace {
            bool isStatusLine(std::string_view head) {
                size_t i = 0;
                while (i < head.size() && i < 16 && head[i] >= 'A' && head[i] <= 'Z') ++i;
                if (i == 0 || i + 9 > head.size() || head[i] != '/') return false;
                const std::string_view rest = head.substr(i + 1);
                // "1.1 200"
                return rest.size() >= 7 && std::isdigit(static_cast<unsigned char>(rest[0])) && rest[1] == '.' &&
                       std::isdigit(static_cast<unsigned char>(rest[2])) && rest[3] == ' ' &&
                       std::isdigit(static_cast<unsigned char>(rest[4])) && std::isdigit(static_cast<unsigned char>(rest[5])) &&
                       std::isdigit(static_cast<unsigned char>(rest[6]));
            }
        }

        int discardResponse(std::vector<uint8_t>& buf) {
            const std::string_view view(reinterpret_cast<const char*>(buf.data()), buf.size());
            const size_t end = view.find("\r\n\r\n");
            if (end == std::string_view::npos) return buf.size() > kMaxHeaderBytes ? -1 : 0;
            // Riga di stato "NOME/x.y codice ...": iOS risponde agli eventi con HTTP/1.1, ma
            // ad alcuni con EVENT/1.0 (visto con l'app YouTube dopo ~13 s di video).
            if (end > kMaxHeaderBytes || !isStatusLine(view.substr(0, end))) return -1;

            // Le risposte di iOS a /event non hanno corpo, ma un Content-Length va rispettato
            // per non interpretare il corpo come l'inizio della risposta successiva.
            size_t contentLength = 0;
            bool haveLength = false;
            const std::string_view head = view.substr(0, end);
            size_t pos = head.find("\r\n");
            while (pos != std::string_view::npos && pos + 2 < head.size()) {
                pos += 2;
                size_t next = head.find("\r\n", pos);
                const std::string_view h = head.substr(pos, next == std::string_view::npos ? head.size() - pos : next - pos);
                const size_t colon = h.find(':');
                if (colon != std::string_view::npos) {
                    const std::string_view name = trim(h.substr(0, colon));
                    if (iequals(name, "Transfer-Encoding")) return -1;
                    if (iequals(name, "Content-Length")) {
                        if (haveLength || !parseContentLength(trim(h.substr(colon + 1)), kMaxBodyBytes, contentLength))
                            return -1;
                        haveLength = true;
                    }
                }
                pos = next;
            }
            const size_t total = end + 4 + contentLength;
            if (buf.size() < total) return 0;
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(total));
            return 1;
        }


} // namespace ref::rtsp
