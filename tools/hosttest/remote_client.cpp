// Client che simula un iPhone contro iRPlay nell'emulatore (via adb forward).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/aes.h>
#include <sodium.h>

#include "plist_reader.h"
#include "plist_writer.h"
#include "ALACEncoder.h"
#include <cmath>
extern "C" {
#include "playfair.h"
}

using namespace irplay;
using Bytes = std::vector<uint8_t>;

static Bytes readFile(const char* p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), {}); }
static Bytes rnd(size_t n) { Bytes b(n); randombytes_buf(b.data(), n); return b; }

// ICAST_HOST/ICAST_PORT: collegamento diretto a una TV sulla LAN (senza adb forward).
static const char* targetHost() { const char* h = getenv("ICAST_HOST"); return h ? h : "127.0.0.1"; }
static int connectTcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, targetHost(), &a.sin_addr);
    if (connect(fd, (sockaddr*) &a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return fd;
}

struct Resp { int code = -1; Bytes body; };
static Resp rtsp(int fd, const std::string& method, const std::string& uri, const Bytes& body = {}, const char* proto = "RTSP/1.0") {
    static int cseq = 1;
    std::string req = method + " " + uri + " " + proto + "\r\nCSeq: " + std::to_string(cseq++) + "\r\n";
    if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    Bytes out(req.begin(), req.end()); out.insert(out.end(), body.begin(), body.end());
    send(fd, out.data(), out.size(), 0);
    std::string buf; char c;
    timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (buf.find("\r\n\r\n") == std::string::npos) { if (recv(fd, &c, 1, 0) != 1) return {}; buf += c; }
    Resp r; r.code = std::atoi(buf.c_str() + buf.find(' ') + 1);
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

struct Ctr {
    mbedtls_aes_context c; uint8_t nonce[16], block[16]; size_t off = 0;
    Ctr(const uint8_t k[16], const uint8_t iv[16]) { mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, k, 128); memcpy(nonce, iv, 16); }
    void apply(uint8_t* d, size_t n) { mbedtls_aes_crypt_ctr(&c, n, &off, nonce, block, d, d); }
};

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 8;
    if (sodium_init() < 0) return 1;
    const bool audioMode = argc > 2 && std::string(argv[2]) == "audio";
    const bool musicMode = argc > 2 && std::string(argv[2]) == "music";   // ALAC + metadati, come Apple Music
    const char* envPort = getenv("ICAST_PORT");
    int fd = connectTcp(envPort ? (uint16_t) atoi(envPort) : ((audioMode || musicMode) ? 7100 : 7000));
    if (fd < 0) { fprintf(stderr, "connessione a 7000 fallita\n"); return 1; }

    Resp r = rtsp(fd, "GET", "/info");
    auto info = plist::parseBinary(r.body.data(), r.body.size());
    if (r.code != 200 || !info) { fprintf(stderr, "/info fallito\n"); return 1; }
    const Bytes serverPk = *info->asDict()->find("pk")->asData();
    printf("/info OK, modello %s\n", info->asDict()->find("model")->asString()->c_str());

    uint8_t edPk[32], edSk[64], xPk[32], xSk[32], shared[32];
    crypto_sign_keypair(edPk, edSk); randombytes_buf(xSk, 32); crypto_scalarmult_base(xPk, xSk);
    r = rtsp(fd, "POST", "/pair-setup", Bytes(edPk, edPk + 32), "HTTP/1.1");
    if (r.code != 200 || r.body != serverPk) { fprintf(stderr, "pair-setup fallito\n"); return 1; }
    Bytes b1 = {1, 0, 0, 0}; b1.insert(b1.end(), xPk, xPk + 32); b1.insert(b1.end(), edPk, edPk + 32);
    r = rtsp(fd, "POST", "/pair-verify", b1, "HTTP/1.1");
    if (r.code != 200 || r.body.size() != 96) { fprintf(stderr, "pair-verify 1 fallito\n"); return 1; }
    const uint8_t* sPk = r.body.data(); uint8_t sig[64]; memcpy(sig, r.body.data() + 32, 64);
    crypto_scalarmult(shared, xSk, sPk);
    uint8_t key[16], iv[16];
    std::string sk = "Pair-Verify-AES-Key", si = "Pair-Verify-AES-IV";
    sha512_16(Bytes(sk.begin(), sk.end()), shared, 32, key); sha512_16(Bytes(si.begin(), si.end()), shared, 32, iv);
    Ctr pv(key, iv); pv.apply(sig, 64);
    uint8_t m[64]; memcpy(m, sPk, 32); memcpy(m + 32, xPk, 32);
    if (crypto_sign_verify_detached(sig, m, 64, serverPk.data()) != 0) { fprintf(stderr, "firma della TV non valida\n"); return 1; }
    uint8_t my[64]; memcpy(m, xPk, 32); memcpy(m + 32, sPk, 32); crypto_sign_detached(my, nullptr, m, 64, edSk);
    pv.apply(my, 64);
    Bytes b2 = {0, 0, 0, 0}; b2.insert(b2.end(), my, my + 64);
    if (rtsp(fd, "POST", "/pair-verify", b2, "HTTP/1.1").code != 200) { fprintf(stderr, "pair-verify 2 fallito\n"); return 1; }
    printf("pair-verify OK (firma Ed25519 della TV verificata)\n");

    Bytes f1 = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 1, 0};
    if (rtsp(fd, "POST", "/fp-setup", f1, "HTTP/1.1").code != 200) return 1;
    Bytes msg164 = rnd(164); memcpy(msg164.data(), "FPLY\x03\x01\x03\x00", 8); msg164[12] = 1;
    if (rtsp(fd, "POST", "/fp-setup", msg164, "HTTP/1.1").code != 200) return 1;
    printf("fp-setup OK\n");

    Bytes ekey = rnd(72), eiv = rnd(16);
    plist::Dict sd; sd.set("ekey", ekey).set("eiv", eiv).set("timingProtocol", "NTP").set("timingPort", 0).set("name", "Mac di prova");
    r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/1", *plist::toBinary(sd));
    if (r.code != 200) { fprintf(stderr, "SETUP fallito %d\n", r.code); return 1; }
    rtsp(fd, "RECORD", "rtsp://127.0.0.1/1");
    uint8_t fp[16], skey[16]; { Bytes mm = msg164, e = ekey; playfair_decrypt(mm.data(), e.data(), fp); }
    sha512_16(Bytes(fp, fp + 16), shared, 32, skey);

    if (musicMode) {
        // tono di prova codificato in ALAC (352 campioni per pacchetto, come iOS)
        ALACEncoder enc; enc.SetFrameSize(352);
        AudioFormatDescription in{}, out{};
        in.mSampleRate = 44100; in.mFormatID = kALACFormatLinearPCM; in.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked;
        in.mBytesPerPacket = 4; in.mFramesPerPacket = 1; in.mBytesPerFrame = 4; in.mChannelsPerFrame = 2; in.mBitsPerChannel = 16;
        out.mSampleRate = 44100; out.mFormatID = kALACFormatAppleLossless; out.mFramesPerPacket = 352; out.mChannelsPerFrame = 2; out.mFormatFlags = 1;
        enc.InitializeEncoder(out);
        plist::Dict as; as.set("streams", plist::Array{plist::Dict().set("type", 96).set("ct", 2).set("spf", 352).set("controlPort", 6001)});
        r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/1", *plist::toBinary(as));
        auto ar = plist::parseBinary(r.body.data(), r.body.size());
        if (r.code != 200 || !ar) { fprintf(stderr, "SETUP audio fallito\n"); return 1; }
        const unsigned aport = (unsigned) *ar->asDict()->find("streams")->asArray()->at(0).asDict()->find("dataPort")->asUInt();
        system((std::string(getenv("ADB")) + " emu redir add udp:" + std::to_string(aport) + ":" + std::to_string(aport) + " >/dev/null").c_str());
        auto item = [](const char* tag, const std::string& v) { Bytes b(tag, tag + 4); uint32_t n = v.size();
            for (int i = 3; i >= 0; --i) b.push_back((uint8_t) (n >> (8 * i))); b.insert(b.end(), v.begin(), v.end()); return b; };
        Bytes inner; for (auto& x : {item("minm", "Bohemian Rhapsody"), item("asar", "Queen"), item("asal", "A Night at the Opera")}) inner.insert(inner.end(), x.begin(), x.end());
        Bytes dmap = item("mlit", std::string(inner.begin(), inner.end()));
        auto setp = [&](const Bytes& body, const char* ct) { return rtsp(fd, "SET_PARAMETER", "rtsp://x/1", body, (std::string("Content-Type: ") + ct + "\r\n").c_str()).code; };
        // rtsp() non accetta header extra in questo client: li aggiungiamo nel corpo della richiesta
        (void) setp;
        auto rawSet = [&](const Bytes& body, const std::string& ctype) {
            static int cs = 900;
            std::string req = "SET_PARAMETER rtsp://x/1 RTSP/1.0\r\nCSeq: " + std::to_string(cs++) + "\r\nContent-Type: " + ctype +
                              "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            Bytes o(req.begin(), req.end()); o.insert(o.end(), body.begin(), body.end());
            send(fd, o.data(), o.size(), 0);
            std::string buf; char c; while (buf.find("\r\n\r\n") == std::string::npos && recv(fd, &c, 1, 0) == 1) buf += c;
        };
        rawSet(dmap, "application/x-dmap-tagged");
        // posizione 60 s su 355 s (la durata vera del brano, per la ricerca del testo)
        const uint32_t start = 1000000; std::string prog = "progress: " + std::to_string(start) + "/" + std::to_string(start + 60 * 44100) + "/" + std::to_string(start + 355 * 44100) + "\r\n";
        rawSet(Bytes(prog.begin(), prog.end()), "text/parameters");
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in aa{}; aa.sin_family = AF_INET; aa.sin_port = htons(aport); aa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        mbedtls_aes_context c2; mbedtls_aes_init(&c2); mbedtls_aes_setkey_enc(&c2, skey, 128);
        auto t0 = std::chrono::steady_clock::now();
        const int total = seconds * 44100 / 352;
        std::vector<int16_t> pcm(352 * 2);
        for (int seq = 0; seq < total; ++seq) {
            for (int i = 0; i < 352; ++i) { int16_t v = (int16_t) (3000 * std::sin((seq * 352 + i) * 0.06)); pcm[2 * i] = v; pcm[2 * i + 1] = v; }
            Bytes ob(352 * 4 + 64); int32_t n = 352 * 4;
            enc.Encode(in, out, (unsigned char*) pcm.data(), ob.data(), &n); ob.resize(n);
            size_t full = ob.size() & ~size_t(15); uint8_t ivc[16]; memcpy(ivc, eiv.data(), 16);
            if (full) mbedtls_aes_crypt_cbc(&c2, MBEDTLS_AES_ENCRYPT, full, ivc, ob.data(), ob.data());
            uint32_t ts = seq * 352u;
            Bytes pkt = {0x80, 0x60, (uint8_t) (seq >> 8), (uint8_t) seq, (uint8_t) (ts >> 24), (uint8_t) (ts >> 16), (uint8_t) (ts >> 8), (uint8_t) ts, 0, 0, 0, 1};
            pkt.insert(pkt.end(), ob.begin(), ob.end());
            sendto(us, pkt.data(), pkt.size(), 0, (sockaddr*) &aa, sizeof aa);
            if (seq % 250 == 0) rtsp(fd, "POST", "/feedback");
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((int64_t) (seq + 1) * 352 * 1000000 / 44100));
        }
        printf("inviati %d pacchetti ALAC\n", total);
        // pausa come fa iOS: FLUSH, poi TEARDOWN del solo flusso audio; la connessione resta
        std::string fl = "FLUSH rtsp://x/1 RTSP/1.0\r\nCSeq: 990\r\nRTP-Info: seq=" + std::to_string(total) + ";rtptime=0\r\n\r\n";
        send(fd, fl.data(), fl.size(), 0);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        plist::Dict td; td.set("streams", plist::Array{plist::Dict().set("type", 96)});
        rtsp(fd, "TEARDOWN", "rtsp://127.0.0.1/1", *plist::toBinary(td));
        printf("in pausa (flusso audio chiuso, sessione aperta)\n");
        std::this_thread::sleep_for(std::chrono::seconds(8));
        r = rtsp(fd, "TEARDOWN", "rtsp://127.0.0.1/1");
        printf("TEARDOWN -> %d\n", r.code);
        return 0;
    }

    if (audioMode) {
        plist::Dict as; as.set("streams", plist::Array{plist::Dict().set("type", 96).set("ct", 8).set("spf", 480).set("controlPort", 6001)});
        r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/1", *plist::toBinary(as));
        auto ar = plist::parseBinary(r.body.data(), r.body.size());
        if (r.code != 200 || !ar) { fprintf(stderr, "SETUP audio fallito\n"); return 1; }
        const unsigned aport = (unsigned) *ar->asDict()->find("streams")->asArray()->at(0).asDict()->find("dataPort")->asUInt();
        std::string c = std::string(getenv("ADB")) + " emu redir add udp:" + std::to_string(aport) + ":" + std::to_string(aport) + " >/dev/null";
        system(c.c_str());
        printf("SETUP audio OK, porta UDP %u\n", aport);
        rtsp(fd, "SET_PARAMETER", "rtsp://x", Bytes{'v','o','l','u','m','e',':',' ','-','6','.','0','\r','\n'});
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in aa{}; aa.sin_family = AF_INET; aa.sin_port = htons(aport); aa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const Bytes eld = readFile("eld.bin");
        std::vector<Bytes> fr;
        for (size_t p = 0; p + 2 <= eld.size();) { uint16_t n; memcpy(&n, &eld[p], 2); fr.push_back(Bytes(eld.begin() + p + 2, eld.begin() + p + 2 + n)); p += 2 + n; }
        mbedtls_aes_context c2; mbedtls_aes_init(&c2); mbedtls_aes_setkey_enc(&c2, skey, 128);
        auto t0 = std::chrono::steady_clock::now();
        size_t sent = 0;
        for (int loop = 0; loop < seconds / 5 + 1; ++loop) for (auto& f : fr) {
            Bytes enc = f; uint8_t ivc[16]; memcpy(ivc, eiv.data(), 16);
            size_t full = enc.size() & ~size_t(15);
            if (full) mbedtls_aes_crypt_cbc(&c2, MBEDTLS_AES_ENCRYPT, full, ivc, enc.data(), enc.data());
            uint16_t seq = (uint16_t) sent; uint32_t ts = (uint32_t) (sent * 480);
            Bytes pkt = {0x80, 0x60, (uint8_t) (seq >> 8), (uint8_t) seq, (uint8_t) (ts >> 24), (uint8_t) (ts >> 16), (uint8_t) (ts >> 8), (uint8_t) ts, 0, 0, 0, 1};
            pkt.insert(pkt.end(), enc.begin(), enc.end());
            sendto(us, pkt.data(), pkt.size(), 0, (sockaddr*) &aa, sizeof aa);
            ++sent;
            if (sent % 180 == 0) rtsp(fd, "POST", "/feedback");
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(sent * 480 * 1000000 / 44100));
        }
        printf("inviati %zu pacchetti audio\n", sent);
        r = rtsp(fd, "TEARDOWN", "rtsp://127.0.0.1/1");
        printf("TEARDOWN -> %d\n", r.code);
        return 0;
    }

    const uint64_t streamId = 0x1234567890ABCDEFull;
    plist::Dict vs; vs.set("streams", plist::Array{plist::Dict().set("type", 110).set("streamConnectionID", streamId)});
    r = rtsp(fd, "SETUP", "rtsp://127.0.0.1/1", *plist::toBinary(vs));
    auto vr = plist::parseBinary(r.body.data(), r.body.size());
    if (r.code != 200 || !vr) { fprintf(stderr, "SETUP video fallito\n"); return 1; }
    const unsigned vport = (unsigned) *vr->asDict()->find("streams")->asArray()->at(0).asDict()->find("dataPort")->asUInt();
    printf("SETUP video OK, porta %u\n", vport);
    std::string cmd = std::string(getenv("ADB")) + " forward tcp:" + std::to_string(vport) + " tcp:" + std::to_string(vport) + " >/dev/null";
    if (!getenv("ICAST_HOST") && system(cmd.c_str()) != 0) return 1;
    int vfd = connectTcp((uint16_t) vport);
    if (vfd < 0) { fprintf(stderr, "connessione video fallita\n"); return 1; }

    uint8_t vkey[16], viv[16];
    { char b[64]; int n = snprintf(b, sizeof b, "AirPlayStreamKey%llu", (unsigned long long) streamId); sha512_16(Bytes(b, b + n), skey, 16, vkey);
      n = snprintf(b, sizeof b, "AirPlayStreamIV%llu", (unsigned long long) streamId); sha512_16(Bytes(b, b + n), skey, 16, viv); }
    Ctr video(vkey, viv);

    auto sendPkt = [&](uint8_t type, Bytes payload, uint64_t ts) {
        Bytes h(128, 0); uint32_t n = payload.size(); memcpy(h.data(), &n, 4); h[4] = type; h[6] = type == 1 ? 0x16 : 0; memcpy(h.data() + 8, &ts, 8);
        h.insert(h.end(), payload.begin(), payload.end()); return send(vfd, h.data(), h.size(), 0) == (ssize_t) h.size();
    };
    const Bytes avcc = readFile("avcc.bin"), frames = readFile("frames.bin");
    sendPkt(1, avcc, 0);
    std::vector<std::pair<Bytes, bool>> fl;
    for (size_t p = 0; p + 5 <= frames.size();) { uint32_t n; memcpy(&n, &frames[p], 4); fl.push_back({Bytes(frames.begin() + p + 5, frames.begin() + p + 5 + n), frames[p + 4] != 0}); p += 5 + n; }
    printf("invio %zu frame in loop per %d s\n", fl.size(), seconds);
    auto start = std::chrono::steady_clock::now();
    uint64_t frameNo = 0; int lastFeedback = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
        Bytes f = fl[frameNo % fl.size()].first;
        video.apply(f.data(), f.size());
        uint64_t ts = ((frameNo / 30) << 32) | (uint64_t) (((frameNo % 30) * 0x100000000ull) / 30);
        if (!sendPkt(0, f, ts)) { fprintf(stderr, "invio video interrotto\n"); break; }
        ++frameNo;
        int el = (int) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        if (el / 2 != lastFeedback) { lastFeedback = el / 2; rtsp(fd, "POST", "/feedback"); }
        std::this_thread::sleep_until(start + std::chrono::milliseconds(frameNo * 1000 / 30));
    }
    printf("inviati %llu frame\n", (unsigned long long) frameNo);
    if (argc > 2 && std::string(argv[2]) == "vanish") {
        printf("simulo iPhone sparito: niente più dati\n");
        std::this_thread::sleep_for(std::chrono::seconds(6));
        return 0;
    }
    r = rtsp(fd, "TEARDOWN", "rtsp://127.0.0.1/1");
    printf("TEARDOWN -> %d\n", r.code);
    close(vfd); close(fd);
    return 0;
}
