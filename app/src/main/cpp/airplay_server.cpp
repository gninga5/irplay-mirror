/*
 * airplay_server.cpp
 * Scopo: ciclo di accettazione TCP, parser RTSP/HTTP robusto contro input ostile e
 * gestione dei metodi del protocollo AirPlay (mirroring, AirPlay 1 con pairing legacy).
 *
 * Modello a thread: un thread per l'accept più un thread per ogni connessione
 * (iOS ne apre 1 o 2 per sessione). Il limite kMaxClients impedisce a un host
 * della LAN di esaurire thread e descrittori di file della TV, che su chiavette
 * da 1-2 GB di RAM porterebbe il Low Memory Killer a terminare l'app.
 *
 * Watchdog: recv() sulla connessione RTSP ha un timeout di 1 s. A ogni scadenza si
 * controlla l'ultima attività della sessione (pacchetti video/audio, richieste RTSP).
 * Con il video connesso, 3 s senza nulla significano iPhone spento o fuori portata:
 * la sessione viene chiusa senza aspettare il timeout TCP (che può durare minuti).
 */
#define LOG_TAG "iRPlay_RTSP"
#include "airplay_server.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <netinet/tcp.h>
#include <poll.h>
#include <string_view>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#include <mbedtls/platform_util.h>
#include <sodium.h>

#include "log.h"
#include "media_streams.h"
#include "net_util.h"
#include "now_playing.h"
#include "plist_reader.h"

namespace irplay {

    struct RtspRequest {
        std::string method, uri, protocol;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<uint8_t> body;

        const std::string* header(std::string_view name) const;
    };

    struct AirPlayServer::ConnState {
        Client* client = nullptr;
        int fd = -1;
        sockaddr_in6 peer{};
        crypto::PairingSession pairing;
        crypto::FairPlay fairplay;
        std::unique_ptr<Session> session;
    };

    namespace {

        constexpr size_t kMaxHeaderBytes = 16 * 1024;
        constexpr size_t kMaxBodyBytes   = 64 * 1024;   // fp-setup = 164 B, i plist di SETUP pesano pochi KB
        // Unica eccezione: la copertina dell'album (SET_PARAMETER image/jpeg) può superare i 64 KB.
        constexpr size_t kMaxArtworkBytes = 2 * 1024 * 1024;
        constexpr size_t kMaxHeaders     = 64;
        constexpr size_t kMaxClients     = 6;
        constexpr int    kIdleTimeoutSec = 60;          // connessione senza sessione
        constexpr int64_t kWatchdogMs    = 3000;        // video connesso: iOS invia dati almeno ogni secondo
        constexpr int64_t kSessionIdleMs = 60000;       // sessione aperta ma video non ancora connesso
        constexpr size_t kMaxClientName  = 64;
        constexpr int64_t kPinLifetimeMs = 120000;      // un PIN vale 2 minuti
        constexpr int kPinMaxFailures    = 3;           // poi blocco: 60 s, 120 s, 240 s ... fino a 64 min
        constexpr size_t kMaxTrusted     = 64;
        constexpr const char* kServerHeader = "AirTunes/220.68";
        constexpr const char* kPublic =
                "SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER, POST, GET";
        constexpr const char* kOctet = "application/octet-stream";
        constexpr const char* kBplist = "application/x-apple-binary-plist";

        using Headers = std::vector<std::pair<std::string, std::string>>;

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

// Solo cifre decimali, niente segno né spazi interni, con limite massimo:
// rifiuta valori come "-1", "1e9" o "123abc" invece di interpretarli a metà.
        bool parseContentLength(std::string_view v, size_t maxBody, size_t& out) {
            if (v.empty() || v.size() > 7) return false;
            size_t n = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return false;
                n = n * 10 + static_cast<size_t>(c - '0');
            }
            if (n > maxBody) return false;
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
                    // Il limite dipende dal metodo, già noto dalla prima riga: solo SET_PARAMETER
                    // (copertine) può avere un corpo grande.
                    const size_t maxBody = req.method == "SET_PARAMETER" ? kMaxArtworkBytes : kMaxBodyBytes;
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

