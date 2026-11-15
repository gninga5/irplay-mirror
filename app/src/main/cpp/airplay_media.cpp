/*
 * airplay_media.cpp
 * Scopo: AirPlay video, cioè il pulsante AirPlay dentro le app (YouTube, Safari, ...).
 * Al contrario della duplicazione, l'iPhone non invia lo schermo: passa alla TV l'indirizzo
 * del video e la TV lo riproduce da sé (ExoPlayer, lato Kotlin). L'iPhone diventa un
 * telecomando: pausa, salto, stop.
 *
 * Protocollo (HTTP/1.1 sulla stessa porta del mirroring, richieste con X-Apple-Session-ID):
 *   GET  /server-info              capacità del ricevitore (plist XML)
 *   POST /reverse                  "Upgrade: PTTH/1.0": la connessione si rovescia e diventa
 *                                  il canale su cui la TV invia richieste a iOS (POST /event)
 *   POST /play                     Content-Location + posizione iniziale
 *   POST /action                   risposte FCUP (playlist HLS chieste all'iPhone), playlistRemove
 *   GET  /playback-info            posizione, durata, stato: iOS la chiede circa ogni secondo
 *   POST /rate?value=0|1           pausa / riproduzione
 *   POST /scrub?position=s         salto; GET /scrub restituisce posizione e durata
 *   POST /stop                     fine
 *   PUT  /setProperty?..., GET /getProperty?...   accettate senza effetto
 *
 * Sicurezza:
 *  - con il PIN attivo il video è accettato solo da dispositivi che hanno completato
 *    pair-verify (sulla connessione o, nelle ultime 12 ore, dallo stesso indirizzo);
 *  - gli indirizzi diretti devono essere http/https: niente file:, content: o altri schemi
 *    che farebbero leggere al lettore file locali della TV;
 *  - le playlist ricevute sono controllate e limitate (hls_playlist.h), e la TV accetta in
 *    POST /action solo la playlist che ha appena chiesto (stesso URL, stessa sessione);
 *  - nel log non finiscono URL né titoli: rivelerebbero cosa guarda l'utente.
 * Resta un rischio intrinseco del protocollo: un dispositivo autorizzato può far scaricare
 * alla TV un indirizzo http qualsiasi della rete locale (come con una Apple TV).
 */
#define LOG_TAG "iRPlay_Media"
#include "server_conn.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <sys/socket.h>

#include "hls_playlist.h"
#include "log.h"
#include "plist_reader.h"
#include "plist_writer.h"

namespace irplay {

    using rtsp::iequals;
    using rtsp::sendResponse;

    namespace {

        constexpr const char* kXmlPlist = "text/x-apple-plist+xml";
        constexpr int64_t kVerifiedPeerMs = 12LL * 3600 * 1000;   // pair-verify vale per il video per 12 ore
        constexpr int64_t kMediaWatchdogMs = 45000;   // iOS chiede /playback-info ogni secondo
        constexpr size_t kMaxTotalPlaylistBytes = 32 * 1024 * 1024;
        constexpr size_t kMaxVerifiedPeers = 16;
        constexpr int kEventTimeoutMs = 200;
        // User-Agent che una Apple TV dichiara nelle richieste FCUP: iOS lo inoltra al server
        // delle playlist, che con alcuni valori risponde con formati diversi.
        constexpr const char* kFcupUserAgent =
                "AppleCoreMedia/1.0.0.11B554a (Apple TV; U; CPU OS 7_0_4 like Mac OS X; en_us";

        // X-Apple-Session-ID: UUID (36 caratteri). Si accettano solo lettere, cifre e '-'
        // perché il valore viene ripetuto nelle intestazioni che inviamo.
        bool validSessionId(const std::string& s) {
            if (s.empty() || s.size() > 64) return false;
            for (char c : s) {
                const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
                if (!ok) return false;
            }
            return true;
        }

        // Numero finito in [lo, hi] dall'intera stringa (niente "12abc").
        bool parseNumber(std::string_view s, double lo, double hi, double& out) {
            if (s.empty() || s.size() > 32) return false;
            const std::string tmp(s);
            char* end = nullptr;
            const double v = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || *end != '\0' || !std::isfinite(v) || v < lo || v > hi) return false;
            out = v;
            return true;
        }

