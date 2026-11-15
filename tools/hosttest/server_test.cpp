// Test host del motore iRPlay: simula un iPhone contro AirPlayServer.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <sodium.h>

#include "airplay_server.h"
#include "nal_parser.h"
#include "plist_reader.h"
#include "plist_writer.h"
extern "C" {
#include "playfair.h"
}

using namespace irplay;
using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Event { std::string kind; Bytes a, b; int64_t i = 0; bool flag = false; float f = 0; };

class TestSink : public SessionSink {
public:
    std::mutex m; std::condition_variable cv; std::vector<Event> ev;
    void push(Event e) { { std::lock_guard<std::mutex> l(m); ev.push_back(std::move(e)); } cv.notify_all(); }
    bool waitFor(const std::string& kind, Event* out = nullptr, int ms = 3000) {
        std::unique_lock<std::mutex> l(m);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        size_t idx = 0;
        for (;;) {
            for (; idx < ev.size(); ++idx) if (ev[idx].kind == kind) { if (out) *out = ev[idx]; ev.erase(ev.begin() + idx); return true; }
            if (cv.wait_until(l, deadline) == std::cv_status::timeout) {
                for (size_t j = 0; j < ev.size(); ++j) if (ev[j].kind == kind) { if (out) *out = ev[j]; ev.erase(ev.begin() + j); return true; }
                return false;
            }
        }
    }
    bool has(const std::string& kind) { std::lock_guard<std::mutex> l(m); for (auto& e : ev) if (e.kind == kind) return true; return false; }
    void onSessionStarted(const std::string& n) override { push({"start", Bytes(n.begin(), n.end())}); }
    void onSessionEnded(EndReason r) override { Event e{"end"}; e.i = (int) r; push(e); }
    void onVideoStarted() override { push({"vstart"}); }
    void onVideoStopped() override { push({"vstop"}); }
    void onVideoConfig(const uint8_t* s, size_t sl, const uint8_t* p, size_t pl) override { push({"vcfg", Bytes(s, s + sl), Bytes(p, p + pl)}); }
    void onVideoFrame(const uint8_t* d, size_t l, int64_t pts, bool k) override { Event e{"frame", Bytes(d, d + l)}; e.i = pts; e.flag = k; push(e); }
    void onAudioStarted(int ct) override { Event e{"astart"}; e.i = ct; push(e); }
    void onAudioStopped() override { push({"astop"}); }
    void onAudioFrame(const uint8_t* d, size_t l) override { push({"aframe", Bytes(d, d + l)}); }
    void onVolume(float db) override { Event e{"vol"}; e.f = db; push(e); }
    void onPinRequested(const std::string& pin) override { push({"pin", Bytes(pin.begin(), pin.end())}); }
    void onPinDismissed() override { push({"pinoff"}); }
    void onClientTrusted(const uint8_t pk[32]) override { push({"trusted", Bytes(pk, pk + 32)}); }
};

static std::mt19937 rng(12345);
static Bytes rnd(size_t n) { Bytes b(n); for (auto& x : b) x = (uint8_t) rng(); return b; }

static int connectTcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*) &a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return fd;
}

struct Resp { int code = -1; std::string head; Bytes body; };

static Resp rtsp(int fd, const std::string& method, const std::string& uri, const Bytes& body = {},
                 const std::string& extra = "", const char* proto = "RTSP/1.0") {
    static int cseq = 1;
    std::string req = method + " " + uri + " " + proto + "\r\nCSeq: " + std::to_string(cseq++) + "\r\n" + extra;
    if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    Bytes out(req.begin(), req.end()); out.insert(out.end(), body.begin(), body.end());
    if (send(fd, out.data(), out.size(), 0) != (ssize_t) out.size()) return {};
    std::string buf; char c;
    timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (buf.find("\r\n\r\n") == std::string::npos) { if (recv(fd, &c, 1, 0) != 1) return {}; buf += c; }
    Resp r; r.head = buf; r.code = std::atoi(buf.c_str() + buf.find(' ') + 1);
    size_t cl = buf.find("Content-Length: "); size_t len = cl == std::string::npos ? 0 : std::atoi(buf.c_str() + cl + 16);
    r.body.resize(len); size_t got = 0;
    while (got < len) { ssize_t n = recv(fd, r.body.data() + got, len - got, 0); if (n <= 0) return {}; got += n; }
    return r;
}

static void sha512_16(const Bytes& a, const uint8_t* b, size_t bl, uint8_t out[16]) {
    crypto_hash_sha512_state st; uint8_t h[64];
    crypto_hash_sha512_init(&st); crypto_hash_sha512_update(&st, a.data(), a.size()); crypto_hash_sha512_update(&st, b, bl);
    crypto_hash_sha512_final(&st, h); memcpy(out, h, 16);
}
static void ctr(const uint8_t key[16], const uint8_t iv[16], size_t skip, uint8_t* data, size_t len) {
    mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, key, 128);
    uint8_t nonce[16], block[16]; size_t off = 0; memcpy(nonce, iv, 16);
    Bytes z(skip); if (skip) mbedtls_aes_crypt_ctr(&c, skip, &off, nonce, block, z.data(), z.data());
    mbedtls_aes_crypt_ctr(&c, len, &off, nonce, block, data, data); mbedtls_aes_free(&c);
}

