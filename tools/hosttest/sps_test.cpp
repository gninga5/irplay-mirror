#include <cstdio>
#include <fstream>
#include <vector>
#include "nal_parser.h"
#include "sps_rewriter.h"
using Bytes = std::vector<uint8_t>;
static Bytes rd(const char* p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), {}); }
static void hex(const char* t, const Bytes& b) { printf("%s:", t); for (auto x : b) printf(" %02x", x); printf("\n"); }
int main() {
    Bytes avcc = rd("avcc.bin"), sps, pps, patched, again;
    if (!irplay::nal::parseAvcC(avcc.data(), avcc.size(), sps, pps)) return 1;
    hex("sps orig", sps);
    if (!irplay::nal::patchSpsLowLatency(sps, patched)) { puts("patch fallita"); return 2; }
    hex("sps patch", patched);
    if (irplay::nal::patchSpsLowLatency(patched, again)) { puts("non idempotente"); return 3; }
    std::ofstream("sps_patched.bin", std::ios::binary).write((const char*) patched.data() + 4, patched.size() - 4);
    std::ofstream("pps.bin", std::ios::binary).write((const char*) pps.data() + 4, pps.size() - 4);
    // fuzz: nessun crash con SPS corrotti
    srand(1); int okc = 0;
    for (int i = 0; i < 200000; ++i) { Bytes m = sps; for (int k = 0; k < 3; ++k) m[4 + rand() % (m.size() - 4)] = rand(); m[4] = 0x67; if (rand() % 4 == 0) m.resize(5 + rand() % (m.size() - 5)); Bytes o; okc += irplay::nal::patchSpsLowLatency(m, o); }
    Bytes nal = {0x41, 0x9a, 0x00}; printf("P slice -> B? %d\n", irplay::nal::sliceIsB(nal.data(), nal.size()));
    Bytes bnal = {0x01, 0b10100000}; printf("B slice (type 1) -> B? %d\n", irplay::nal::sliceIsB(bnal.data(), bnal.size()));
    printf("fuzz ok (%d patch riuscite su 200000)\n", okc);
}
