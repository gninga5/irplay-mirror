/*
 * srp_server.cpp
 * Aritmetica a precisione multipla con mbedtls_mpi (già nel progetto per AES); casualità da
 * libsodium. Ogni passo segue l'ordine e la codifica dei byte di UxPlay srp.c (APPLE_VARIANT,
 * rfc5054_compat = 1): una differenza anche di un solo zero iniziale renderebbe la prova
 * dell'iPhone non verificabile.
 *
 * Codifica dei numeri negli hash:
 *  - "bytes(n)": big-endian senza zeri iniziali (BN_bn2bin in OpenSSL);
 *  - "PAD(n)":   big-endian portato alla lunghezza di N (256 byte), solo per k e u.
 */
#include "srp_server.h"

#include <cstring>

#include <mbedtls/constant_time.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha1.h>
#include <sodium.h>

namespace irplay::srp {
    namespace {

        // RFC 5054, appendice A, gruppo a 2048 bit.
        constexpr char kNHex[] =
                "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
                "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
                "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
                "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
                "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
                "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
                "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";
        constexpr size_t kNBytes = 256;
        constexpr size_t kMaxUser = 64;

        // Contesto SHA-1 con distruzione garantita (il contenuto può derivare da segreti).
        class Sha1 {
        public:
            Sha1() { mbedtls_sha1_init(&c_); mbedtls_sha1_starts(&c_); }
            ~Sha1() { mbedtls_sha1_free(&c_); }
            void add(const void* p, size_t n) { mbedtls_sha1_update(&c_, static_cast<const unsigned char*>(p), n); }
            void add(const std::vector<uint8_t>& v) { add(v.data(), v.size()); }
            void finish(uint8_t out[20]) { mbedtls_sha1_finish(&c_, out); }
        private:
            mbedtls_sha1_context c_;
        };

        std::vector<uint8_t> bytes(const mbedtls_mpi& n) {
            std::vector<uint8_t> out(mbedtls_mpi_size(&n));
            if (!out.empty()) mbedtls_mpi_write_binary(&n, out.data(), out.size());
            return out;
        }

        std::vector<uint8_t> padded(const mbedtls_mpi& n) {
            std::vector<uint8_t> out(kNBytes);
            mbedtls_mpi_write_binary(&n, out.data(), out.size());   // fallisce solo se n >= 2^2048
            return out;
        }

        // Interpreta un hash SHA-1 come intero (H(...) usato come numero in k, u, x).
        int hashToMpi(mbedtls_mpi& out, const uint8_t h[20]) { return mbedtls_mpi_read_binary(&out, h, 20); }

        // RAII per i temporanei: mbedtls_mpi_free azzera la memoria prima di liberarla.
        struct Mpi {
            mbedtls_mpi v;
            Mpi() { mbedtls_mpi_init(&v); }
            ~Mpi() { mbedtls_mpi_free(&v); }
            mbedtls_mpi* operator&() { return &v; }
        };

        // Funzione di casualità per mbedtls (libsodium usa il generatore del kernel).
        int randomBytes(void*, unsigned char* out, size_t n) {
            randombytes_buf(out, n);
            return 0;
        }

    } // namespace

    Server::Server() {
        for (mbedtls_mpi* m : {&N_, &g_, &k_, &v_, &b_, &B_}) mbedtls_mpi_init(m);
        mbedtls_mpi_read_string(&N_, 16, kNHex);
        mbedtls_mpi_lset(&g_, 2);
        // k = H(N | PAD(g))
        uint8_t h[20];
        Sha1 sha;
        sha.add(padded(N_));
        sha.add(padded(g_));
        sha.finish(h);
        hashToMpi(k_, h);
    }

    Server::~Server() {
        for (mbedtls_mpi* m : {&N_, &g_, &k_, &v_, &b_, &B_}) mbedtls_mpi_free(m);
        mbedtls_platform_zeroize(K_.data(), K_.size());
    }