// Client simulato: pairing + FairPlay. Restituisce la chiave di sessione attesa.
struct FakeIphone {
    int fd = -1;
    uint8_t edPk[32], edSk[64], xPk[32], xSk[32], shared[32];
    Bytes msg164;
    bool pair(const uint8_t serverPk[32]) {
        crypto_sign_keypair(edPk, edSk); randombytes_buf(xSk, 32); crypto_scalarmult_base(xPk, xSk);
        Resp r = rtsp(fd, "POST", "/pair-setup", Bytes(edPk, edPk + 32), "Content-Type: application/octet-stream\r\n", "HTTP/1.1");
        if (r.code != 200 || r.body.size() != 32 || memcmp(r.body.data(), serverPk, 32)) return false;
        Bytes b1 = {1, 0, 0, 0}; b1.insert(b1.end(), xPk, xPk + 32); b1.insert(b1.end(), edPk, edPk + 32);
        r = rtsp(fd, "POST", "/pair-verify", b1, "", "HTTP/1.1");
        if (r.code != 200 || r.body.size() != 96) return false;
        const uint8_t* sPk = r.body.data(); uint8_t sig[64]; memcpy(sig, r.body.data() + 32, 64);
        if (crypto_scalarmult(shared, xSk, sPk) != 0) return false;
        uint8_t key[16], iv[16];
        std::string sk = "Pair-Verify-AES-Key", si = "Pair-Verify-AES-IV";
        sha512_16(Bytes(sk.begin(), sk.end()), shared, 32, key); sha512_16(Bytes(si.begin(), si.end()), shared, 32, iv);
        ctr(key, iv, 0, sig, 64);
        uint8_t m[64]; memcpy(m, sPk, 32); memcpy(m + 32, xPk, 32);
        if (crypto_sign_verify_detached(sig, m, 64, serverPk) != 0) { fprintf(stderr, "firma server non valida\n"); return false; }
        uint8_t my[64]; memcpy(m, xPk, 32); memcpy(m + 32, sPk, 32); crypto_sign_detached(my, nullptr, m, 64, edSk);
        ctr(key, iv, 64, my, 64);
        Bytes b2 = {0, 0, 0, 0}; b2.insert(b2.end(), my, my + 64);
        r = rtsp(fd, "POST", "/pair-verify", b2, "", "HTTP/1.1");
        return r.code == 200;
    }
    bool fairplay() {
        Bytes f1 = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 2, 0};
        Resp r = rtsp(fd, "POST", "/fp-setup", f1, "", "HTTP/1.1");
        if (r.code != 200 || r.body.size() != 142 || r.body[13] != 2 || memcmp(r.body.data(), "FPLY", 4)) return false;
        msg164 = rnd(164); memcpy(msg164.data(), "FPLY\x03\x01\x03\x00", 8); msg164[12] = 2;
        r = rtsp(fd, "POST", "/fp-setup", msg164, "", "HTTP/1.1");
        return r.code == 200 && r.body.size() == 32 && !memcmp(r.body.data() + 12, msg164.data() + 144, 20);
    }
    void sessionKey(const Bytes& ekey, uint8_t out[16], bool paired = true) {
        Bytes m = msg164, e = ekey; uint8_t fp[16]; playfair_decrypt(m.data(), e.data(), fp);
        if (!paired) { memcpy(out, fp, 16); return; }
        sha512_16(Bytes(fp, fp + 16), shared, 32, out);
    }
};

static Bytes setupBody(const Bytes& ekey, const Bytes& eiv, int timingPort) {
    plist::Dict d; d.set("ekey", ekey).set("eiv", eiv).set("timingProtocol", "NTP").set("timingPort", timingPort)
            .set("name", "iPhone di Test").set("isScreenMirroringSession", true);
    return *plist::toBinary(d);
}

