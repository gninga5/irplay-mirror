/*
 * parser_diff.cpp
 * Fuzzing differenziale dei parser di rete: per ogni input (generato valido, poi alterato)
 * il parser dell'app (Rust, tramite gli involucri C++) deve dare esattamente lo stesso
 * risultato della versione C++ di riferimento (tools/hosttest/reference), che l'app ha
 * usato fino alla fase 4. Uso: parser_diff [iterazioni per parser, predefinito 200000]
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "reference.h"

using Bytes = std::vector<uint8_t>;
using namespace irplay;

static std::mt19937_64 rng(20261116);
static uint64_t R(uint64_t n) { return n ? rng() % n : 0; }
static long failures = 0;

#define DIFF(cond, what, input)                                                              \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            if (failures++ < 5) {                                                            \
                fprintf(stderr, "DIFFERENZA %s (riga %d), input di %zu byte:\n", what, __LINE__, (input).size()); \
                for (size_t z = 0; z < (input).size() && z < 160; ++z) fprintf(stderr, "%02x", (unsigned) (uint8_t) (input)[z]); \
                fprintf(stderr, "\n");                                                       \
            }                                                                                \
        }                                                                                    \
    } while (0)

// Alterazioni casuali: byte cambiati, inseriti, tolti, troncamento, blocchi duplicati.
template <class T> static void mutate(T& m) {
    const int ops = 1 + (int) R(4);
    for (int k = 0; k < ops; ++k) {
        switch (R(6)) {
            case 0: if (!m.empty()) m[R(m.size())] = (char) rng(); break;
            case 1: m.insert(m.begin() + (long) R(m.size() + 1), (char) rng()); break;
            case 2: if (!m.empty()) m.erase(m.begin() + (long) R(m.size())); break;
            case 3: if (!m.empty()) m.resize(R(m.size())); break;
            case 4: if (!m.empty()) { size_t a = R(m.size()), n = 1 + R(std::min<size_t>(32, m.size() - a)); T blk(m.begin() + (long) a, m.begin() + (long) (a + n)); m.insert(m.begin() + (long) R(m.size() + 1), blk.begin(), blk.end()); } break;
            case 5: { static const char* toks[] = {"\r\n", "\n", ":", ",", "\"", "/", " ", "#", "=", "\0", "\xff", "0", "9"}; const char* t = toks[R(13)]; size_t l = t[0] ? strlen(t) : 1; m.insert(m.begin() + (long) R(m.size() + 1), t, t + l); } break;
        }
    }
}

// ------------------------------------------------------------------------------- plist

static bool same(const plist::Value& a, const plist::Value& b) {
    if (a.v.index() != b.v.index()) return false;
    if (auto x = std::get_if<double>(&a.v)) { const double y = std::get<double>(b.v); return std::memcmp(x, &y, sizeof y) == 0; }
    if (auto x = a.asArray()) {
        const auto& y = *b.asArray();
        if (x->size() != y.size()) return false;
        for (size_t i = 0; i < x->size(); ++i) if (!same((*x)[i], y[i])) return false;
        return true;
    }
    if (auto x = a.asDict()) {
        const auto& y = *b.asDict();
        if (x->keys != y.keys || x->values.size() != y.values.size()) return false;
        for (size_t i = 0; i < x->values.size(); ++i) if (!same(x->values[i], y.values[i])) return false;
        return true;
    }
    if (auto x = a.asBool()) return *x == *b.asBool();
    if (auto x = a.asUInt()) return *x == *b.asUInt();
    if (auto x = a.asString()) return *x == *b.asString();
    return *a.asData() == *b.asData();
}

static plist::Value randomValue(int depth) {
    switch (R(depth > 3 ? 5 : 7)) {
        case 0: return plist::Value(R(2) == 1);
        case 1: return plist::Value((uint64_t) (R(2) ? R(300) : rng()));
        case 2: return plist::Value((double) (int64_t) rng() / 7.0);
        case 3: { std::string s; for (size_t i = 0, n = R(40); i < n; ++i) s.push_back(R(8) ? (char) ('a' + R(26)) : (char) rng()); return plist::Value(s); }
        case 4: { plist::Data d(R(64)); for (auto& c : d) c = (uint8_t) rng(); return plist::Value(d); }
        case 5: { plist::Array a; for (size_t i = 0, n = R(6); i < n; ++i) a.push_back(randomValue(depth + 1)); return plist::Value(a); }
        default: { plist::Dict d; for (size_t i = 0, n = R(6); i < n; ++i) d.set("k" + std::to_string(R(10)), randomValue(depth + 1)); return plist::Value(d); }
    }
}

// Plist costruito a mano con marker scelti a caso (UID, reali a 32 bit, date, UTF-16,
// interi da 16 byte, riferimenti condivisi o ciclici): copre ciò che il writer non produce.
static Bytes randomRawPlist() {
    const size_t n = 1 + R(12);
    std::vector<Bytes> objs(n);
    for (size_t i = 0; i < n; ++i) {
        Bytes& o = objs[i];
        switch (R(11)) {
            case 0: o = {(uint8_t) (R(2) ? 0x08 : 0x09)}; break;
            case 1: { uint8_t info = (uint8_t) R(5); o = {(uint8_t) (0x10 | info)}; for (size_t k = 0; k < (1u << info); ++k) o.push_back((uint8_t) rng()); break; }
            case 2: o = {0x22}; for (int k = 0; k < 4; ++k) o.push_back((uint8_t) rng()); break;
            case 3: o = {(uint8_t) (R(2) ? 0x23 : 0x33)}; for (int k = 0; k < 8; ++k) o.push_back((uint8_t) rng()); break;
            case 4: case 5: { size_t l = R(20); o = {(uint8_t) (0x50 | std::min<size_t>(l, 15))}; if (l >= 15) o.insert(o.end(), {0x10, (uint8_t) l}); for (size_t k = 0; k < l; ++k) o.push_back((uint8_t) ('a' + R(26))); break; }
            case 6: { size_t l = R(8); o = {(uint8_t) (0x60 | l)}; for (size_t k = 0; k < l; ++k) { uint16_t u = R(3) == 0 ? (uint16_t) (0xD800 + R(0x800)) : (uint16_t) rng(); o.push_back((uint8_t) (u >> 8)); o.push_back((uint8_t) u); } break; }
            case 7: { uint8_t l = (uint8_t) R(8); o = {(uint8_t) (0x80 | l)}; for (int k = 0; k <= l; ++k) o.push_back((uint8_t) rng()); break; }
            case 8: { size_t l = R(5); o = {(uint8_t) (0x40 | l)}; for (size_t k = 0; k < l; ++k) o.push_back((uint8_t) rng()); break; }
            case 9: { size_t l = R(5); o = {(uint8_t) (0xA0 | l)}; for (size_t k = 0; k < l; ++k) o.push_back((uint8_t) R(n)); break; }
            default: { size_t l = R(4); o = {(uint8_t) (0xD0 | l)}; for (size_t k = 0; k < 2 * l; ++k) o.push_back((uint8_t) R(n)); break; }
        }
    }
    Bytes out = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<size_t> offs;
    for (auto& o : objs) { offs.push_back(out.size()); out.insert(out.end(), o.begin(), o.end()); }
    const size_t table = out.size();
    for (size_t o : offs) { out.push_back((uint8_t) (o >> 8)); out.push_back((uint8_t) o); }
    uint8_t t[32] = {};
    t[6] = 2; t[7] = 1; t[15] = (uint8_t) n; t[23] = (uint8_t) R(n); t[30] = (uint8_t) (table >> 8); t[31] = (uint8_t) table;
    out.insert(out.end(), t, t + 32);
    return out;
}

static void plistDiff(long iters) {
    long valid = 0;
    for (long it = 0; it < iters; ++it) {
        Bytes in;
        if (R(2)) { auto b = plist::toBinary(randomValue(0)); if (!b) continue; in = *b; } else in = randomRawPlist();
        if (R(3)) mutate(in);
        const auto a = plist::parseBinary(in.data(), in.size());
        const auto b = ref::plist::parseBinary(in.data(), in.size());
        DIFF(a.has_value() == b.has_value() && (!a || same(*a, *b)), "plist", in);
        valid += a.has_value();
    }
    printf("plist:     %ld input, %ld validi, differenze %ld\n", iters, valid, failures);
}

// ------------------------------------------------------------------------- DMAP, progress

static Bytes el(const char* tag, const Bytes& d) {
    Bytes v(tag, tag + 4);
    const uint32_t n = (uint32_t) d.size();
    v.insert(v.end(), {(uint8_t) (n >> 24), (uint8_t) (n >> 16), (uint8_t) (n >> 8), (uint8_t) n});
    v.insert(v.end(), d.begin(), d.end());
    return v;
}

static Bytes randomText() {
    Bytes s;
    const size_t n = R(4) == 0 ? 250 + R(20) : R(30);
    for (size_t i = 0; i < n; ++i) {
        if (R(5) == 0) { s.insert(s.end(), {0xC3, 0xA8}); continue; }   // "è"
        if (R(9) == 0) { s.insert(s.end(), {0xF0, 0x9F, 0x98, 0x80}); continue; }
        s.push_back((uint8_t) ('a' + R(26)));
    }
    return s;
}

static void dmapDiff(long iters) {
    static const char* tags[] = {"minm", "asar", "asal", "mlit", "xxxx", "mper"};
    long before = failures, valid = 0;
    for (long it = 0; it < iters; ++it) {
        Bytes inner;
        for (size_t i = 0, n = R(6); i < n; ++i) { const char* t = tags[R(6)]; Bytes e = std::strcmp(t, "mlit") == 0 ? el(t, el("minm", randomText())) : el(t, randomText()); inner.insert(inner.end(), e.begin(), e.end()); }
        Bytes in = R(4) ? el("mlit", inner) : inner;
        if (R(3)) mutate(in);
        nowplaying::Metadata a, b;
        const bool ra = nowplaying::parseDmap(in.data(), in.size(), a), rb = ref::nowplaying::parseDmap(in.data(), in.size(), b);
        DIFF(ra == rb && a.title == b.title && a.artist == b.artist && a.album == b.album, "dmap", in);
        valid += ra;
    }
    printf("DMAP:      %ld input, %ld validi, differenze %ld\n", iters, valid, failures - before);
    before = failures; valid = 0;
    static const char* nums[] = {"0", "1", "44100", "4294967295", "4294967296", "99999999999", "007", "", "-1"};
    for (long it = 0; it < iters; ++it) {
        std::string s = std::string(R(8) ? "progress:" : "progres:") + std::string(R(3), ' ') + nums[R(9)] + "/" + nums[R(9)] + "/" + nums[R(9)] + (R(2) ? "\r\n" : "");
        if (R(3)) mutate(s);
        if (R(20) == 0) s.resize(129, '1');
        uint32_t a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
        const auto* p = (const uint8_t*) s.data();
        const bool ra = nowplaying::parseProgressRtp(p, s.size(), a1, a2, a3), rb = ref::nowplaying::parseProgressRtp(p, s.size(), b1, b2, b3);
        DIFF(ra == rb && (!ra || (a1 == b1 && a2 == b2 && a3 == b3)), "progress", s);
        valid += ra;
    }
    printf("progress:  %ld input, %ld validi, differenze %ld\n", iters, valid, failures - before);
}

// -------------------------------------------------------------------------------- RTSP

static std::string randomRequest() {
    static const char* methods[] = {"GET", "POST", "SETUP", "SET_PARAMETER", "TEARDOWN", "OPTIONS", "FLUSH", "get"};
    static const char* uris[] = {"/info", "/action", "rtsp://192.168.1.2/1234", "*", "/fp-setup", "/play"};
    static const char* protos[] = {"RTSP/1.0", "HTTP/1.1", "HTTP/1.0", "RTSP/1.0 "};
    std::string m = methods[R(8)];
    std::string s = m + " " + uris[R(6)] + " " + protos[R(4)] + "\r\n";
    const size_t body = R(3) == 0 ? R(100) : 0;
    for (size_t i = 0, n = R(8); i < n; ++i) {
        switch (R(8)) {
            case 0: s += "CSeq: " + std::to_string(R(100)) + "\r\n"; break;
            case 1: s += std::string(R(2) ? "content-length" : "Content-Length") + ":" + std::string(R(3), ' ') + std::to_string(R(4) ? body : R(5000000)) + std::string(R(2), '\t') + "\r\n"; break;
            case 2: s += "Transfer-Encoding: chunked\r\n"; break;
            case 3: s += "X-Apple-Session-ID: " + std::to_string(rng()) + "\r\n"; break;
            case 4: s += "Content-Type: application/x-apple-binary-plist\r\n"; break;
            case 5: s += "Bad header\r\n"; break;
            case 6: s += " Spaced : value \r\n"; break;
            default: s += "User-Agent: AirPlay/900.7\r\n"; break;
        }
    }
    s += "\r\n" + std::string(body, 'b');
    if (R(4) == 0) s += "GET /info RTSP/1.0\r\n\r\n";   // richiesta successiva nello stesso buffer
    return s;
}

static void rtspDiff(long iters) {
    long before = failures, complete = 0;
    for (long it = 0; it < iters; ++it) {
        std::string s = randomRequest();
        if (R(3)) mutate(s);
        if (R(50) == 0) s = std::string(16 * 1024 + R(10), 'a') + (R(2) ? "\r\n\r\n" : "");
        Bytes ba(s.begin(), s.end()), bb = ba;
        RtspRequest a, b;
        const int ra = rtsp::parseRequest(ba, a), rb = ref::rtsp::parseRequest(bb, b);
        bool eq = ra == rb && ba == bb;
        if (eq && ra == 1) eq = a.method == b.method && a.uri == b.uri && a.protocol == b.protocol && a.headers == b.headers && a.body == b.body;
        DIFF(eq, "rtsp richiesta", s);
        complete += ra == 1;
    }
    printf("RTSP:      %ld richieste, %ld complete, differenze %ld\n", iters, complete, failures - before);
    before = failures; complete = 0;
    static const char* lines[] = {"HTTP/1.1 200 OK", "EVENT/1.0 200 OK", "RTSP/1.0 404 Not Found", "HTTP/1.1 2000", "http/1.1 200 OK", "ABCDEFGHIJKLMNOPQ/1.1 200 X", "HTTP/1.1 200"};
    for (long it = 0; it < iters; ++it) {
        std::string s = std::string(lines[R(7)]) + "\r\n";
        const size_t body = R(3) == 0 ? R(50) : 0;
        if (R(2)) s += "Content-Length: " + std::to_string(body) + "\r\n";
        if (R(8) == 0) s += "Content-Length: 1\r\n";
        if (R(10) == 0) s += "transfer-encoding: x\r\n";
        if (R(3) == 0) s += "Date: x\r\nnocolon\r\n";
        s += "\r\n" + std::string(body, 'x');
        if (R(3)) mutate(s);
        Bytes ba(s.begin(), s.end()), bb = ba;
        const int ra = rtsp::discardResponse(ba), rb = ref::rtsp::discardResponse(bb);
        DIFF(ra == rb && ba == bb, "rtsp risposta", s);
        complete += ra == 1;
    }
    printf("risposte:  %ld input, %ld complete, differenze %ld\n", iters, complete, failures - before);
}

// --------------------------------------------------------------------------------- HLS

static std::string randomMaster() {
    static const char* codecs[] = {"avc1.4d401f,mp4a.40.5", "avc1.640028,mp4a.40.2", "avc1.64001f,mp4a.40.29", "hvc1.2.4.L123,mp4a.40.2", "avc1.42e01e"};
    static const char* prefixes[] = {"mlhls://localhost", "http://cdn.example", "mlhls://localhost/sub"};
    std::string s = R(20) ? "#EXTM3U\n" : "#EXTM3\n";
    if (R(3) == 0) s += "#EXT-X-VERSION:6\r\n";
    const int groups = (int) R(3);
    for (int g = 0; g < groups; ++g)
        s += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"g" + std::to_string(g) + "\",NAME=\"a\",DEFAULT=YES,URI=\"" + prefixes[R(3)] + "/a" + std::to_string(g) + ".m3u8\"\n";
    if (R(4) == 0) s += "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",BASE-URI=\"x\",URI=\"mlhls://localhost/s.m3u8\"\n";
    for (size_t v = 0, n = 1 + R(5); v < n; ++v) {
        s += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(R(9000000)) + ",CODECS=\"" + codecs[R(5)] + "\"";
        if (groups) s += ",AUDIO=\"g" + std::to_string(R(groups)) + "\"";
        s += R(4) ? "\n" : "\r\n";
        if (R(6) == 0) s += "\n";
        s += std::string(prefixes[R(3)]) + "/v" + std::to_string(R(4)) + ".m3u8\n";
    }
    if (R(10) == 0) s += "#EXTINF:5,\nseg.ts\n";
    return s;
}

static std::string randomMedia() {
    std::string s = "#EXTM3U\n#EXT-X-TARGETDURATION:5\n";
    std::vector<std::string> params;
    for (size_t i = 0, n = 1 + R(4); i < n; ++i) params.push_back(R(10) ? "p" + std::to_string(i) : "");
    std::string plist;
    for (size_t i = 0; i < params.size(); ++i) plist += (i ? "," : "") + params[i];
    static const char* bases[] = {"https://rr1.googlevideo.com/videoplayback", "http://x.y", "file:///sdcard/x", "https://u@evil/x", "https://"};
    if (R(5)) s += "#YT-EXT-CONDENSED-URL:BASE-URI=\"" + std::string(bases[R(5)]) + "\",PARAMS=\"" + plist + "\",PREFIX=\"" + (R(8) ? "s/" : "") + "\"\n";
    for (size_t i = 0, n = R(10); i < n; ++i) {
        s += "#EXTINF:5.0,\n";
        std::string seg = R(8) ? "s/" : "other/";
        for (size_t k = 0, m = params.size() + R(3) - 1; k < m; ++k) seg += std::to_string(R(1000)) + "/";
        seg += "end";
        s += seg + (R(5) ? "\n" : "\r\n");
    }
    if (R(2)) s += "#EXT-X-ENDLIST";
    return s;
}

static void hlsDiff(long iters) {
    static const char* prefixes[] = {"mlhls://localhost", "mlhls://localhost/sub", "http://cdn.example", ""};
    long before = failures, ok = 0;
    for (long it = 0; it < iters; ++it) {
        std::string s = R(2) ? randomMaster() : randomMedia();
        if (R(3)) mutate(s);
        const std::string_view pfx = prefixes[R(4)];
        const auto a = hls::rewriteMaster(s, pfx), b = ref::hls::rewriteMaster(s, pfx);
        DIFF(a.has_value() == b.has_value() && (!a || (a->text == b->text && a->mediaUris == b->mediaUris)), "hls master", s);
        const auto c = hls::expandCondensed(s), d = ref::hls::expandCondensed(s);
        DIFF(c == d, "hls condensed", s);
        DIFF(hls::isPlaylist(s) == ref::hls::isPlaylist(s), "hls isPlaylist", s);
        ok += a.has_value() + c.has_value();
    }
    printf("HLS:       %ld playlist, %ld risultati validi, differenze %ld\n", iters, ok, failures - before);
    before = failures; ok = 0;
    static const char* urls[] = {"https://a.b/c?d", "http://h", "https://user:pw@h/x", "file:///etc/hosts", "mlhls://localhost/master.m3u8",
                                 "x://y/master.m3u8", "/master.m3u8", "a/master.m3u8://", "https://", "http://h#f", "HTTPS://h/"};
    for (long it = 0; it < iters; ++it) {
        std::string s = urls[R(11)];
        if (R(2)) mutate(s);
        if (R(30) == 0) s.resize(8190 + R(5), 'a');
        DIFF(hls::isSafeHttpUrl(s) == ref::hls::isSafeHttpUrl(s), "hls url", s);
        DIFF(hls::remotePrefixOf(s) == ref::hls::remotePrefixOf(s), "hls prefix", s);
        ok += hls::isSafeHttpUrl(s);
    }
    printf("URL HLS:   %ld input, %ld sicuri, differenze %ld\n", iters, ok, failures - before);
}

int main(int argc, char** argv) {
    const long iters = argc > 1 ? std::atol(argv[1]) : 200000;
    plistDiff(iters);
    dmapDiff(iters);
    rtspDiff(iters);
    hlsDiff(iters / 2);
    if (failures) { printf("PARSER DIVERSI: %ld differenze\n", failures); return 1; }
    printf("parser Rust identici ai riferimenti C++\n");
    return 0;
}