        bool sendResponse(int fd, const RtspRequest* req, int code, const char* reason,
                          const Headers& extra = {}, const std::vector<uint8_t>& body = {}) {
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

        bool startsWith(const std::vector<uint8_t>& body, std::string_view prefix) {
            return body.size() >= prefix.size() && std::memcmp(body.data(), prefix.data(), prefix.size()) == 0;
        }

        // "volume: -12.500000\r\n" -> -12.5. Accetta solo un numero finito tra -144 e 0.
        bool parseVolume(const std::vector<uint8_t>& body, float& out) {
            constexpr std::string_view prefix = "volume:";
            if (!startsWith(body, prefix) || body.size() > 64) return false;
            std::string s(body.begin() + prefix.size(), body.end());
            const size_t first = s.find_first_not_of(' ');
            if (first == std::string::npos) return false;
            const char* begin = s.c_str() + first;
            char* end = nullptr;
            const float v = std::strtof(begin, &end);
            if (end == begin || (*end != '\0' && *end != '\r' && *end != '\n')) return false;
            if (!std::isfinite(v) || v < -144.0f || v > 0.0f) return false;
            out = v;
            return true;
        }

        // Nome del dispositivo: massimo kMaxClientName byte, niente caratteri di controllo,
        // troncato al confine di un carattere UTF-8.
        std::string sanitizeName(const std::string& in) {
            std::string out;
            for (unsigned char c : in) {
                if (c < 0x20 || c == 0x7F) continue;
                out.push_back(static_cast<char>(c));
            }
            if (out.size() > kMaxClientName) {
                size_t cut = kMaxClientName;
                while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
                out.resize(cut);
            }
            return out;
        }

        const uint64_t* findUInt(const plist::Dict& d, std::string_view key) {
            const plist::Value* v = d.find(key);
            return v ? v->asUInt() : nullptr;
        }

    } // namespace

    const std::string* RtspRequest::header(std::string_view name) const {
        for (const auto& [k, v] : headers) if (iequals(k, name)) return &v;
        return nullptr;
    }

    // =============================================================== dispatch