static void testPlistReader() {
    // roundtrip
    plist::Dict d; d.set("a", 1).set("s", "ciao").set("b", true).set("x", 2.5).set("data", Bytes{1, 2, 3})
            .set("arr", plist::Array{plist::Value(7), plist::Value("z")}).set("big", (uint64_t) 0xFEDCBA9876543210ull);
    auto bin = plist::toBinary(d); auto back = plist::parseBinary(bin->data(), bin->size());
    CHECK(back && back->asDict());
    auto* bd = back->asDict();
    CHECK(*bd->find("a")->asUInt() == 1 && *bd->find("s")->asString() == "ciao" && *bd->find("b")->asBool());
    CHECK(*bd->find("big")->asUInt() == 0xFEDCBA9876543210ull);
    CHECK(bd->find("arr")->asArray()->size() == 2);
    // plutil con UTF-16 e annidamento
    std::ifstream f("utf16.bplist", std::ios::binary); Bytes u((std::istreambuf_iterator<char>(f)), {});
    auto pu = plist::parseBinary(u.data(), u.size());
    CHECK(pu && pu->asDict());
    if (pu && pu->asDict()) {
        CHECK(*pu->asDict()->find("name")->asString() == "Caffè ☕ 😀 iPhone");
        CHECK(*pu->asDict()->find("n")->asDict()->find("neg")->asUInt() == (uint64_t) -5);
    }
    // fuzz: nessun crash/UB sotto ASan/UBSan
    std::vector<Bytes> seeds = {*bin, u, *plist::toBinary(plist::Dict().set("streams", plist::Array{plist::Dict().set("type", 110)}))};
    size_t ok = 0;
    for (int it = 0; it < 300000; ++it) {
        Bytes m = seeds[it % seeds.size()];
        int nmut = 1 + rng() % 6;
        for (int k = 0; k < nmut; ++k) m[rng() % m.size()] = (uint8_t) rng();
        if (rng() % 10 == 0) m.resize(rng() % m.size());
        if (plist::parseBinary(m.data(), m.size())) ++ok;
    }
    fprintf(stderr, "fuzz plist: 300000 mutazioni, %zu ancora valide, nessun crash\n", ok);
    // grafo con riferimenti ripetuti: array di 4000 riferimenti allo stesso data da 8 KB -> deve essere rifiutato
    Bytes bomb = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    size_t off0 = bomb.size(); bomb.push_back(0x4F); bomb.push_back(0x11); bomb.push_back(0x20); bomb.push_back(0x00);
    bomb.insert(bomb.end(), 8192, 0x41);
    size_t off1 = bomb.size(); bomb.push_back(0xAF); bomb.push_back(0x11); bomb.push_back(4000 >> 8); bomb.push_back(4000 & 0xFF);
    for (int i = 0; i < 4000; ++i) { bomb.push_back(0); bomb.push_back(0); }
    size_t table = bomb.size();
    for (size_t o : {off0, off1}) for (int s = 3; s >= 0; --s) bomb.push_back((uint8_t) (o >> (8 * s)));
    Bytes tr(32, 0); tr[6] = 4; tr[7] = 2; tr[15] = 2; tr[23] = 1; for (int s = 0; s < 8; ++s) tr[24 + s] = (uint8_t) (table >> (8 * (7 - s)));
    bomb.insert(bomb.end(), tr.begin(), tr.end());
    CHECK(!plist::parseBinary(bomb.data(), bomb.size()));
}

static void testNal() {
    Bytes f = {0, 0, 0, 3, 0x65, 1, 2, 0, 0, 0, 2, 0x41, 9};
    bool k; CHECK(nal::avccToAnnexB(f.data(), f.size(), k) && k);
    CHECK((f == Bytes{0, 0, 0, 1, 0x65, 1, 2, 0, 0, 0, 1, 0x41, 9}));
    Bytes bad = {0, 0, 0, 9, 0x65, 1}; CHECK(!nal::avccToAnnexB(bad.data(), bad.size(), k));
    Bytes avcc = {1, 0x64, 0, 0x28, 0xFF, 0xE1, 0, 3, 0x67, 0xAA, 0xBB, 1, 0, 2, 0x68, 0xCC};
    Bytes s, p; CHECK(nal::parseAvcC(avcc.data(), avcc.size(), s, p));
    CHECK((s == Bytes{0, 0, 0, 1, 0x67, 0xAA, 0xBB}) && (p == Bytes{0, 0, 0, 1, 0x68, 0xCC}));
    for (size_t cut = 0; cut < avcc.size(); ++cut) CHECK(!nal::parseAvcC(avcc.data(), cut, s, p));
}

static Bytes infoTxt();

// Nomi scelti dall'utente: UTF-8 valido e senza caratteri pericolosi; nel plist diventano UTF-16.
static void testNames() {
    uint8_t seed[32] = {7}, pk[32], sk[64]; crypto_sign_seed_keypair(pk, sk, seed);
    ReceiverInfo i; i.deviceId = "02:11:22:33:44:55"; i.pairingId = "0f8e2b4a-1234-4abc-9def-0123456789ab";
    i.publicKey.assign(pk, pk + 32); i.features = 0x5A7FFEE6; i.model = "AppleTV3,2"; i.sourceVersion = "220.68"; i.txtAirPlay = infoTxt();
    const std::string good = "Salotto più bello \xF0\x9F\x98\x80";   // accenti + emoji (fuori dal piano base)
    i.name = good;
    auto p = buildInfoPlists(i);
    CHECK(p.has_value());
    if (p) {
        auto v = plist::parseBinary(p->full.data(), p->full.size());
        CHECK(v && *v->asDict()->find("name")->asString() == good);
        std::ofstream("name.plist", std::ios::binary).write((const char*) p->full.data(), p->full.size());
        CHECK(std::system("plutil -lint name.plist > /dev/null") == 0);   // il parser di Apple lo accetta
    }
    const char* bad[] = {"", "Tab\there", "a\x7F", "\u202Eevil",  // U+202E: inverte il testo
                         "\xC0\xAF", "\xED\xA0\x80", "\xF0\x9F\x98"};          // sovralungo, surrogato, troncato
    for (const char* b : bad) { i.name = b; CHECK(!buildInfoPlists(i).has_value()); }
    i.name = std::string(64, 'a'); CHECK(!buildInfoPlists(i).has_value());
    i.name = std::string(63, 'a'); CHECK(buildInfoPlists(i).has_value());
}

