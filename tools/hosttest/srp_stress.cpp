// srp_stress.cpp: molti abbinamenti con PIN, server Rust contro il client Python indipendente
// (srp_client.py): copre anche i numeri con zeri iniziali, che capitano ~1 volta su 100.
#include <cstdio>
#include <string>
#include <vector>
#include "rust_core.h"
static std::string hex(const uint8_t* p, size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
static std::vector<uint8_t> unhex(const std::string& s) { std::vector<uint8_t> v; for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t) std::stoi(s.substr(i, 2), nullptr, 16)); return v; }
int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    int ok = 0, fail = 0, b255 = 0, a255 = 0;
    for (int i = 0; i < n; ++i) {
        IrplayPairing* p = irplay_pairing_new();
        uint8_t salt[16], B[256]; size_t bl = 0;
        const std::string user = "iPhone-" + std::to_string(i), pin = "0" + std::to_string(100 + i % 900);
        irplay_pairing_pin_begin(p, (const uint8_t*) user.data(), user.size(), (const uint8_t*) pin.data(), pin.size(), salt, B, 256, &bl);
        b255 += bl < 256;
        std::string cmd = "python3 -I srp_client.py '" + user + "' " + pin + " " + hex(salt, 16) + " " + hex(B, bl);
        FILE* f = popen(cmd.c_str(), "r"); char buf[4096] = {}; fread(buf, 1, sizeof buf - 1, f); pclose(f);
        char A[1100] = {}, M1[100] = {}, M2[100] = {}, K[200] = {};
        sscanf(buf, "%1099s %99s %99s %199s", A, M1, M2, K);
        auto a = unhex(A), m1 = unhex(M1), m2e = unhex(M2);
        a255 += a.size() < 256;
        uint8_t m2[20];
        const bool good = irplay_pairing_pin_proof(p, a.data(), a.size(), m1.data(), m1.size(), m2) && m2e.size() == 20 && std::equal(m2, m2 + 20, m2e.begin());
        good ? ++ok : ++fail;
        if (!good && fail < 5) fprintf(stderr, "FALLITO: B %zu byte, A %zu byte\n", bl, a.size());
        irplay_pairing_free(p);
    }
    printf("abbinamenti PIN: %d riusciti, %d falliti (B con zero iniziale: %d, A con zero iniziale: %d)\n", ok, fail, b255, a255);
    return fail != 0;
}