    bool AirPlayServer::dispatch(ConnState& st, const RtspRequest& req) {
        // Nel log vanno solo metodo e URI (troncato): i corpi contengono chiavi.
        LOGI("%s %.*s", req.method.c_str(), static_cast<int>(std::min<size_t>(req.uri.size(), 64)), req.uri.c_str());
        if (st.session) st.session->touch();
        const int fd = st.fd;
        const std::string& m = req.method;
        const std::string& u = req.uri;
        const bool rtsp = req.protocol == "RTSP/1.0";
        Headers jack;
        if (rtsp) jack.emplace_back("Audio-Jack-Status", "connected; type=analog");

        if (m == "OPTIONS") {
            jack.emplace_back("Public", kPublic);
            return sendResponse(fd, &req, 200, "OK", jack);
        }

        if (m == "GET" && u == "/info") {
            const bool txtOnly = requestsTxtAirPlay(req.body);
            return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kBplist}},
                                txtOnly ? info_.txtOnly : info_.full);
        }

        if (m == "POST" && u == "/pair-pin-start") return handlePinStart(st, req);
        if (m == "POST" && u == "/pair-setup-pin") return handlePinSetup(st, req);

        if (m == "POST" && u == "/pair-setup") {
            {
                // Con il PIN attivo il pair-setup "semplice" aggirerebbe il PIN: si rifiuta.
                std::lock_guard<std::mutex> lock(pinMutex_);
                if (pinMode_ != PinMode::Off) {
                    sendResponse(fd, &req, 470, "Connection Authorization Required");
                    return false;
                }
            }
            if (!st.pairing.setup(req.body.data(), req.body.size())) {
                sendResponse(fd, &req, 400, "Bad Request");
                return false;
            }
            const uint8_t* pk = identity_->publicKey();
            return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kOctet}}, std::vector<uint8_t>(pk, pk + 32));
        }

        if (m == "POST" && u == "/pair-verify") {
            if (!req.body.empty() && req.body[0] == 1) {
                if (req.body.size() == 68 && !clientAllowed(st, req.body.data() + 36)) {
                    // Dispositivo non associato: risposta vuota, come fa UxPlay. iOS la interpreta
                    // come "serve il PIN" e passa a pair-pin-start sulla stessa connessione.
                    LOGI("Dispositivo non associato: serve il PIN");
                    return sendResponse(fd, &req, 200, "OK");
                }
                std::array<uint8_t, 96> out{};
                if (!st.pairing.verifyStep1(*identity_, req.body.data(), req.body.size(), out)) {
                    LOGW("pair-verify fase 1 rifiutata");
                    sendResponse(fd, &req, 400, "Bad Request");
                    return false;
                }
                return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kOctet}},
                                    std::vector<uint8_t>(out.begin(), out.end()));
            }
            if (!req.body.empty() && req.body[0] == 0) {
                if (!st.pairing.verifyStep2(req.body.data(), req.body.size())) {
                    // Firma non valida: chiave del client manomessa o messaggio alterato. Si chiude.
                    LOGW("pair-verify: firma del client non valida");
                    sendResponse(fd, &req, 470, "Connection Authorization Required");
                    return false;
                }
                LOGI("pair-verify completato");
                return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kOctet}});
            }
            sendResponse(fd, &req, 400, "Bad Request");
            return false;
        }

        if (m == "POST" && u == "/fp-setup") {
            std::vector<uint8_t> out;
            if (!st.fairplay.handle(req.body.data(), req.body.size(), out)) {
                LOGW("fp-setup non valido (%zu byte)", req.body.size());
                sendResponse(fd, &req, 400, "Bad Request");
                return false;
            }
            return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kOctet}}, out);
        }

        if (m == "SETUP") return handleSetup(st, req);
        if (m == "TEARDOWN") return handleTeardown(st, req);

        if (m == "RECORD") {
            if (!st.session) return sendResponse(fd, &req, 455, "Method Not Valid in This State");
            // Latenza audio dichiarata a iOS, in campioni a 44,1 kHz (0,25 s).
            return sendResponse(fd, &req, 200, "OK",
                                {{"Audio-Latency", "11025"}, {"Audio-Jack-Status", "connected; type=analog"}});
        }

        if (m == "GET_PARAMETER") {
            if (startsWith(req.body, "volume")) {
                char text[40];
                std::snprintf(text, sizeof text, "volume: %.6f\r\n", static_cast<double>(volumeDb_.load()));
                jack.emplace_back("Content-Type", "text/parameters");
                return sendResponse(fd, &req, 200, "OK", jack,
                                    std::vector<uint8_t>(text, text + std::strlen(text)));
            }
            return sendResponse(fd, &req, 200, "OK", jack);
        }

        if (m == "SET_PARAMETER") {
            const std::string* ct = req.header("Content-Type");
            float db;
            int64_t pos, dur;
            if (ct && iequals(*ct, "text/parameters")) {
                if (parseVolume(req.body, db)) {
                    volumeDb_ = db;
                    sink_->onVolume(db);
                } else if (nowplaying::parseProgress(req.body.data(), req.body.size(), pos, dur)) {
                    sink_->onProgress(pos, dur);
                }
            } else if (ct && iequals(*ct, "application/x-dmap-tagged")) {
                nowplaying::Metadata md;
                if (nowplaying::parseDmap(req.body.data(), req.body.size(), md)) {
                    sink_->onMetadata(md.title, md.artist, md.album);
                }
            } else if (ct && (iequals(*ct, "image/jpeg") || iequals(*ct, "image/png") || iequals(*ct, "image/none"))) {
                // Si inoltrano solo immagini che iniziano davvero come JPEG (FF D8 FF) o PNG
                // (89 50 4E 47): il Content-Type lo decide il mittente e non basta.
                const auto& b = req.body;
                const bool jpeg = b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
                const bool png = b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
                if (jpeg || png) sink_->onArtwork(b.data(), b.size());
                else if (b.empty()) sink_->onArtwork(nullptr, 0);   // brano senza copertina
            }
            return sendResponse(fd, &req, 200, "OK", jack);
        }

        if (m == "FLUSH") {
            // "RTP-Info: seq=1234;rtptime=..." indica il primo pacchetto valido dopo la pausa o il
            // salto: quelli precedenti in coda vanno scartati.
            int next = -1;
            if (const std::string* info = req.header("RTP-Info")) {
                const size_t p = info->find("seq=");
                if (p != std::string::npos) {
                    uint32_t v = 0;
                    size_t i = p + 4, digits = 0;
                    while (i < info->size() && digits < 6 && (*info)[i] >= '0' && (*info)[i] <= '9') {
                        v = v * 10 + static_cast<uint32_t>((*info)[i] - '0');
                        ++i;
                        ++digits;
                    }
                    if (digits > 0 && v <= 0xFFFF) next = static_cast<int>(v);
                }
            }
            if (st.session) st.session->flushAudio(next);
            return sendResponse(fd, &req, 200, "OK", jack);
        }

        if (m == "POST" && (u == "/feedback" || u == "/audioMode" || u == "/command")) {
            return sendResponse(fd, &req, 200, "OK", jack);
        }

        LOGW("Metodo non gestito: %s", m.c_str());
        return sendResponse(fd, &req, 501, "Not Implemented");
    }

    // =============================================================== PIN

    bool AirPlayServer::clientAllowed(ConnState& st, const uint8_t clientEdKey[32]) {
        std::lock_guard<std::mutex> lock(pinMutex_);
        if (pinMode_ == PinMode::Off) return true;
        // PIN appena inserito su questa connessione: vale solo per la chiave che l'ha usato.
        if (st.pairing.pinAuthenticated()) return sodium_memcmp(clientEdKey, st.pairing.pinClientKey(), 32) == 0;
        if (pinMode_ == PinMode::Always) return false;
        for (const auto& k : trusted_) {
            if (sodium_memcmp(k.data(), clientEdKey, 32) == 0) return true;
        }
        return false;
    }

    /**
     * PIN attivo, o uno nuovo se non c'è o è scaduto. Va chiamata con pinMutex_ tenuto.
     * Un solo PIN alla volta: iOS può chiedere pair-pin-start più volte per la stessa richiesta
     * e riprovare pair-setup-pin su connessioni nuove ogni volta che l'utente ridigita il codice;
     * se la TV generasse un PIN nuovo a ogni richiesta, quello a schermo non corrisponderebbe più
     * a quello che l'iPhone sta chiedendo.
     */
    std::string AirPlayServer::currentOrNewPinLocked(bool& isNew) {
        const int64_t now = monotonicMs();
        isNew = pin_.empty() || now > pinExpiresMs_;
        if (isNew) {
            // 4 cifre uniformi dal generatore crittografico (randombytes_uniform evita il bias del modulo).
            char buf[8];
            std::snprintf(buf, sizeof buf, "%04u", static_cast<unsigned>(randombytes_uniform(10000)));
            pin_ = buf;
            sodium_memzero(buf, sizeof buf);
            pinExpiresMs_ = now + kPinLifetimeMs;
            pinFailures_ = 0;
        }
        return pin_;
    }

    bool AirPlayServer::handlePinStart(ConnState& st, const RtspRequest& req) {
        std::string pin;
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(pinMutex_);
            if (pinMode_ == PinMode::Off) return sendResponse(st.fd, &req, 404, "Not Found");
            const int64_t now = monotonicMs();
            if (now < lockedUntilMs_) {
                LOGW("Troppi PIN sbagliati: richieste bloccate per %lld s",
                     static_cast<long long>((lockedUntilMs_ - now) / 1000));
                return sendResponse(st.fd, &req, 503, "Service Unavailable");
            }
            pin = currentOrNewPinLocked(isNew);
        }
        LOGI(isNew ? "Richiesto il PIN: mostrato sulla TV" : "Richiesto di nuovo il PIN: resta quello a schermo");
        sink_->onPinRequested(pin);   // il PIN non va mai nel log, solo a schermo
        sodium_memzero(pin.data(), pin.size());
        return sendResponse(st.fd, &req, 200, "OK");
    }

    bool AirPlayServer::handlePinSetup(ConnState& st, const RtspRequest& req) {
        const int fd = st.fd;
        {
            std::lock_guard<std::mutex> lock(pinMutex_);
            if (pinMode_ == PinMode::Off) {
                sendResponse(fd, &req, 470, "Client Authentication Failure");
                return false;
            }
        }
        const auto root = plist::parseBinary(req.body.data(), req.body.size());
        const plist::Dict* d = root ? root->asDict() : nullptr;
        if (d == nullptr) {
            sendResponse(fd, &req, 400, "Bad Request");
            return false;
        }
        auto fail = [&]() {
            sendResponse(fd, &req, 470, "Client Authentication Failure");
            return false;
        };
        const plist::Value* method = d->find("method");
        const plist::Value* user = d->find("user");
        const plist::Value* pk = d->find("pk");
        const plist::Value* proof = d->find("proof");
        const plist::Value* epk = d->find("epk");
        const plist::Value* tag = d->find("authTag");

        // ---- fase 1: {method: "pin", user: <id>} -> {pk: B, salt}
        if (method && user && method->asString() && user->asString()) {
            if (*method->asString() != "pin") return fail();
            std::string pin;
            bool showNew = false;
            {
                std::lock_guard<std::mutex> lock(pinMutex_);
                if (monotonicMs() < lockedUntilMs_) return fail();
                if (pin_.empty() || monotonicMs() > pinExpiresMs_) {
                    // iOS riprova senza pair-pin-start (l'utente ridigita il codice dopo che il
                    // PIN precedente è stato usato o è scaduto). Si mostra un PIN nuovo e si
                    // rifiuta questo tentativo: iOS chiede di nuovo il codice, che ora è a schermo.
                    bool isNew = false;
                    pin = currentOrNewPinLocked(isNew);
                    showNew = true;
                    LOGW("pair-setup-pin senza un PIN attivo: ne mostro uno nuovo");
                } else {
                    pin = pin_;
                }
            }
            if (showNew) {
                sink_->onPinRequested(pin);
                sodium_memzero(pin.data(), pin.size());
                return fail();
            }
            std::vector<uint8_t> salt, B;
            const bool ok = st.pairing.pinBegin(*user->asString(), pin, salt, B);
            sodium_memzero(pin.data(), pin.size());
            if (!ok) return fail();
            plist::Dict r;
            r.set("pk", std::move(B)).set("salt", std::move(salt));
            const auto bin = plist::toBinary(r);
            return bin && sendResponse(fd, &req, 200, "OK", {{"Content-Type", kBplist}}, *bin);
        }

        // ---- fase 2: {pk: A, proof: M1} -> {proof: M2}
        if (pk && proof && pk->asData() && proof->asData()) {
            std::array<uint8_t, srp::kProofSize> M2{};
            const auto& A = *pk->asData();
            const auto& M1 = *proof->asData();
            if (!st.pairing.pinProof(A.data(), A.size(), M1.data(), M1.size(), M2)) {
                // Il PIN resta valido (e a schermo) per ridigitarlo, ma ogni PIN ammette al massimo
                // kPinMaxFailures errori: poi viene ritirato e scatta il blocco, che raddoppia.
                bool locked = false;
                {
                    std::lock_guard<std::mutex> lock(pinMutex_);
                    if (++pinFailures_ >= kPinMaxFailures) {
                        pin_.clear();
                        pinFailures_ = 0;
                        const int shift = lockLevel_ < 6 ? lockLevel_ : 6;
                        lockedUntilMs_ = monotonicMs() + (60000LL << shift);
                        ++lockLevel_;
                        locked = true;
                        LOGW("PIN sbagliato più volte: nuovi tentativi bloccati per %lld s",
                             static_cast<long long>((60000LL << shift) / 1000));
                    }
                }
                LOGW("PIN sbagliato");
                if (locked) sink_->onPinDismissed();
                return fail();
            }
            plist::Dict r;
            r.set("proof", plist::Data(M2.begin(), M2.end()));
            const auto bin = plist::toBinary(r);
            return bin && sendResponse(fd, &req, 200, "OK", {{"Content-Type", kBplist}}, *bin);
        }

        // ---- fase 3: {epk, authTag} (chiave del client cifrata) -> {epk, authTag} (la nostra)
        if (epk && tag && epk->asData() && tag->asData()) {
            std::array<uint8_t, 32> outEpk{};
            std::array<uint8_t, 16> outTag{};
            const auto& e = *epk->asData();
            const auto& t = *tag->asData();
            if (!st.pairing.pinExchange(*identity_, e.data(), e.size(), t.data(), t.size(), outEpk, outTag)) {
                LOGW("pair-setup-pin fase 3 non valida");
                return fail();
            }
            bool newTrusted = false;
            {
                std::lock_guard<std::mutex> lock(pinMutex_);
                pin_.clear();
                pinFailures_ = 0;
                lockLevel_ = 0;
                if (pinMode_ == PinMode::FirstTime) {
                    std::array<uint8_t, 32> key{};
                    std::memcpy(key.data(), st.pairing.pinClientKey(), 32);
                    bool known = false;
                    for (const auto& k : trusted_) known = known || sodium_memcmp(k.data(), key.data(), 32) == 0;
                    if (!known) {
                        if (trusted_.size() >= kMaxTrusted) trusted_.erase(trusted_.begin());   // il più vecchio
                        trusted_.push_back(key);
                        newTrusted = true;
                    }
                }
            }
            LOGI("Dispositivo associato con il PIN");
            if (newTrusted) sink_->onClientTrusted(st.pairing.pinClientKey());
            sink_->onPinDismissed();
            plist::Dict r;
            r.set("epk", plist::Data(outEpk.begin(), outEpk.end()))
                    .set("authTag", plist::Data(outTag.begin(), outTag.end()));
            const auto bin = plist::toBinary(r);
            return bin && sendResponse(fd, &req, 200, "OK", {{"Content-Type", kBplist}}, *bin);
        }

        sendResponse(fd, &req, 400, "Bad Request");
        return false;
    }

    bool AirPlayServer::handleSetup(ConnState& st, const RtspRequest& req) {
        const int fd = st.fd;
        const auto root = plist::parseBinary(req.body.data(), req.body.size());
        const plist::Dict* d = root ? root->asDict() : nullptr;
        if (d == nullptr) {
            LOGW("SETUP con plist non valido");
            sendResponse(fd, &req, 400, "Bad Request");
            return false;
        }
        plist::Dict resp;

        const plist::Value* ekeyV = d->find("ekey");
        const plist::Value* eivV = d->find("eiv");
        if (ekeyV || eivV) {
            const plist::Data* ekey = ekeyV ? ekeyV->asData() : nullptr;
            const plist::Data* eiv = eivV ? eivV->asData() : nullptr;
            uint8_t fpKey[16], key[16];
            if (!ekey || !eiv || eiv->size() != 16 || !st.fairplay.decryptKey(ekey->data(), ekey->size(), fpKey)) {
                LOGW("SETUP: chiave FairPlay assente o handshake non completato");
                sendResponse(fd, &req, 400, "Bad Request");
                return false;
            }
            bool pinRequired;
            {
                std::lock_guard<std::mutex> lock(pinMutex_);
                pinRequired = pinMode_ != PinMode::Off;
            }
            if (pinRequired && !st.pairing.verified()) {
                // Con il PIN attivo solo un dispositivo che ha completato pair-verify (e quindi è
                // stato autorizzato) può aprire una sessione.
                mbedtls_platform_zeroize(fpKey, sizeof fpKey);
                LOGW("SETUP senza pairing con PIN attivo: rifiutato");
                sendResponse(fd, &req, 470, "Connection Authorization Required");
                return false;
            }
            if (st.pairing.verified()) {
                crypto::deriveSessionKey(fpKey, st.pairing.sharedSecret(), key);
            } else {
                // Client che salta pair-verify (es. alcuni mittenti per Windows): la chiave
                // FairPlay si usa così com'è, come fa UxPlay.
                LOGW("SETUP senza pair-verify: chiave FairPlay non derivata");
                std::memcpy(key, fpKey, 16);
            }
            mbedtls_platform_zeroize(fpKey, sizeof fpKey);

            bool busy;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy = sessionOwner_ != nullptr && sessionOwner_ != st.client;
                if (!busy) sessionOwner_ = st.client;
            }
            if (busy) {
                mbedtls_platform_zeroize(key, sizeof key);
                LOGW("Un altro dispositivo sta già trasmettendo: SETUP rifiutato");
                sendResponse(fd, &req, 453, "Not Enough Bandwidth");
                return false;
            }

            const bool newSession = !st.session;
            st.session.reset();   // un SETUP ripetuto sulla stessa connessione riparte da zero
            st.session = std::make_unique<Session>(*sink_, st.peer, key, eiv->data());
            mbedtls_platform_zeroize(key, sizeof key);

            const uint64_t* tp = findUInt(*d, "timingPort");
            const uint16_t remoteTiming = (tp && *tp <= 65535) ? static_cast<uint16_t>(*tp) : 0;
            const int localTiming = st.session->startTiming(remoteTiming);
            resp.set("eventPort", 0).set("timingPort", localTiming > 0 ? localTiming : 0);

            if (newSession) {
                const plist::Value* nameV = d->find("name");
                const std::string* name = nameV ? nameV->asString() : nullptr;
                sink_->onSessionStarted(name ? sanitizeName(*name) : std::string());
            }
            LOGI("Sessione avviata (pairing %s)", st.pairing.verified() ? "verificato" : "assente");
        }

        if (const plist::Value* streamsV = d->find("streams")) {
            const plist::Array* streams = streamsV->asArray();
            if (!streams || streams->empty() || streams->size() > 4) {
                sendResponse(fd, &req, 400, "Bad Request");
                return false;
            }
            if (!st.session) return sendResponse(fd, &req, 455, "Method Not Valid in This State");

            plist::Array out;
            for (const plist::Value& sv : *streams) {
                const plist::Dict* sd = sv.asDict();
                const uint64_t* type = sd ? findUInt(*sd, "type") : nullptr;
                if (!type) {
                    sendResponse(fd, &req, 400, "Bad Request");
                    return false;
                }
                if (*type == 110) {
                    const uint64_t* id = findUInt(*sd, "streamConnectionID");
                    const int port = id ? st.session->startMirror(*id) : -1;
                    if (port < 0) {
                        sendResponse(fd, &req, 500, "Internal Server Error");
                        return false;
                    }
                    plist::Dict r;
                    r.set("type", 110).set("dataPort", port);
                    out.emplace_back(std::move(r));
                    LOGI("Stream video pronto sulla porta %d", port);
                } else if (*type == 96) {
                    const uint64_t* ct = findUInt(*sd, "ct");
                    const uint64_t* rcp = findUInt(*sd, "controlPort");   // porta dell'iPhone per le ritrasmissioni
                    const uint16_t remoteControl = (rcp && *rcp <= 65535) ? static_cast<uint16_t>(*rcp) : 0;
                    uint16_t dataPort = 0, controlPort = 0;
                    if (!st.session->startAudio(ct ? static_cast<int>(*ct & 0xFF) : 0, remoteControl, dataPort, controlPort)) {
                        sendResponse(fd, &req, 500, "Internal Server Error");
                        return false;
                    }
                    plist::Dict r;
                    r.set("type", 96).set("dataPort", static_cast<int>(dataPort)).set("controlPort", static_cast<int>(controlPort));
                    out.emplace_back(std::move(r));
                    LOGI("Stream audio pronto (ct=%d)", ct ? static_cast<int>(*ct & 0xFF) : 0);
                } else {
                    LOGW("SETUP: tipo di stream sconosciuto %llu", static_cast<unsigned long long>(*type));
                    sendResponse(fd, &req, 400, "Bad Request");
                    return false;
                }
            }
            resp.set("streams", std::move(out));
        }

        const auto bin = plist::toBinary(resp);
        if (!bin) {
            sendResponse(fd, &req, 500, "Internal Server Error");
            return false;
        }
        return sendResponse(fd, &req, 200, "OK", {{"Content-Type", kBplist}}, *bin);
    }

    bool AirPlayServer::handleTeardown(ConnState& st, const RtspRequest& req) {
        bool audio = false, video = false;
        if (!req.body.empty()) {
            const auto root = plist::parseBinary(req.body.data(), req.body.size());
            const plist::Dict* d = root ? root->asDict() : nullptr;
            const plist::Value* sv = d ? d->find("streams") : nullptr;
            if (const plist::Array* streams = sv ? sv->asArray() : nullptr) {
                for (const plist::Value& v : *streams) {
                    const plist::Dict* sd = v.asDict();
                    const uint64_t* type = sd ? findUInt(*sd, "type") : nullptr;
                    if (type && *type == 96) audio = true;
                    if (type && *type == 110) video = true;
                }
            }
        }
        if (st.session && audio) st.session->stopAudio();
        if (st.session && video) st.session->stopMirror();

        // Senza stream indicati è la fine della sessione. iOS 26+ non invia più TEARDOWN 110
        // quando si interrompe il mirroring, solo quello completo.
        const bool full = !audio && !video;
        Headers h = {{"Audio-Jack-Status", "connected; type=analog"}};
        if (full) h.emplace_back("Connection", "close");
        const bool ok = sendResponse(st.fd, &req, 200, "OK", h);
        if (full) endSession(st, EndReason::Teardown);
        return ok;
    }

    void AirPlayServer::endSession(ConnState& st, EndReason reason) {
        if (!st.session) return;
        st.session.reset();   // ferma i flussi: il distruttore attende i thread
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sessionOwner_ == st.client) sessionOwner_ = nullptr;
        }
        LOGI("Sessione terminata (motivo %d)", static_cast<int>(reason));
        sink_->onSessionEnded(reason);
    }

    // =============================================================== ciclo di vita

    AirPlayServer::~AirPlayServer() { stop(); }

    int AirPlayServer::start(uint16_t preferredPort, const ReceiverInfo& info, const uint8_t seed[32],
                             const AccessConfig& access) {
        if (running_) return -1;
        {
            std::lock_guard<std::mutex> lock(pinMutex_);
            pinMode_ = access.pin;
            trusted_ = access.trusted;
            if (trusted_.size() > kMaxTrusted) trusted_.resize(kMaxTrusted);
        }
        if (!crypto::init()) { LOGE("libsodium non inizializzata"); return -1; }

        identity_ = crypto::Ed25519Identity::fromSeed(seed);
        if (!identity_ || info.publicKey.size() != 32 ||
            std::memcmp(identity_->publicKey(), info.publicKey.data(), 32) != 0) {
            // La chiave annunciata via mDNS deve essere quella con cui firmiamo pair-verify,
            // altrimenti iOS rifiuterebbe il pairing con un errore poco chiaro.
            LOGE("Identità Ed25519 non coerente con la chiave pubblica annunciata");
            identity_.reset();
            return -1;
        }

        auto plists = buildInfoPlists(info);
        if (!plists) { LOGE("ReceiverInfo non valido, server non avviato"); return -1; }
        info_ = std::move(*plists);

        uint16_t port = preferredPort;
        int fd = net::tcpListen(port, 8);
        if (fd < 0) {
            LOGW("Porta %u occupata, uso una porta di sistema", preferredPort);
            port = 0;
            fd = net::tcpListen(port, 8);
            if (fd < 0) { LOGE("Impossibile aprire il socket di ascolto"); return -1; }
        }

        listenFd_ = fd;
        running_ = true;
        acceptThread_ = std::thread(&AirPlayServer::acceptLoop, this);
        LOGI("Server RTSP in ascolto sulla porta %u", port);
        return port;
    }

    void AirPlayServer::stop() {
        if (!running_.exchange(false)) return;

        ::shutdown(listenFd_, SHUT_RDWR);   // il thread di accettazione esce entro un ciclo di poll
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
        identity_.reset();
        LOGI("Server RTSP fermato");
    }

    void AirPlayServer::stopActiveSession() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessionOwner_ == nullptr) return;
        sessionOwner_->stopRequested = true;
        if (sessionOwner_->fd >= 0) ::shutdown(sessionOwner_->fd, SHUT_RDWR);
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
            // poll con timeout invece di accept bloccante: stop() non dipende dal fatto che
            // shutdown() sblocchi accept() (vero su Linux, non su altri sistemi).
            pollfd p{listenFd_, POLLIN, 0};
            if (::poll(&p, 1, 500) <= 0) continue;
            sockaddr_storage ss{};
            const int fd = net::acceptConn(listenFd_, ss);
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
            sockaddr_in6 peer6{};
            net::toIn6(ss, peer6);
            auto client = std::make_unique<Client>();
            client->fd = fd;
            Client* raw = client.get();
            client->thread = std::thread(&AirPlayServer::handleConnection, this, raw, net::toString(peer6), ss);
            clients_.push_back(std::move(client));
        }
    }

    void AirPlayServer::handleConnection(Client* client, std::string peer, sockaddr_storage peerAddr) {
        ConnState st;
        st.client = client;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            st.fd = client->fd;
        }
        net::toIn6(peerAddr, st.peer);
        LOGI("Connessione da %s", peer.c_str());

        // Timeout breve: serve a far girare il watchdog, non a chiudere la connessione.
        timeval tv{1, 0};
        ::setsockopt(st.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const int one = 1;
        ::setsockopt(st.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // risposte RTSP piccole: niente ritardo di Nagle

        std::vector<uint8_t> buf;
        buf.reserve(4096);
        uint8_t chunk[4096];
        bool keepOpen = true;
        int idleSec = 0;
        EndReason reason = EndReason::Disconnected;

        while (keepOpen && running_) {
            const ssize_t n = ::recv(st.fd, chunk, sizeof chunk, 0);
            if (n == 0) break;                       // il client ha chiuso
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
                if (st.session) {
                    const bool video = st.session->mirrorConnected();
                    if (st.session->idleMs() > (video ? kWatchdogMs : kSessionIdleMs)) {
                        LOGW("Watchdog: nessun dato dal client, chiudo la sessione");
                        reason = EndReason::Watchdog;
                        break;
                    }
                } else if (++idleSec >= kIdleTimeoutSec) {
                    break;
                }
                continue;
            }
            idleSec = 0;
            buf.insert(buf.end(), chunk, chunk + n);
            if (buf.size() > kMaxHeaderBytes + kMaxArtworkBytes + 4) {
                LOGW("Buffer oltre il limite da %s, chiudo", peer.c_str());
                break;
            }
            for (;;) {
                RtspRequest req;
                const int r = parseRequest(buf, req);
                if (r == 0) break;
                if (r < 0) {
                    LOGW("Richiesta malformata da %s", peer.c_str());
                    sendResponse(st.fd, nullptr, 400, "Bad Request");
                    keepOpen = false;
                    break;
                }
                if (!dispatch(st, req)) { keepOpen = false; break; }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (client->stopRequested) reason = EndReason::Local;
        }
        endSession(st, reason);

        std::lock_guard<std::mutex> lock(mutex_);
        ::close(client->fd);
        client->fd = -1;
        client->done = true;
        LOGI("Connessione chiusa: %s", peer.c_str());
    }

} // namespace irplay