// ---------------------------------------------------------------- PIN (pair-setup-pin)

static std::string hexOf(const Bytes& b) { static const char* d = "0123456789abcdef"; std::string s; for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; } return s; }
static Bytes fromHex(const std::string& h) { Bytes b; for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t) std::stoi(h.substr(i, 2), nullptr, 16)); return b; }

// Esegue il client SRP indipendente in Python: restituisce A, M1, M2 atteso, K.
static bool pySrp(const std::string& user, const std::string& pin, const Bytes& salt, const Bytes& B, Bytes out[4]) {
    std::string cmd = "python3 -I srp_client.py '" + user + "' " + pin + " " + hexOf(salt) + " " + hexOf(B);
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return false;
    char buf[4096] = {}; size_t n = fread(buf, 1, sizeof buf - 1, f); pclose(f);
    std::string line(buf, n); size_t pos = 0;
    for (int i = 0; i < 4; ++i) { size_t sp = line.find_first_of(" \n", pos); out[i] = fromHex(line.substr(pos, sp - pos)); pos = sp + 1; }
    return out[0].size() > 0 && out[1].size() == 20;
}

// pair-verify con una chiave Ed25519 data (quella associata); code = esito della fase 1.
static bool verifyWith(int fd, const uint8_t serverPk[32], const uint8_t edPk[32], const uint8_t edSk[64], int* bodyLen) {
    uint8_t xPk[32], xSk[32], shared[32]; randombytes_buf(xSk, 32); crypto_scalarmult_base(xPk, xSk);
    Bytes b1 = {1, 0, 0, 0}; b1.insert(b1.end(), xPk, xPk + 32); b1.insert(b1.end(), edPk, edPk + 32);
    Resp r = rtsp(fd, "POST", "/pair-verify", b1, "", "HTTP/1.1");
    *bodyLen = (int) r.body.size();
    if (r.code != 200 || r.body.size() != 96) return false;
    if (crypto_scalarmult(shared, xSk, r.body.data()) != 0) return false;
    uint8_t key[16], iv[16]; std::string sk = "Pair-Verify-AES-Key", si = "Pair-Verify-AES-IV";
    sha512_16(Bytes(sk.begin(), sk.end()), shared, 32, key); sha512_16(Bytes(si.begin(), si.end()), shared, 32, iv);
    // la firma della TV deve essere valida con la sua chiave pubblica
    uint8_t ssig[64], sm[64]; memcpy(ssig, r.body.data() + 32, 64); ctr(key, iv, 0, ssig, 64);
    memcpy(sm, r.body.data(), 32); memcpy(sm + 32, xPk, 32);
    if (crypto_sign_verify_detached(ssig, sm, 64, serverPk) != 0) return false;
    uint8_t m[64], my[64]; memcpy(m, xPk, 32); memcpy(m + 32, r.body.data(), 32); crypto_sign_detached(my, nullptr, m, 64, edSk);
    ctr(key, iv, 64, my, 64);
    Bytes b2 = {0, 0, 0, 0}; b2.insert(b2.end(), my, my + 64);
    return rtsp(fd, "POST", "/pair-verify", b2, "", "HTTP/1.1").code == 200;
}

