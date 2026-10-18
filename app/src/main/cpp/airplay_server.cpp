/*
 * airplay_server.cpp
 * Scopo: ciclo di accettazione TCP e parser RTSP/HTTP robusto contro input ostile.
 *
 * Modello a thread: un thread per l'accept più un thread per ogni connessione
 * (iOS ne apre 1 o 2 per sessione). Il limite kMaxClients impedisce a un host
 * della LAN di esaurire thread e descrittori di file della TV, che su chiavette
 * da 1-2 GB di RAM porterebbe il Low Memory Killer a terminare l'app.
 *
 * Il socket è dual-stack (IPv6 con IPV6_V6ONLY=0): NsdManager annuncia sia il
 * record A sia AAAA e iOS spesso si collega via IPv6 link-local.
 */
#include "airplay_server.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#define TAG "IcAst_RTSP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace icast {
    namespace {

        constexpr size_t kMaxHeaderBytes = 16 * 1024;
        constexpr size_t kMaxBodyBytes   = 64 * 1024;   // fp-setup = 164 B, i plist di SETUP pesano pochi KB
        constexpr size_t kMaxHeaders     = 64;
        constexpr size_t kMaxClients     = 4;
        constexpr int    kIdleTimeoutSec = 60;          // iOS manda GET_PARAMETER/feedback ogni ~2 s durante una sessione
        constexpr const char* kServerHeader = "AirTunes/220.68";
        constexpr const char* kPublic =
                "SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, ANNOUNCE";

        using Headers = std::vector<std::pair<std::string, std::string>>;

        struct Request {
            std::string method, uri, protocol;
            Headers headers;
            std::vector<uint8_t> body;

            const std::string* header(std::string_view name) const;
        };

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

        const std::string* Request::header(std::string_view name) const {
            for (const auto& [k, v] : headers) if (iequals(k, name)) return &v;
            return nullptr;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
            return s;
        }

// Solo cifre decimali, niente segno né spazi interni, con limite massimo:
// rifiuta valori come "-1", "1e9" o "123abc" invece di interpretarli a metà.
        bool parseContentLength(std::string_view v, size_t& out) {
            if (v.empty() || v.size() > 7) return false;
            size_t n = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return false;
                n = n * 10 + static_cast<size_t>(c - '0');
            }
            if (n > kMaxBodyBytes) return false;
            out = n;
            return true;
        }

        bool isDigits(std::string_view s, size_t maxLen) {
            if (s.empty() || s.size() > maxLen) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            return true;
        }