        constexpr size_t kMaxMediaProps = 32;

        // "/getProperty?selectedMediaArray" -> "selectedMediaArray"; vuoto se non è un nome valido.
        std::string_view propertyName(std::string_view uri) {
            const size_t q = uri.find('?');
            if (q == std::string_view::npos) return {};
            const std::string_view n = uri.substr(q + 1);
            if (n.empty() || n.size() > 64) return {};
            for (char c : n) if (!std::isalnum(static_cast<unsigned char>(c))) return {};
            return n;
        }

        // Valore di un parametro nella query: "/scrub?position=12.5" -> "12.5".
        std::string_view queryValue(std::string_view uri, std::string_view key) {
            const size_t q = uri.find('?');
            if (q == std::string_view::npos) return {};
            std::string_view rest = uri.substr(q + 1);
            while (!rest.empty()) {
                const size_t amp = rest.find('&');
                const std::string_view pair = rest.substr(0, amp);
                const size_t eq = pair.find('=');
                if (eq != std::string_view::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
                if (amp == std::string_view::npos) break;
                rest.remove_prefix(amp + 1);
            }
            return {};
        }

        std::string_view pathOf(std::string_view uri) {
            const size_t q = uri.find('?');
            return q == std::string_view::npos ? uri : uri.substr(0, q);
        }

        const double* asDouble(const plist::Value* v) {
            return v ? std::get_if<double>(&v->v) : nullptr;
        }

        std::array<uint8_t, 16> addrOf(const sockaddr_in6& a) {
            std::array<uint8_t, 16> out{};
            std::memcpy(out.data(), &a.sin6_addr, 16);
            return out;
        }

        std::vector<uint8_t> xmlBody(const plist::Value& v) {
            const std::string x = plist::toXml(v);
            return {x.begin(), x.end()};
        }

        struct PlayRequest {
            std::string location;
            double startSec = 0;
            double startFraction = -1;
        };

        // Corpo di POST /play: plist binario (iOS recenti) o testo "Content-Location: ...\n
        // Start-Position: 0.25\n" (mittenti più vecchi, posizione come frazione della durata).
        // Dizionario di un video: corpo di /play o "item" di playlistInsert.
        bool parsePlayDict(const plist::Dict* d, PlayRequest& out) {
            const plist::Value* loc = d ? d->find("Content-Location") : nullptr;
            if (!loc || !loc->asString()) return false;
            out.location = *loc->asString();
            if (const double* s = asDouble(d->find("Start-Position-Seconds"))) {
                if (std::isfinite(*s) && *s >= 0 && *s < 1e7) out.startSec = *s;
            } else if (const double* f = asDouble(d->find("Start-Position"))) {
                if (std::isfinite(*f) && *f >= 0 && *f <= 1) out.startFraction = *f;
            }
            return true;
        }

        bool parsePlay(const RtspRequest& req, PlayRequest& out) {
            const std::string* ct = req.header("Content-Type");
            if (ct && ct->find("binary-plist") != std::string::npos) {
                const auto root = plist::parseBinary(req.body.data(), req.body.size());
                return parsePlayDict(root ? root->asDict() : nullptr, out);
            }
            const std::string_view text(reinterpret_cast<const char*>(req.body.data()), req.body.size());
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string_view::npos) nl = text.size();
                std::string_view line = text.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                const size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    const std::string_view k = rtsp::trim(line.substr(0, colon));
                    const std::string_view v = rtsp::trim(line.substr(colon + 1));
                    double f;
                    if (iequals(k, "Content-Location")) out.location.assign(v);
                    else if (iequals(k, "Start-Position") && parseNumber(v, 0, 1, f)) out.startFraction = f;
                }
                pos = nl + 1;
            }
            return !out.location.empty();
        }

    } // namespace

    // =============================================================== autorizzazione

    void AirPlayServer::notePeerVerified(const ConnState& st) {
        if (st.client) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::memcpy(st.client->edKey.data(), st.pairing.clientEdKey(), st.client->edKey.size());
            st.client->hasEdKey = true;
        }
        const auto addr = addrOf(st.peer);
        const int64_t now = monotonicMs();
        std::lock_guard<std::mutex> lock(pinMutex_);
        for (auto& [a, t] : verifiedPeers_) {
            if (a == addr) { t = now; return; }
        }
        if (verifiedPeers_.size() >= kMaxVerifiedPeers) verifiedPeers_.erase(verifiedPeers_.begin());
        verifiedPeers_.emplace_back(addr, now);
    }

    bool AirPlayServer::mediaAuthorized(const ConnState& st) {
        std::lock_guard<std::mutex> lock(pinMutex_);
        if (pinMode_ == PinMode::Off || st.pairing.verified()) return true;
        // iOS non ripete pair-verify su ogni connessione del video: basta che lo stesso
        // dispositivo l'abbia completato da poco (pair-verify riesce solo se il dispositivo
        // è autorizzato, vedi clientAllowed).
        const auto addr = addrOf(st.peer);
        const int64_t now = monotonicMs();
        for (const auto& [a, t] : verifiedPeers_) {
            if (a == addr && now - t < kVerifiedPeerMs) return true;
        }
        return false;
    }

    // =============================================================== dispatch

    bool AirPlayServer::dispatchMedia(ConnState& st, const RtspRequest& req, bool& keepOpen) {
        if (req.protocol != "HTTP/1.1") return false;
        const std::string& m = req.method;
        const std::string_view path = pathOf(req.uri);
        const int fd = st.fd;
        keepOpen = true;

        if (m == "GET" && path == "/server-info") {
            keepOpen = sendResponse(fd, &req, 200, "OK", {{"Content-Type", kXmlPlist}}, serverInfo_);
            return true;
        }
        if (m == "POST" && path == "/fp-setup2") {
            // Variante FairPlay per i video protetti (Apple TV+, film acquistati): non supportata.
            LOGW("Video protetto da FairPlay: non supportato");
            keepOpen = sendResponse(fd, &req, 421, "Misdirected Request");
            return true;
        }

        const bool known = path == "/reverse" || path == "/play" || path == "/action" || path == "/playback-info" ||
                           path == "/rate" || path == "/scrub" || path == "/stop" ||
                           path == "/setProperty" || path == "/getProperty";
        if (!known) return false;

        const std::string* sidHeader = req.header("X-Apple-Session-ID");
        const std::string sid = sidHeader && validSessionId(*sidHeader) ? *sidHeader : std::string();
        if (!sid.empty()) {
            std::lock_guard<std::mutex> lock(mediaMutex_);
            if (media_ && media_->sessionId == sid) media_->lastSeenMs = monotonicMs();
        }

        if (!mediaAuthorized(st)) {
            LOGW("AirPlay video da un dispositivo non associato: rifiutato");
            sendResponse(fd, &req, 403, "Forbidden");
            keepOpen = false;
            return true;
        }

        if (path == "/reverse" && m == "POST") {
            keepOpen = handleReverse(st, req, sid);
            return true;
        }
        if (path == "/play" && m == "POST") {
            keepOpen = handlePlay(st, req, sid);
            return true;
        }
        if (path == "/action" && m == "POST") {
            keepOpen = handleAction(st, req, sid);
            return true;
        }
        if (path == "/playback-info" && m == "GET") {
            keepOpen = handlePlaybackInfo(st, req);
            return true;
        }
        if (path == "/rate" && m == "POST") {
            double rate;
            if (!parseNumber(queryValue(req.uri, "value"), 0, 2, rate)) {
                keepOpen = sendResponse(fd, &req, 400, "Bad Request");
                return true;
            }
            sink_->onMediaRate(static_cast<float>(rate));
            keepOpen = sendResponse(fd, &req, 200, "OK");
            return true;
        }
        if (path == "/scrub") {
            if (m == "GET") {
                char text[96];
                std::snprintf(text, sizeof text, "duration: %.6f\r\nposition: %.6f\r\n",
                              mediaDur_.load(), mediaPos_.load());
                keepOpen = sendResponse(fd, &req, 200, "OK", {{"Content-Type", "text/parameters"}},
                                        std::vector<uint8_t>(text, text + std::strlen(text)));
                return true;
            }
            double position;
            if (m != "POST" || !parseNumber(queryValue(req.uri, "position"), 0, 1e7, position)) {
                keepOpen = sendResponse(fd, &req, 400, "Bad Request");
                return true;
            }
            {
                // Playlist ancora in arrivo (playlistInsert seguito da /scrub): la posizione
                // diventa il punto di partenza del video, il lettore non è ancora aperto.
                std::lock_guard<std::mutex> lock(mediaMutex_);
                if (media_ && media_->fetching && (sid.empty() || media_->sessionId == sid)) {
                    media_->startSec = position;
                    media_->startFraction = -1;
                    keepOpen = sendResponse(fd, &req, 200, "OK");
                    return true;
                }
            }
            sink_->onMediaScrub(position);
            keepOpen = sendResponse(fd, &req, 200, "OK");
            return true;
        }
        if (path == "/stop" && m == "POST") {
            bool stop = false;
            {
                std::lock_guard<std::mutex> lock(mediaMutex_);
                if (media_ && (sid.empty() || media_->sessionId == sid)) {
                    media_.reset();
                    stop = true;
                }
            }
            if (stop) {
                LOGI("Video fermato dall'iPhone");
                mediaState_ = static_cast<int>(MediaState::Idle);
                sink_->onMediaStop();
            }
            keepOpen = sendResponse(fd, &req, 200, "OK");
            return true;
        }
        if (path == "/setProperty" || path == "/getProperty") {
            // Proprietà del lettore (tracce scelte, fine dell'elemento ...). ExoPlayer sceglie
            // da sé le tracce, ma iOS deve ritrovare i valori che ha impostato: dopo un cambio di
            // traccia (playlistInsert) chiede getProperty?selectedMediaArray e, senza "value",
            // resta in caricamento e smette di chiedere lo stato.
            plist::Dict r;
            r.set("errorCode", 0);
            const std::string_view name = propertyName(req.uri);
            if (!name.empty()) {
                std::lock_guard<std::mutex> lock(mediaMutex_);
                auto it = std::find_if(mediaProps_.begin(), mediaProps_.end(), [&](const auto& kv) { return kv.first == name; });
                if (path == "/setProperty") {
                    const auto root = plist::parseBinary(req.body.data(), req.body.size());
                    const plist::Dict* d = root ? root->asDict() : nullptr;
                    const plist::Value* v = d ? d->find("value") : nullptr;
                    if (v) {
                        if (it != mediaProps_.end()) it->second = *v;
                        else if (mediaProps_.size() < kMaxMediaProps) mediaProps_.emplace_back(std::string(name), *v);
                    }
                } else if (it != mediaProps_.end()) {
                    r.set("value", it->second);
                }
            }
            keepOpen = sendResponse(fd, &req, 200, "OK", {{"Content-Type", kXmlPlist}}, xmlBody(r));
            return true;
        }
        keepOpen = sendResponse(fd, &req, 405, "Method Not Allowed");
        return true;
    }

    // =============================================================== canale inverso

    bool AirPlayServer::handleReverse(ConnState& st, const RtspRequest& req, const std::string& sid) {
        const std::string* upgrade = req.header("Upgrade");
        if (sid.empty() || !upgrade || !iequals(*upgrade, "PTTH/1.0")) {
            sendResponse(st.fd, &req, 400, "Bad Request");
            return false;
        }
        std::lock_guard<std::mutex> lock(mediaMutex_);
        if (reverseFd_ >= 0 && reverseFd_ != st.fd) {
            // Un solo canale inverso: il precedente (video finito o iPhone riconnesso) si chiude.
            // Il suo thread esce e, non essendo più quello attivo, non tocca lo stato nuovo.
            ::shutdown(reverseFd_, SHUT_RDWR);
        }
        reverseFd_ = st.fd;
        reverseSession_ = sid;
        st.reverse = true;
        // La risposta parte con mediaMutex_ tenuto: un FCUP inviato da un altro thread non può
        // arrivare a iOS prima del 101 che apre il canale.
        LOGI("Canale inverso aperto");
        return sendResponse(st.fd, &req, 101, "Switching Protocols", {{"Upgrade", "PTTH/1.0"}, {"Connection", "Upgrade"}});
    }

    bool AirPlayServer::isActiveReverse(int fd) {
        std::lock_guard<std::mutex> lock(mediaMutex_);
        return reverseFd_ == fd;
    }

    bool AirPlayServer::sendFcupLocked(const std::string& url) {
        if (reverseFd_ < 0 || !media_) return false;
        plist::Dict headers;
        headers.set("X-Playback-Session-Id", media_->sessionId).set("User-Agent", kFcupUserAgent);
        plist::Dict request;
        request.set("FCUP_Response_ClientInfo", 1)
                .set("FCUP_Response_ClientRef", 40030004)
                .set("FCUP_Response_RequestID", static_cast<uint64_t>(++media_->requestId))
                .set("FCUP_Response_URL", url)
                .set("sessionID", 1)
                .set("FCUP_Response_Headers", std::move(headers));
        plist::Dict root;
        root.set("sessionID", 1).set("type", "unhandledURLRequest").set("request", std::move(request));
        const std::string body = plist::toXml(root);
        const std::string head = "POST /event HTTP/1.1\r\nX-Apple-Session-ID: " + media_->sessionId +
                                 "\r\nContent-Type: " + kXmlPlist + "\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n";
        return rtsp::sendAllTimed(reverseFd_, head.data(), head.size(), 1000) &&
               rtsp::sendAllTimed(reverseFd_, body.data(), body.size(), 1000);
    }

    void AirPlayServer::sendEventLocked(const char* state) {
        if (reverseFd_ < 0 || !media_ || media_->lastEvent == state) return;
        plist::Dict root;
        root.set("category", "video").set("sessionID", 1).set("state", state);
        const std::string body = plist::toXml(root);
        const std::string head = "POST /event HTTP/1.1\r\nX-Apple-Session-ID: " + media_->sessionId +
                                 "\r\nContent-Type: " + kXmlPlist + "\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n";
        // Evento best-effort: se il socket è pieno si rinuncia (iOS legge comunque /playback-info).
        if (rtsp::sendAllTimed(reverseFd_, head.data(), head.size(), kEventTimeoutMs) &&
            rtsp::sendAllTimed(reverseFd_, body.data(), body.size(), kEventTimeoutMs)) {
            media_->lastEvent = state;
        }
    }

    // Nuovo elemento o elemento tolto: iOS non deve vedere posizione e durata del precedente
    // insieme a "non pronto" (dopo un playlistInsert smette di chiedere lo stato).
    void AirPlayServer::resetMediaPlayback(MediaState state) {
        mediaPos_ = 0.0;
        mediaDur_ = 0.0;
        mediaRate_ = 0.0f;
        mediaState_ = static_cast<int>(state);
    }

    // =============================================================== /play e /action

    bool AirPlayServer::handlePlay(ConnState& st, const RtspRequest& req, const std::string& sid) {
        const int fd = st.fd;
        PlayRequest play;
        if (sid.empty() || !parsePlay(req, play)) {
            LOGW("POST /play non valido");
            sendResponse(fd, &req, 400, "Bad Request");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mediaMutex_);
            mediaProps_.clear();   // nuovo video: le proprietà del precedente non valgono più
        }
        return startMedia(st, req, sid, play.location, play.startSec, play.startFraction);
    }

    /*
     * Avvio di un video: da POST /play o da /action playlistInsert (YouTube lo usa per cambiare
     * traccia audio: playlistRemove del video attuale, playlistInsert dello stesso video con la
     * nuova lingua e subito dopo /scrub alla posizione raggiunta).
     */
    bool AirPlayServer::startMedia(ConnState& st, const RtspRequest& req, const std::string& sid,
                                   const std::string& location, double startSec, double startFraction) {
        const int fd = st.fd;
        PlayRequest play;
        play.location = location;
        play.startSec = startSec;
        play.startFraction = startFraction;

        // Duplicazione in corso da un altro dispositivo: la TV ha un solo schermo.
        // Stesso dispositivo = stesso indirizzo oppure stessa chiave autenticata: iOS può aprire
        // la sessione in IPv4 e mandare il video in IPv6 (visto su una TCL con l'app YouTube).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool sameDevice = sessionOwner_ == nullptr || st.client == nullptr ||
                                    sessionOwner_->addr == st.client->addr ||
                                    (sessionOwner_->hasEdKey && st.client->hasEdKey &&
                                     sessionOwner_->edKey == st.client->edKey);
            if (!sameDevice) {
                LOGW("Video rifiutato: un altro dispositivo sta duplicando lo schermo");
                return sendResponse(fd, &req, 453, "Not Enough Bandwidth");
            }
        }

        if (hls::isSafeHttpUrl(play.location)) {
            {
                std::lock_guard<std::mutex> lock(mediaMutex_);
                media_ = std::make_unique<MediaSession>();
                media_->sessionId = sid;
                media_->startSec = play.startSec;
                media_->startFraction = play.startFraction;
                media_->lastSeenMs = monotonicMs();
            }
            LOGI("Video AirPlay: indirizzo diretto");
            resetMediaPlayback(MediaState::Loading);
            sink_->onMediaPlay(play.location, play.startSec, play.startFraction, {});
            return sendResponse(fd, &req, 200, "OK");
        }

        const auto prefix = hls::remotePrefixOf(play.location);
        if (!prefix) {
            // Richiesta ben formata ma con uno schema non ammesso (file:, content: ...): si
            // rifiuta il video, la connessione di controllo resta aperta.
            LOGW("Video AirPlay: indirizzo in una forma non supportata");
            return sendResponse(fd, &req, 400, "Bad Request");
        }

        bool sent;
        {
            std::lock_guard<std::mutex> lock(mediaMutex_);
            if (reverseFd_ < 0 || reverseSession_ != sid) {
                LOGW("Video HLS senza canale inverso: impossibile chiedere le playlist");
                return sendResponse(fd, &req, 400, "Bad Request");
            }
            media_ = std::make_unique<MediaSession>();
            media_->sessionId = sid;
            media_->startSec = play.startSec;
            media_->startFraction = play.startFraction;
            media_->lastSeenMs = monotonicMs();
            media_->remotePrefix = *prefix;
            media_->pendingUrl = play.location;
            media_->fetching = true;
            sent = sendFcupLocked(play.location);
            if (!sent) media_.reset();
        }
        if (!sent) {
            LOGW("Richiesta della playlist all'iPhone fallita");
            return sendResponse(fd, &req, 500, "Internal Server Error");
        }
        LOGI("Video AirPlay HLS: chiedo le playlist all'iPhone");
        resetMediaPlayback(MediaState::Loading);
        return sendResponse(fd, &req, 200, "OK");
    }

    bool AirPlayServer::handleAction(ConnState& st, const RtspRequest& req, const std::string& sid) {
        const int fd = st.fd;
        const auto root = plist::parseBinary(req.body.data(), req.body.size());
        const plist::Dict* d = root ? root->asDict() : nullptr;
        const plist::Value* typeV = d ? d->find("type") : nullptr;
        const plist::Value* paramsV = d ? d->find("params") : nullptr;
        const std::string* type = typeV ? typeV->asString() : nullptr;
        const plist::Dict* params = paramsV ? paramsV->asDict() : nullptr;
        if (sid.empty() || !type || !params) {
            sendResponse(fd, &req, 400, "Bad Request");
            return false;
        }

        if (*type == "playlistRemove") {
            bool stop = false;
            {
                std::lock_guard<std::mutex> lock(mediaMutex_);
                if (media_ && media_->sessionId == sid) {
                    media_.reset();
                    stop = true;
                }
            }
            if (stop) {
                resetMediaPlayback(MediaState::Idle);
                sink_->onMediaStop();
            }
            return sendResponse(fd, &req, 200, "OK");
        }
        if (*type == "playlistInsert") {
            const plist::Value* itemV = params->find("item");
            PlayRequest play;
            if (!parsePlayDict(itemV ? itemV->asDict() : nullptr, play)) {
                return sendResponse(fd, &req, 400, "Bad Request");
            }
            return startMedia(st, req, sid, play.location, play.startSec, play.startFraction);
        }
        if (*type != "unhandledURLResponse") {
            return sendResponse(fd, &req, 200, "OK");   // altri tipi: nessun effetto
        }

        const plist::Value* urlV = params->find("FCUP_Response_URL");
        const plist::Value* dataV = params->find("FCUP_Response_Data");
        const plist::Value* codeV = params->find("FCUP_Response_StatusCode");
        const std::string* url = urlV ? urlV->asString() : nullptr;
        const plist::Data* data = dataV ? dataV->asData() : nullptr;
        const uint64_t* code = codeV ? codeV->asUInt() : nullptr;

        enum class Outcome { Bad, Abort, Next, Done } outcome = Outcome::Bad;
        std::string playUrl;
        double startSec = 0, startFraction = -1;
        std::vector<std::pair<std::string, std::string>> playlists;
        {
            std::lock_guard<std::mutex> lock(mediaMutex_);
            MediaSession* ms = media_.get();
            // Si accetta solo la risposta alla richiesta appena fatta: stessa sessione, stesso URL.
            if (!ms || ms->sessionId != sid || !ms->fetching || !url || *url != ms->pendingUrl) {
                outcome = Outcome::Bad;
            } else if (!data || data->empty() || (code && *code != 0 && (*code < 200 || *code > 299))) {
                // iOS (YouTube) manda StatusCode 0 insieme a una playlist valida: 0 e 2xx sono successo.
                outcome = Outcome::Abort;
            } else {
                const std::string_view text(reinterpret_cast<const char*>(data->data()), data->size());
                outcome = Outcome::Abort;
                if (!ms->haveMaster) {
                    if (auto master = hls::rewriteMaster(text, ms->remotePrefix)) {
                        ms->master = std::move(master->text);
                        ms->mediaUris = std::move(master->mediaUris);
                        ms->mediaPlaylists.assign(ms->mediaUris.size(), std::string());
                        ms->haveMaster = true;
                        ms->totalBytes = ms->master.size();
                        outcome = Outcome::Next;
                    }
                } else if (ms->next >= 1 && ms->next <= ms->mediaPlaylists.size()) {
                    if (auto media = hls::expandCondensed(text)) {
                        ms->totalBytes += media->size();
                        ms->mediaPlaylists[ms->next - 1] = std::move(*media);
                        outcome = Outcome::Next;
                    }
                }
                if (outcome == Outcome::Next && ms->totalBytes > kMaxTotalPlaylistBytes) outcome = Outcome::Abort;

                if (outcome == Outcome::Next && ms->next < ms->mediaUris.size()) {
                    ms->pendingUrl = ms->mediaUris[ms->next++];
                    if (!sendFcupLocked(ms->pendingUrl)) outcome = Outcome::Abort;
                } else if (outcome == Outcome::Next) {
                    // Tutte le playlist arrivate: passano al lettore.
                    ms->fetching = false;
                    playUrl = std::string(hls::kLocalMaster);
                    startSec = ms->startSec;
                    startFraction = ms->startFraction;
                    playlists.emplace_back(playUrl, std::move(ms->master));
                    for (size_t i = 0; i < ms->mediaUris.size(); ++i) {
                        playlists.emplace_back(hls::toLocal(ms->mediaUris[i], ms->remotePrefix),
                                               std::move(ms->mediaPlaylists[i]));
                    }
                    ms->master.clear();
                    ms->mediaPlaylists.clear();
                    outcome = Outcome::Done;
                }
            }
            if (outcome == Outcome::Abort && ms) {
                sendEventLocked("stopped");
                media_.reset();
            }
        }

        switch (outcome) {
            case Outcome::Bad:
                LOGW("POST /action inatteso o non valido");
                return sendResponse(fd, &req, 400, "Bad Request");
            case Outcome::Abort:
                LOGW("Playlist HLS non valida o non disponibile: video annullato");
                mediaState_ = static_cast<int>(MediaState::Idle);
                sink_->onMediaStop();
                return sendResponse(fd, &req, 200, "OK");
            case Outcome::Next:
                return sendResponse(fd, &req, 200, "OK");
            case Outcome::Done:
                LOGI("Playlist ricevute (%zu): avvio il lettore", playlists.size());
                sink_->onMediaPlay(playUrl, startSec, startFraction, playlists);
                return sendResponse(fd, &req, 200, "OK");
        }
        return false;
    }

    // =============================================================== stato verso iOS

    bool AirPlayServer::handlePlaybackInfo(ConnState& st, const RtspRequest& req) {
        const auto state = static_cast<MediaState>(mediaState_.load());
        const double dur = std::max(0.0, mediaDur_.load());
        const double pos = std::min(std::max(0.0, mediaPos_.load()), dur > 0 ? dur : 1e7);
        const bool ready = state == MediaState::Playing || state == MediaState::Paused;

        plist::Dict loaded, seekable;
        loaded.set("duration", dur - pos).set("start", pos);
        seekable.set("duration", dur).set("start", 0.0);
        plist::Dict d;
        d.set("duration", dur)
                .set("loadedTimeRanges", plist::Array{std::move(loaded)})
                .set("playbackBufferEmpty", state == MediaState::Loading)
                .set("playbackBufferFull", false)
                .set("playbackLikelyToKeepUp", ready)
                .set("position", pos)
                .set("rate", static_cast<double>(state == MediaState::Playing ? mediaRate_.load() : 0.0f))
                .set("readyToPlay", ready)
                .set("seekableTimeRanges", plist::Array{std::move(seekable)});
        return sendResponse(st.fd, &req, 200, "OK", {{"Content-Type", kXmlPlist}}, xmlBody(d));
    }

    void AirPlayServer::updateMediaPlayback(double positionSec, double durationSec, float rate, MediaState state) {
        if (std::isfinite(positionSec)) mediaPos_ = positionSec;
        if (std::isfinite(durationSec)) mediaDur_ = durationSec;
        if (std::isfinite(rate)) mediaRate_ = rate;
        mediaState_ = static_cast<int>(state);

        const char* event = nullptr;
        switch (state) {
            case MediaState::Loading: event = "loading"; break;
            case MediaState::Playing: event = "playing"; break;
            case MediaState::Paused: event = "paused"; break;
            case MediaState::Ended: event = "stopped"; break;
            case MediaState::Idle: break;
        }
        if (event == nullptr) return;
        std::lock_guard<std::mutex> lock(mediaMutex_);
        sendEventLocked(event);
    }

    void AirPlayServer::stopMedia() {
        std::lock_guard<std::mutex> lock(mediaMutex_);
        if (!media_) return;
        sendEventLocked("stopped");
        media_.reset();
        mediaState_ = static_cast<int>(MediaState::Idle);
        LOGI("Video fermato dalla TV");
    }

    void AirPlayServer::mediaHousekeeping(ConnState* closing) {
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(mediaMutex_);
            if (closing && closing->reverse && reverseFd_ == closing->fd) {
                // Canale inverso chiuso. Conta solo se servono ancora playlist (FCUP): a video
                // avviato si continua, e un telefono sparito lo ferma il watchdog su /playback-info.
                reverseFd_ = -1;
                reverseSession_.clear();
                if (media_ && media_->fetching) {
                    media_.reset();
                    stop = true;
                }
            }
            if (media_ && monotonicMs() - media_->lastSeenMs > kMediaWatchdogMs) {
                LOGW("Watchdog: l'iPhone non chiede più lo stato del video, chiudo");
                media_.reset();
                stop = true;
            }
        }
        if (stop) {
            mediaState_ = static_cast<int>(MediaState::Idle);
            sink_->onMediaStop();
        }
    }

    std::vector<uint8_t> buildServerInfo(const ReceiverInfo& info) {
        // Bit 0-6 e 9 (video, foto, FairPlay video, volume, HLS, presentazioni, audio): gli
        // stessi che dichiarano UxPlay e le Apple TV di terza generazione.
        plist::Dict d;
        d.set("deviceid", info.deviceId)
                .set("features", 0x27F)
                .set("macAddress", info.deviceId)
                .set("model", info.model)
                .set("osBuildVersion", "12B435")
                .set("protovers", "1.0")
                .set("srcvers", info.sourceVersion)
                .set("vv", 2);
        return xmlBody(d);
    }

} // namespace irplay