// Fasi 1-3 di pair-setup-pin. Restituisce l'esito della fase 2 (true = PIN accettato).
static bool pinPair(int fd, TestSink& sink, const uint8_t serverPk[32], const uint8_t edPk[32], bool wrongPin, bool* phase3ok) {
    *phase3ok = false;
    if (rtsp(fd, "POST", "/pair-pin-start", {}, "", "HTTP/1.1").code != 200) return false;
    Event e; if (!sink.waitFor("pin", &e)) return false;
    std::string pin(e.a.begin(), e.a.end());
    CHECK(pin.size() == 4);
    if (wrongPin) pin = pin == "0000" ? "0001" : "0000";
    const std::string user = "A1:B2:C3:D4:E5:F6";
    Resp r = rtsp(fd, "POST", "/pair-setup-pin", *plist::toBinary(plist::Dict().set("method", "pin").set("user", user)),
                  "Content-Type: application/x-apple-binary-plist\r\n", "HTTP/1.1");
    auto p1 = plist::parseBinary(r.body.data(), r.body.size());
    if (r.code != 200 || !p1) return false;
    Bytes salt = *p1->asDict()->find("salt")->asData(), B = *p1->asDict()->find("pk")->asData();
    CHECK(salt.size() == 16);
    Bytes v[4]; if (!pySrp(user, pin, salt, B, v)) { fprintf(stderr, "client SRP python fallito\n"); return false; }
    r = rtsp(fd, "POST", "/pair-setup-pin", *plist::toBinary(plist::Dict().set("pk", v[0]).set("proof", v[1])),
             "Content-Type: application/x-apple-binary-plist\r\n", "HTTP/1.1");
    if (r.code != 200) return false;
    auto p2 = plist::parseBinary(r.body.data(), r.body.size());
    CHECK(p2 && *p2->asDict()->find("proof")->asData() == v[2]);   // M2 della TV = quello calcolato da Python
    // Fase 3: chiave e IV da K (SHA-512), IV[15]++ per cifrare la nostra chiave, ancora ++ per la risposta.
    uint8_t key[16], iv[16]; std::string sk = "Pair-Setup-AES-Key", si = "Pair-Setup-AES-IV";
    sha512_16(Bytes(sk.begin(), sk.end()), v[3].data(), v[3].size(), key);
    sha512_16(Bytes(si.begin(), si.end()), v[3].data(), v[3].size(), iv);
    iv[15]++;
    mbedtls_gcm_context g; mbedtls_gcm_init(&g); mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    uint8_t epk[32], tag[16];
    mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 32, iv, 16, nullptr, 0, edPk, epk, 16, tag);
    r = rtsp(fd, "POST", "/pair-setup-pin", *plist::toBinary(plist::Dict().set("epk", Bytes(epk, epk + 32)).set("authTag", Bytes(tag, tag + 16))),
             "Content-Type: application/x-apple-binary-plist\r\n", "HTTP/1.1");
    if (r.code == 200) {
        auto p3 = plist::parseBinary(r.body.data(), r.body.size());
        iv[15]++;
        uint8_t spk[32];
        const Bytes& se = *p3->asDict()->find("epk")->asData(); const Bytes& st = *p3->asDict()->find("authTag")->asData();
        *phase3ok = mbedtls_gcm_auth_decrypt(&g, 32, iv, 16, nullptr, 0, st.data(), 16, se.data(), spk) == 0 && memcmp(spk, serverPk, 32) == 0;
    }
    mbedtls_gcm_free(&g);
    return true;
}

static void testPin(const ReceiverInfo& baseInfo, const uint8_t seed[32], const uint8_t serverPk[32]) {
    auto sink = std::make_shared<TestSink>();
    AirPlayServer server(sink);
    AccessConfig acc; acc.pin = PinMode::FirstTime;
    const int port = server.start(0, baseInfo, seed, acc);
    CHECK(port > 0);
    uint8_t edPk[32], edSk[64]; crypto_sign_keypair(edPk, edSk);
    int len = 0;

    // pair-setup semplice: rifiutato con il PIN attivo (altrimenti aggirerebbe il PIN)
    { int fd = connectTcp(port); CHECK(rtsp(fd, "POST", "/pair-setup", Bytes(edPk, edPk + 32), "", "HTTP/1.1").code == 470); close(fd); }
    // dispositivo sconosciuto: pair-verify risponde vuoto (iOS passa a pair-pin-start)
    { int fd = connectTcp(port); CHECK(!verifyWith(fd, serverPk, edPk, edSk, &len) && len == 0); close(fd); }
    // SETUP senza pairing: rifiutato
    { FakeIphone p; p.fd = connectTcp(port); CHECK(p.fairplay()); CHECK(rtsp(p.fd, "SETUP", "rtsp://x/1", setupBody(rnd(72), rnd(16), 0)).code == 470); close(p.fd); }

    // PIN corretto: associazione completa, poi pair-verify e sessione
    {
        FakeIphone p; p.fd = connectTcp(port);
        bool ok3 = false;
        CHECK(pinPair(p.fd, *sink, serverPk, edPk, false, &ok3));
        CHECK(ok3);
        Event e; CHECK(sink->waitFor("trusted", &e) && e.a == Bytes(edPk, edPk + 32));
        CHECK(sink->waitFor("pinoff"));
        CHECK(verifyWith(p.fd, serverPk, edPk, edSk, &len));
        CHECK(p.fairplay());
        CHECK(rtsp(p.fd, "SETUP", "rtsp://x/1", setupBody(rnd(72), rnd(16), 0)).code == 200);
        CHECK(sink->waitFor("start"));
        rtsp(p.fd, "TEARDOWN", "rtsp://x/1");
        close(p.fd);
        CHECK(sink->waitFor("end"));
    }
    // connessione successiva: il dispositivo associato entra senza PIN, uno sconosciuto no
    { int fd = connectTcp(port); CHECK(verifyWith(fd, serverPk, edPk, edSk, &len)); close(fd); }
    { uint8_t o[32], os[64]; crypto_sign_keypair(o, os); int fd = connectTcp(port); CHECK(!verifyWith(fd, serverPk, o, os, &len) && len == 0); close(fd); }

    // 3 PIN sbagliati: blocco, e pair-pin-start risponde 503
    for (int i = 0; i < 3; ++i) {
        int fd = connectTcp(port); bool ok3 = true;
        pinPair(fd, *sink, serverPk, edPk, true, &ok3);
        CHECK(!ok3);
        close(fd);
    }
    { int fd = connectTcp(port); CHECK(rtsp(fd, "POST", "/pair-pin-start", {}, "", "HTTP/1.1").code == 503); close(fd); }
    server.stop();

    // modalità "sempre": anche il dispositivo già associato deve inserire il PIN
    AirPlayServer always(sink);
    AccessConfig acc2; acc2.pin = PinMode::Always; acc2.trusted.push_back({}); memcpy(acc2.trusted[0].data(), edPk, 32);
    const int port2 = always.start(0, baseInfo, seed, acc2);
    { int fd = connectTcp(port2); CHECK(!verifyWith(fd, serverPk, edPk, edSk, &len) && len == 0); close(fd); }
    always.stop();
    fprintf(stderr, "test PIN completati\n");
}