// Restituisce 1 se ha estratto una richiesta completa (rimossa da buf),
// 0 se servono altri byte, -1 se la richiesta è malformata o supera i limiti.
        int parseRequest(std::vector<uint8_t>& buf, Request& req) {
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

            req = Request{};
            req.method   = std::string(line.substr(0, sp1));
            req.uri      = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
            req.protocol = std::string(line.substr(sp2 + 1));
            if (req.protocol != "RTSP/1.0" && req.protocol != "HTTP/1.1") return -1;

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
                    if (haveLength || !parseContentLength(value, contentLength)) return -1;
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

        bool sendAll(int fd, const void* data, size_t len) {
            const auto* p = static_cast<const uint8_t*>(data);
            while (len > 0) {
                // MSG_NOSIGNAL: se il client chiude la connessione, il processo non riceve SIGPIPE (che lo terminerebbe).
                const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                p += n;
                len -= static_cast<size_t>(n);
            }
            return true;
        }

        bool sendResponse(int fd, const Request* req, int code, const char* reason,
                          const Headers& extra, const std::vector<uint8_t>& body) {
            const std::string proto = req ? req->protocol : "RTSP/1.0";
            std::string out = proto + " " + std::to_string(code) + " " + reason + "\r\n";
            if (req) {
                const std::string* cseq = req->header("CSeq");
                if (cseq && isDigits(*cseq, 10)) out += "CSeq: " + *cseq + "\r\n";
            }
            out += std::string("Server: ") + kServerHeader + "\r\n";
            for (const auto& [k, v] : extra) out += k + ": " + v + "\r\n";
            out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            if (!sendAll(fd, out.data(), out.size())) return false;
            return body.empty() || sendAll(fd, body.data(), body.size());
        }

// Restituisce false se la connessione va chiusa.
        bool dispatch(int fd, const Request& req, const std::string& peer) {
            // Nel log vanno solo metodo e URI (troncato): i corpi conterranno chiavi.
            LOGI("%s -> %s %.*s", peer.c_str(), req.method.c_str(),
                 static_cast<int>(std::min<size_t>(req.uri.size(), 128)), req.uri.c_str());

            if (req.method == "OPTIONS") {
                return sendResponse(fd, &req, 200, "OK", {{"Public", kPublic}}, {});
            }
            LOGW("Metodo non ancora gestito: %s", req.method.c_str());
            return sendResponse(fd, &req, 501, "Not Implemented", {}, {});
        }

        std::string peerString(const sockaddr_storage& ss) {
            char ip[INET6_ADDRSTRLEN] = {};
            if (ss.ss_family == AF_INET6) {
                const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
                inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
            } else if (ss.ss_family == AF_INET) {
                const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
                inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
            }
            return ip;
        }

    } // namespace

    AirPlayServer::~AirPlayServer() { stop(); }

    int AirPlayServer::start(uint16_t preferredPort) {
        if (running_) return -1;

        const int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { LOGE("socket() fallita: errno %d", errno); return -1; }

        const int off = 0, one = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);   // accetta anche IPv4 (mappato)
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   // riavvio rapido dopo stop()

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(preferredPort);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            LOGW("Porta %u occupata (errno %d), uso una porta di sistema", preferredPort, errno);
            addr.sin6_port = 0;
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                LOGE("bind() fallita: errno %d", errno);
                ::close(fd);
                return -1;
            }
        }
        if (::listen(fd, 8) != 0) {
            LOGE("listen() fallita: errno %d", errno);
            ::close(fd);
            return -1;
        }

        sockaddr_in6 bound{};
        socklen_t len = sizeof bound;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            ::close(fd);
            return -1;
        }

        listenFd_ = fd;
        running_ = true;
        acceptThread_ = std::thread(&AirPlayServer::acceptLoop, this);
        const int port = ntohs(bound.sin6_port);
        LOGI("Server RTSP in ascolto sulla porta %d", port);
        return port;
    }

    void AirPlayServer::stop() {
        if (!running_.exchange(false)) return;

        // shutdown() sblocca accept(), che ritorna con EINVAL e fa terminare il thread di accettazione.
        ::shutdown(listenFd_, SHUT_RDWR);
        if (acceptThread_.joinable()) acceptThread_.join();
        ::close(listenFd_);
        listenFd_ = -1;

        std::vector<std::unique_ptr<Client>> clients;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& c : clients_) if (c->fd >= 0) ::shutdown(c->fd, SHUT_RDWR);   // sblocca recv()
            clients.swap(clients_);
        }
        for (auto& c : clients) if (c->thread.joinable()) c->thread.join();
        LOGI("Server RTSP fermato");
    }

    void AirPlayServer::reapFinishedLocked() {
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->done) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void AirPlayServer::acceptLoop() {
        while (running_) {
            sockaddr_storage ss{};
            socklen_t len = sizeof ss;
            const int fd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_CLOEXEC);
            if (fd < 0) {
                if (!running_) break;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                LOGW("accept() fallita: errno %d", errno);
                ::usleep(100 * 1000);   // es. EMFILE: evita un ciclo a vuoto che occupa il 100% della CPU
                continue;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            reapFinishedLocked();
            if (clients_.size() >= kMaxClients) {
                LOGW("Troppi client contemporanei, connessione rifiutata");
                ::close(fd);
                continue;
            }
            auto client = std::make_unique<Client>();
            client->fd = fd;
            Client* raw = client.get();
            client->thread = std::thread(&AirPlayServer::handleConnection, this, raw, peerString(ss));
            clients_.push_back(std::move(client));
        }
    }

    void AirPlayServer::handleConnection(Client* client, std::string peer) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd = client->fd;
        }
        LOGI("Connessione da %s", peer.c_str());

        timeval tv{kIdleTimeoutSec, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // risposte RTSP piccole: niente ritardo di Nagle

        std::vector<uint8_t> buf;
        buf.reserve(4096);
        uint8_t chunk[4096];
        bool keepOpen = true;

        while (keepOpen && running_) {
            const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
            if (n == 0) break;                       // il client ha chiuso
            if (n < 0) {
                if (errno == EINTR) continue;
                break;                               // timeout di inattività o errore
            }
            buf.insert(buf.end(), chunk, chunk + n);
            if (buf.size() > kMaxHeaderBytes + kMaxBodyBytes + 4) {
                LOGW("Buffer oltre il limite da %s, chiudo", peer.c_str());
                break;
            }
            for (;;) {
                Request req;
                const int r = parseRequest(buf, req);
                if (r == 0) break;
                if (r < 0) {
                    LOGW("Richiesta malformata da %s", peer.c_str());
                    sendResponse(fd, nullptr, 400, "Bad Request", {}, {});
                    keepOpen = false;
                    break;
                }
                if (!dispatch(fd, req, peer)) { keepOpen = false; break; }
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ::close(client->fd);
        client->fd = -1;
        client->done = true;
        LOGI("Connessione chiusa: %s", peer.c_str());
    }

} // namespace icast