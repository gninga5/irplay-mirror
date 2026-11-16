/*
 * crypto_diff.cpp
 * Confronto della crittografia dell'app (Rust, tramite airplay_crypto.h) con libsodium e
 * mbedTLS, le librerie che l'app usava fino alla fase 5 e che qui fanno da riferimento
 * indipendente: AES-CTR a pezzi casuali (keystream continuo), AES-CBC dell'audio, chiavi
 * del video, chiavi pubbliche e firme Ed25519 (deterministiche: devono coincidere byte
 * per byte), pair-verify con un client libsodium, confronto a tempo costante.
 * Uso: crypto_diff [iterazioni, predefinito 20000]
 */
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <mbedtls/aes.h>
#include <sodium.h>

#include "airplay_crypto.h"

using Bytes = std::vector<uint8_t>;
using namespace irplay;
static std::mt19937_64 rng(5);
static long failures = 0;
#define CHECK(c, what) do { if (!(c)) { if (failures++ < 10) fprintf(stderr, "DIFFERENZA: %s (riga %d)\n", what, __LINE__); } } while (0)

static void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = (uint8_t) rng(); }

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    const long iters = argc > 1 ? std::atol(argv[1]) : 20000;
    for (long it = 0; it < iters; ++it) {
        uint8_t key[16], iv[16];
        fill(key, 16); fill(iv, 16);
        if (it % 7 == 0) std::memset(iv + 8, 0xFF, 8);   // riporto del contatore oltre i 64 bit
        // AES-CTR: stesso keystream di mbedtls_aes_crypt_ctr, anche a pezzi e con skip
        Bytes data(rng() % 4000); fill(data.data(), data.size());
        Bytes a = data, b = data;
        {
            crypto::AesCtr ctr(key, iv);
            mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, key, 128);
            uint8_t nonce[16], block[16] = {}; size_t off = 0; std::memcpy(nonce, iv, 16);
            size_t pos = 0;
            while (pos < data.size()) {
                const size_t n = std::min<size_t>(rng() % 300, data.size() - pos);
                if (rng() % 5 == 0) {
                    ctr.skip(n);
                    Bytes z(n); if (n) mbedtls_aes_crypt_ctr(&c, n, &off, nonce, block, z.data(), z.data());
                    std::memset(a.data() + pos, 0, n); std::memset(b.data() + pos, 0, n);
                } else {
                    ctr.apply(a.data() + pos, n);
                    if (n) mbedtls_aes_crypt_ctr(&c, n, &off, nonce, block, b.data() + pos, b.data() + pos);
                }
                pos += n;
            }
            mbedtls_aes_free(&c);
        }
        CHECK(a == b, "AES-CTR");
        // AES-CBC dell'audio: IV da capo a ogni pacchetto, coda in chiaro
        a = data; b = data;
        {
            crypto::AudioDecryptor dec(key, iv);
            dec.decrypt(a.data(), a.size());
            mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_dec(&c, key, 128);
            uint8_t v[16]; std::memcpy(v, iv, 16);
            const size_t full = b.size() & ~size_t{15};
            if (full) mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_DECRYPT, full, v, b.data(), b.data());
            mbedtls_aes_free(&c);
        }
        CHECK(a == b, "AES-CBC");
        // chiavi del video: SHA-512("AirPlayStreamKey<id>" || K)[0..16]
        {
            const uint64_t id = it % 3 == 0 ? rng() : rng() % 1000;
            uint8_t k1[16], i1[16];
            crypto::deriveMirrorKey(key, id, k1, i1);
            char s[48]; uint8_t h[64];
            for (int w = 0; w < 2; ++w) {
                const int n = std::snprintf(s, sizeof s, w ? "AirPlayStreamIV%" PRIu64 : "AirPlayStreamKey%" PRIu64, id);
                crypto_hash_sha512_state st; crypto_hash_sha512_init(&st);
                crypto_hash_sha512_update(&st, (const uint8_t*) s, (size_t) n); crypto_hash_sha512_update(&st, key, 16);
                crypto_hash_sha512_final(&st, h);
                CHECK(std::memcmp(h, w ? i1 : k1, 16) == 0, "chiavi video");
            }
        }
        // Ed25519: stessa chiave pubblica e stessa firma di libsodium
        if (it % 10 == 0) {
            uint8_t seed[32], pk[32], sk[64], sig[64], mine[64];
            fill(seed, 32);
            crypto_sign_seed_keypair(pk, sk, seed);
            auto id = crypto::Ed25519Identity::fromSeed(seed);
            CHECK(id && std::memcmp(id->publicKey(), pk, 32) == 0, "Ed25519 chiave pubblica");
            Bytes msg(rng() % 200); fill(msg.data(), msg.size());
            crypto_sign_detached(sig, nullptr, msg.data(), msg.size(), sk);
            CHECK(id && id->sign(mine, msg.data(), msg.size()) && std::memcmp(sig, mine, 64) == 0, "Ed25519 firma");
        }
        // confronto a tempo costante
        {
            Bytes x(rng() % 64), y; fill(x.data(), x.size()); y = x;
            CHECK(crypto::equalConstantTime(x.data(), y.data(), x.size()), "uguaglianza");
            if (!y.empty()) { y[rng() % y.size()] ^= (uint8_t) (1 + rng() % 255); CHECK(!crypto::equalConstantTime(x.data(), y.data(), x.size()), "diversità"); }
        }
    }
    // pair-verify con un client libsodium (come iOS), poi chiave di sessione
    for (int it = 0; it < 200; ++it) {
        uint8_t seed[32]; fill(seed, 32);
        auto tv = crypto::Ed25519Identity::fromSeed(seed);
        uint8_t epk[32], esk[64], xsk[32], xpk[32], shared[32];
        crypto_sign_keypair(epk, esk); randombytes_buf(xsk, 32); crypto_scalarmult_base(xpk, xsk);
        crypto::PairingSession p;
        CHECK(p.setup(epk, 32), "pair-setup");
        Bytes body = {1, 0, 0, 0}; body.insert(body.end(), xpk, xpk + 32); body.insert(body.end(), epk, epk + 32);
        std::array<uint8_t, 96> out{};
        CHECK(p.verifyStep1(*tv, body.data(), body.size(), out), "pair-verify 1");
        CHECK(crypto_scalarmult(shared, xsk, out.data()) == 0, "X25519");
        uint8_t h[64], k[16], v[16];
        crypto_hash_sha512_state st;
        crypto_hash_sha512_init(&st); crypto_hash_sha512_update(&st, (const uint8_t*) "Pair-Verify-AES-Key", 19); crypto_hash_sha512_update(&st, shared, 32); crypto_hash_sha512_final(&st, h); std::memcpy(k, h, 16);
        crypto_hash_sha512_init(&st); crypto_hash_sha512_update(&st, (const uint8_t*) "Pair-Verify-AES-IV", 18); crypto_hash_sha512_update(&st, shared, 32); crypto_hash_sha512_final(&st, h); std::memcpy(v, h, 16);
        mbedtls_aes_context c; mbedtls_aes_init(&c); mbedtls_aes_setkey_enc(&c, k, 128);
        uint8_t nonce[16], block[16] = {}; size_t off = 0; std::memcpy(nonce, v, 16);
        uint8_t ssig[64]; std::memcpy(ssig, out.data() + 32, 64);
        mbedtls_aes_crypt_ctr(&c, 64, &off, nonce, block, ssig, ssig);
        uint8_t m[64]; std::memcpy(m, out.data(), 32); std::memcpy(m + 32, xpk, 32);
        CHECK(crypto_sign_verify_detached(ssig, m, 64, tv->publicKey()) == 0, "firma della TV");
        std::memcpy(m, xpk, 32); std::memcpy(m + 32, out.data(), 32);
        uint8_t my[64]; crypto_sign_detached(my, nullptr, m, 64, esk);
        const bool tamper = it % 4 == 3;
        if (tamper) my[rng() % 64] ^= 0x10;
        mbedtls_aes_crypt_ctr(&c, 64, &off, nonce, block, my, my);
        mbedtls_aes_free(&c);
        Bytes b2 = {0, 0, 0, 0}; b2.insert(b2.end(), my, my + 64);
        CHECK(p.verifyStep2(b2.data(), b2.size()) == !tamper, "pair-verify 2");
        CHECK(std::memcmp(p.clientEdKey(), epk, 32) == 0, "chiave del client");
        uint8_t fp[16], sk1[16]; fill(fp, 16);
        const bool have = p.sessionKey(fp, sk1);
        CHECK(have == !tamper, "chiave di sessione disponibile");
        if (have) {
            crypto_hash_sha512_init(&st); crypto_hash_sha512_update(&st, fp, 16); crypto_hash_sha512_update(&st, shared, 32); crypto_hash_sha512_final(&st, h);
            CHECK(std::memcmp(h, sk1, 16) == 0, "chiave di sessione");
        }
    }
    printf("crittografia: %ld casi AES/chiavi/firme e 200 pair-verify, AES hardware ARMv8: %s, differenze %ld\n",
           iters, irplay_aes_arm_hardware() ? "sì" : "no", failures);
    return failures ? 1 : 0;
}