    bool Server::begin(const std::string& user, const std::string& pin,
                       std::vector<uint8_t>& salt, std::vector<uint8_t>& publicB) {
        if (begun_ || user.empty() || user.size() > kMaxUser || pin.size() != 4) return false;
        user_ = user;

        // Salt di 16 byte con il primo diverso da zero: così "bytes(s)" ha sempre 16 byte,
        // come si aspetta UxPlay (che rifiuta altrimenti) e quindi anche iOS.
        salt_.resize(16);
        do { randombytes_buf(salt_.data(), salt_.size()); } while (salt_[0] == 0);

        // x = H(s | H(I ":" P))
        uint8_t inner[20], h[20];
        {
            Sha1 sha;
            sha.add(user_.data(), user_.size());
            sha.add(":", 1);
            sha.add(pin.data(), pin.size());
            sha.finish(inner);
        }
        {
            Sha1 sha;
            sha.add(salt_);
            sha.add(inner, sizeof inner);
            sha.finish(h);
        }
        Mpi x, t1, t2;
        int rc = hashToMpi(x.v, h);
        mbedtls_platform_zeroize(inner, sizeof inner);
        mbedtls_platform_zeroize(h, sizeof h);

        // v = g^x mod N ;  b casuale a 256 bit ;  B = (k*v + g^b) mod N
        rc = rc ? rc : mbedtls_mpi_exp_mod(&v_, &g_, &x, &N_, nullptr);
        rc = rc ? rc : mbedtls_mpi_fill_random(&b_, 32, randomBytes, nullptr);
        rc = rc ? rc : mbedtls_mpi_mul_mpi(&t1, &k_, &v_);
        rc = rc ? rc : mbedtls_mpi_exp_mod(&t2, &g_, &b_, &N_, nullptr);
        rc = rc ? rc : mbedtls_mpi_add_mpi(&t1, &t1, &t2);
        rc = rc ? rc : mbedtls_mpi_mod_mpi(&B_, &t1, &N_);
        if (rc != 0 || mbedtls_mpi_cmp_int(&B_, 0) == 0) return false;

        salt = salt_;
        publicB = bytes(B_);
        begun_ = true;
        return true;
    }

    bool Server::verify(const uint8_t* A, size_t lenA, const uint8_t* M1, size_t lenM1,
                        std::array<uint8_t, kProofSize>& M2) {
        // Una sola verifica per istanza: impedisce di riusare lo stesso b per più tentativi.
        if (!begun_ || verified_ || A == nullptr || M1 == nullptr) return false;
        begun_ = false;
        if (lenA == 0 || lenA > kNBytes || lenM1 != kProofSize) return false;

        Mpi a, u, t1, t2, S;
        int rc = mbedtls_mpi_read_binary(&a, A, lenA);
        // Controllo di sicurezza SRP-6a: con A ≡ 0 (mod N) il segreto S sarebbe 0 e un client
        // potrebbe autenticarsi senza conoscere il PIN.
        rc = rc ? rc : mbedtls_mpi_mod_mpi(&t1, &a, &N_);
        if (rc != 0 || mbedtls_mpi_cmp_int(&t1, 0) == 0) return false;

        // u = H(PAD(A) | PAD(B)), deve essere diverso da 0
        uint8_t h[20];
        {
            Sha1 sha;
            sha.add(padded(a.v));
            sha.add(padded(B_));
            sha.finish(h);
        }
        rc = hashToMpi(u.v, h);
        if (rc != 0 || mbedtls_mpi_cmp_int(&u, 0) == 0) return false;

        // S = (A * v^u)^b mod N
        rc = mbedtls_mpi_exp_mod(&t1, &v_, &u, &N_, nullptr);
        rc = rc ? rc : mbedtls_mpi_mul_mpi(&t2, &a, &t1);
        rc = rc ? rc : mbedtls_mpi_mod_mpi(&t2, &t2, &N_);
        rc = rc ? rc : mbedtls_mpi_exp_mod(&S, &t2, &b_, &N_, nullptr);
        if (rc != 0) return false;

        // K = H(S | 00000000) | H(S | 00000001)  (variante Apple, 40 byte)
        std::vector<uint8_t> sBytes = bytes(S.v);
        for (uint8_t i = 0; i < 2; ++i) {
            const uint8_t counter[4] = {0, 0, 0, i};
            Sha1 sha;
            sha.add(sBytes);
            sha.add(counter, sizeof counter);
            sha.finish(K_.data() + 20 * i);
        }
        mbedtls_platform_zeroize(sBytes.data(), sBytes.size());

        // M = H( (H(N) xor H(g)) | H(I) | s | A | B | K )
        uint8_t hN[20], hg[20], hI[20], expected[20];
        { Sha1 sha; sha.add(bytes(N_)); sha.finish(hN); }
        { Sha1 sha; sha.add(bytes(g_)); sha.finish(hg); }
        { Sha1 sha; sha.add(user_.data(), user_.size()); sha.finish(hI); }
        for (int i = 0; i < 20; ++i) hN[i] ^= hg[i];
        const std::vector<uint8_t> aBytes = bytes(a.v), bBytes = bytes(B_);
        {
            Sha1 sha;
            sha.add(hN, sizeof hN);
            sha.add(hI, sizeof hI);
            sha.add(salt_);
            sha.add(aBytes);
            sha.add(bBytes);
            sha.add(K_.data(), K_.size());
            sha.finish(expected);
        }
        // Confronto a tempo costante: un confronto che si ferma al primo byte diverso
        // rivelerebbe, misurando i tempi di risposta, quanti byte della prova sono giusti.
        if (mbedtls_ct_memcmp(expected, M1, kProofSize) != 0) {
            mbedtls_platform_zeroize(K_.data(), K_.size());
            return false;
        }

        // M2 = H(A | M | K)
        {
            Sha1 sha;
            sha.add(aBytes);
            sha.add(expected, sizeof expected);
            sha.add(K_.data(), K_.size());
            sha.finish(M2.data());
        }
        verified_ = true;
        return true;
    }

} // namespace irplay::srp