static Bytes infoTxt() { std::string e = "deviceid=02:11:22:33:44:55"; Bytes b{(uint8_t) e.size()}; b.insert(b.end(), e.begin(), e.end()); return b; }

int main() {
    if (sodium_init() < 0) return 1;
    testPlistReader();
    testNal();
    testNames();

    auto sink = std::make_shared<TestSink>();
    AirPlayServer server(sink);
    uint8_t seed[32]; randombytes_buf(seed, 32);
    uint8_t pk[32], sk[64]; crypto_sign_seed_keypair(pk, sk, seed);
    ReceiverInfo info; info.name = "iRPlay"; info.deviceId = "02:11:22:33:44:55"; info.pairingId = "0f8e2b4a-1234-4abc-9def-0123456789ab";
    info.publicKey.assign(pk, pk + 32); info.features = 0x5A7FFEE6; info.model = "AppleTV3,2"; info.sourceVersion = "220.68"; info.txtAirPlay = infoTxt();
    // chiave sbagliata -> deve rifiutare
    { auto bad = info; bad.publicKey[0] ^= 1; AirPlayServer s2(sink); CHECK(s2.start(0, bad, seed) < 0); }
    testPin(info, seed, pk);
    const int port = server.start(0, info, seed);
    CHECK(port > 0);

    // ---- /info
    int fd = connectTcp(port);
    Resp r = rtsp(fd, "GET", "/info");
    CHECK(r.code == 200);
    auto ip = plist::parseBinary(r.body.data(), r.body.size());
    CHECK(ip && ip->asDict() && *ip->asDict()->find("deviceID")->asString() == "02:11:22:33:44:55");

    // ---- pair-verify con firma manomessa su connessione separata
    {
        FakeIphone evil; evil.fd = connectTcp(port);
        uint8_t x[32], xs[32], e[32], es[64]; crypto_sign_keypair(e, es); randombytes_buf(xs, 32); crypto_scalarmult_base(x, xs);
        CHECK(rtsp(evil.fd, "POST", "/pair-setup", Bytes(e, e + 32), "", "HTTP/1.1").code == 200);
        Bytes b1 = {1, 0, 0, 0}; b1.insert(b1.end(), x, x + 32); b1.insert(b1.end(), e, e + 32);
        CHECK(rtsp(evil.fd, "POST", "/pair-verify", b1, "", "HTTP/1.1").code == 200);
        Bytes b2(68, 0x5A); b2[0] = 0;
        CHECK(rtsp(evil.fd, "POST", "/pair-verify", b2, "", "HTTP/1.1").code == 470);
        char c; CHECK(recv(evil.fd, &c, 1, 0) == 0);   // connessione chiusa dal server
        close(evil.fd);
        // fp-setup con mode fuori tabella
        int f2 = connectTcp(port);
        Bytes f1 = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 9, 0};
        CHECK(rtsp(f2, "POST", "/fp-setup", f1, "", "HTTP/1.1").code == 400);
        close(f2);
        // messaggio da 164 byte con mode non valido in [12] (difetto di playfair)
        f2 = connectTcp(port);
        Bytes badm = rnd(164); memcpy(badm.data(), "FPLY\x03\x01\x03\x00", 8); badm[12] = 0x80;
        CHECK(rtsp(f2, "POST", "/fp-setup", badm, "", "HTTP/1.1").code == 400);
        close(f2);
    }

    // ---- pair-verify senza pair-setup (iOS che ha già associato la TV)
    {
        int f3 = connectTcp(port);
        uint8_t x[32], xs[32], e[32], es[64]; crypto_sign_keypair(e, es); randombytes_buf(xs, 32); crypto_scalarmult_base(x, xs);
        Bytes b1 = {1, 0, 0, 0}; b1.insert(b1.end(), x, x + 32); b1.insert(b1.end(), e, e + 32);
        Resp v = rtsp(f3, "POST", "/pair-verify", b1, "", "HTTP/1.1");
        CHECK(v.code == 200 && v.body.size() == 96);
        close(f3);
    }

    // ---- sessione completa
    FakeIphone phone; phone.fd = fd;
    CHECK(phone.pair(pk));
    CHECK(phone.fairplay());

    int tsock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ta{}; ta.sin_family = AF_INET; ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(tsock, (sockaddr*) &ta, sizeof ta); socklen_t tl = sizeof ta; getsockname(tsock, (sockaddr*) &ta, &tl);
    Bytes ekey = rnd(72), eiv = rnd(16);
    r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/123", setupBody(ekey, eiv, ntohs(ta.sin_port)), "Content-Type: application/x-apple-binary-plist\r\n");
    CHECK(r.code == 200);
    auto sp = plist::parseBinary(r.body.data(), r.body.size());
    CHECK(sp && sp->asDict()->find("timingPort") && *sp->asDict()->find("timingPort")->asUInt() > 0);
    Event e; CHECK(sink->waitFor("start", &e) && std::string(e.a.begin(), e.a.end()) == "iPhone di Test");
    { pollfd p{tsock, POLLIN, 0}; uint8_t tb[64]; CHECK(poll(&p, 1, 2000) == 1 && recv(tsock, tb, 64, 0) == 32 && tb[1] == 0xD2); }
    CHECK(rtsp(fd, "RECORD", "rtsp://127.0.0.1/123").code == 200);

    uint8_t skey[16]; phone.sessionKey(ekey, skey);

    // ---- video
    const uint64_t streamId = 0xFEDCBA9876543210ull;
    r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/123", *plist::toBinary(plist::Dict().set("streams",
            plist::Array{plist::Dict().set("type", 110).set("streamConnectionID", streamId)})));
    CHECK(r.code == 200);
    auto vs = plist::parseBinary(r.body.data(), r.body.size());
    const uint16_t vport = (uint16_t) *vs->asDict()->find("streams")->asArray()->at(0).asDict()->find("dataPort")->asUInt();
    int vfd = connectTcp(vport); CHECK(vfd >= 0);
    CHECK(sink->waitFor("vstart"));
    auto sendPkt = [&](uint8_t type, const Bytes& payload, uint64_t ts) {
        Bytes h(128, 0); uint32_t n = payload.size(); memcpy(h.data(), &n, 4); h[4] = type; h[6] = 0x16; memcpy(h.data() + 8, &ts, 8);
        h.insert(h.end(), payload.begin(), payload.end()); send(vfd, h.data(), h.size(), 0);
    };
    sendPkt(1, Bytes{1, 0x64, 0, 0x28, 0xFF, 0xE1, 0, 3, 0x67, 0xAA, 0xBB, 1, 0, 2, 0x68, 0xCC}, 0);
    CHECK(sink->waitFor("vcfg", &e) && e.a.size() == 7 && e.b.size() == 6);
    // chiave video calcolata in modo indipendente
    uint8_t vkey[16], viv[16];
    { char b[64]; int n = snprintf(b, sizeof b, "AirPlayStreamKey%llu", (unsigned long long) streamId); sha512_16(Bytes(b, b + n), skey, 16, vkey);
      n = snprintf(b, sizeof b, "AirPlayStreamIV%llu", (unsigned long long) streamId); sha512_16(Bytes(b, b + n), skey, 16, viv); }
    Bytes allPlain; std::vector<Bytes> frames;
    frames.push_back(Bytes{0, 0, 0, 5, 0x65, 1, 2, 3, 4, 0, 0, 0, 2, 0x06, 5});            // 15 byte (non allineato)
    { Bytes f2 = {0, 0, 0, 33, 0x41}; Bytes rest = rnd(32); f2.insert(f2.end(), rest.begin(), rest.end()); frames.push_back(f2); }
    frames.push_back(Bytes{0, 0, 0, 2, 0x41, 7});
    size_t ks = 0;
    for (auto& fr : frames) {
        Bytes enc = fr; ctr(vkey, viv, ks, enc.data(), enc.size()); ks += enc.size();
        uint64_t ts = (5ull << 32) | 0x80000000u; sendPkt(0, enc, ts);
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        CHECK(sink->waitFor("frame", &e));
        Bytes exp = frames[i]; bool k; nal::avccToAnnexB(exp.data(), exp.size(), k);
        if (i == 0) { Bytes pre = {0, 0, 0, 1, 0x67, 0xAA, 0xBB, 0, 0, 0, 1, 0x68, 0xCC}; pre.insert(pre.end(), exp.begin(), exp.end()); exp = pre; CHECK(e.flag); CHECK(e.i == 5500000); }
        CHECK(e.a == exp);
    }
    sendPkt(2, {}, 0);   // heartbeat

    // ---- audio
    r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/123", *plist::toBinary(plist::Dict().set("streams",
            plist::Array{plist::Dict().set("type", 96).set("ct", 8).set("controlPort", 6001).set("spf", 480)})));
    CHECK(r.code == 200);
    CHECK(sink->waitFor("astart", &e) && e.i == 8);
    auto as = plist::parseBinary(r.body.data(), r.body.size());
    const uint16_t aport = (uint16_t) *as->asDict()->find("streams")->asArray()->at(0).asDict()->find("dataPort")->asUInt();
    int usock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in aa{}; aa.sin_family = AF_INET; aa.sin_port = htons(aport); aa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Bytes plain = rnd(37);
    auto sendAudio = [&](uint16_t seq) {
        Bytes enc = plain; mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, skey, 128);
        uint8_t iv[16]; memcpy(iv, eiv.data(), 16); mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, 32, iv, enc.data(), enc.data()); mbedtls_aes_free(&c);
        Bytes pkt = {0x80, 0xE0, (uint8_t) (seq >> 8), (uint8_t) seq, 0, 0, 0, 1, 0, 0, 0, 2}; pkt.insert(pkt.end(), enc.begin(), enc.end());
        sendto(usock, pkt.data(), pkt.size(), 0, (sockaddr*) &aa, sizeof aa);
    };
    sendAudio(100); sendAudio(100); sendAudio(101);
    CHECK(sink->waitFor("aframe", &e) && e.a == plain);
    CHECK(sink->waitFor("aframe", &e) && e.a == plain);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!sink->has("aframe"));   // duplicato scartato

    // ---- volume
    CHECK(rtsp(fd, "SET_PARAMETER", "rtsp://x", Bytes{'v','o','l','u','m','e',':',' ','-','1','5','.','5','\r','\n'}, "Content-Type: text/parameters\r\n").code == 200);
    CHECK(sink->waitFor("vol", &e) && e.f == -15.5f);
    r = rtsp(fd, "GET_PARAMETER", "rtsp://x", Bytes{'v','o','l','u','m','e','\r','\n'}, "Content-Type: text/parameters\r\n");
    CHECK(std::string(r.body.begin(), r.body.end()) == "volume: -15.500000\r\n");
    CHECK(rtsp(fd, "SET_PARAMETER", "rtsp://x", Bytes{'v','o','l','u','m','e',':',' ','n','a','n'}, "Content-Type: text/parameters\r\n").code == 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); CHECK(!sink->has("vol"));
    CHECK(rtsp(fd, "POST", "/feedback").code == 200);

    // ---- secondo iPhone: occupato
    {
        FakeIphone other; other.fd = connectTcp(port);
        CHECK(other.fairplay());
        Resp o = rtsp(other.fd, "SETUP", "rtsp://x/1", setupBody(rnd(72), rnd(16), 0));
        CHECK(o.code == 453);
        close(other.fd);
    }

    // ---- watchdog: nessun dato per > 3 s
    auto t0 = std::chrono::steady_clock::now();
    CHECK(sink->waitFor("end", &e, 6000) && e.i == (int) EndReason::Watchdog);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "watchdog scattato dopo %lld ms\n", (long long) dt);
    CHECK(sink->waitFor("vstop")); CHECK(sink->waitFor("astop"));
    close(vfd); close(fd); close(usock);

    // ---- TEARDOWN completo e stop locale
    for (int round = 0; round < 2; ++round) {
        FakeIphone p2; p2.fd = connectTcp(port);
        CHECK(p2.pair(pk)); CHECK(p2.fairplay());
        CHECK(rtsp(p2.fd, "SETUP", "rtsp://x/2", setupBody(rnd(72), rnd(16), 0)).code == 200);
        CHECK(sink->waitFor("start"));
        if (round == 0) {
            r = rtsp(p2.fd, "TEARDOWN", "rtsp://x/2");
            CHECK(r.code == 200 && r.head.find("Connection: close") != std::string::npos);
            CHECK(sink->waitFor("end", &e) && e.i == (int) EndReason::Teardown);
        } else {
            server.stopActiveSession();
            CHECK(sink->waitFor("end", &e) && e.i == (int) EndReason::Local);
        }
        close(p2.fd);
    }

    // ---- input malformato
    {
        int g = connectTcp(port);
        const char* junk = "GARBAGE\r\n\r\n";
        send(g, junk, strlen(junk), 0);
        char buf[256]; ssize_t n = recv(g, buf, sizeof buf - 1, 0); buf[n > 0 ? n : 0] = 0;
        CHECK(strstr(buf, "400") != nullptr);
        close(g);
        g = connectTcp(port);
        Resp s = rtsp(g, "SETUP", "rtsp://x", Bytes{'n', 'o', 't', 'p', 'l', 'i', 's', 't'});
        CHECK(s.code == 400);
        close(g);
        g = connectTcp(port);
        CHECK(rtsp(g, "SETUP", "rtsp://x", *plist::toBinary(plist::Dict().set("streams", plist::Array{plist::Dict().set("type", 110)}))).code == 455);
        close(g);
    }

    server.stop();
    fprintf(stderr, failures ? "\n%d FALLIMENTI\n" : "\nTUTTI I TEST SUPERATI\n", failures);
    return failures ? 1 : 0;
}
