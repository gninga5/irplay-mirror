/*
 * rtsp_message.cpp
 * Vedi rtsp_message.h.
 */
#include "rtsp_message.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

#include "net_util.h"

namespace irplay {

    const std::string* RtspRequest::header(std::string_view name) const {
        for (const auto& [k, v] : headers) if (rtsp::iequals(k, name)) return &v;
        return nullptr;
    }

    namespace rtsp {

        namespace {

            constexpr const char* kServerHeader = "AirTunes/220.68";
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

        bool iequals(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
                if (x != y) return false;
            }
            return true;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
            return s;
        }

        bool isDigits(std::string_view s, size_t maxLen) {
            if (s.empty() || s.size() > maxLen) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            return true;
        }

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

        int discardResponse(std::vector<uint8_t>& buf) {
            const std::string_view view(reinterpret_cast<const char*>(buf.data()), buf.size());
            const size_t end = view.find("\r\n\r\n");
            if (end == std::string_view::npos) return buf.size() > kMaxHeaderBytes ? -1 : 0;
            if (end > kMaxHeaderBytes || view.substr(0, 9) != "HTTP/1.1 ") return -1;

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

        bool sendAll(int fd, const void* data, size_t len) {
            const auto* p = static_cast<const uint8_t*>(data);
            while (len > 0) {
                const ssize_t n = ::send(fd, p, len, net::kSendFlags);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                p += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        bool sendAllTimed(int fd, const void* data, size_t len, int timeoutMs) {
            using Clock = std::chrono::steady_clock;
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
            const auto* p = static_cast<const uint8_t*>(data);
            while (len > 0) {
                const ssize_t n = ::send(fd, p, len, net::kSendFlags | MSG_DONTWAIT);
                if (n >= 0) {
                    p += n;
                    len -= static_cast<size_t>(n);
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                if (left <= 0) return false;
                pollfd pf{fd, POLLOUT, 0};
                if (::poll(&pf, 1, static_cast<int>(left)) <= 0) return false;
            }
            return true;
        }

        bool sendResponse(int fd, const RtspRequest* req, int code, const char* reason,
                          const Headers& extra, const std::vector<uint8_t>& body) {
            const std::string proto = req ? req->protocol : "RTSP/1.0";
            std::string out = proto + " " + std::to_string(code) + " " + reason + "\r\n";
            if (req) {
                const std::string* cseq = req->header("CSeq");
                if (cseq && isDigits(*cseq, 10)) out += "CSeq: " + *cseq + "\r\n";
            }
            out += std::string("Server: ") + kServerHeader + "\r\n";
            for (const auto& [k, v] : extra) out += k + ": " + v + "\r\n";
            // 101 (passaggio al canale inverso) non ha corpo né Content-Length.
            if (code != 101) out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            out += "\r\n";
            if (!sendAll(fd, out.data(), out.size())) return false;
            return body.empty() || sendAll(fd, body.data(), body.size());
        }

    } // namespace rtsp

} // namespace irplay
