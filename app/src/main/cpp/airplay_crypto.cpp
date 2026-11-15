/*
 * airplay_crypto.cpp
 * Primitive: libsodium (Ed25519, X25519, SHA-512, azzeramento sicuro) e mbedTLS (AES).
 * Ogni buffer che contiene materiale segreto viene azzerato con sodium_memzero o
 * mbedtls_platform_zeroize, che il compilatore non può eliminare in ottimizzazione.
 */
#include "airplay_crypto.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <sodium.h>

extern "C" {
#include "playfair.h"
#include "fairplay_tables.h"
}

namespace irplay::crypto {
    namespace {

        constexpr char kSaltKey[] = "Pair-Verify-AES-Key";
        constexpr char kSaltIv[] = "Pair-Verify-AES-IV";

        // SHA-512(a || b), primi 16 byte.
        void sha512Prefix16(const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, uint8_t out[16]) {
            crypto_hash_sha512_state st;
            unsigned char h[crypto_hash_sha512_BYTES];
            crypto_hash_sha512_init(&st);
            crypto_hash_sha512_update(&st, a, alen);
            crypto_hash_sha512_update(&st, b, blen);
            crypto_hash_sha512_final(&st, h);
            std::memcpy(out, h, 16);
            sodium_memzero(h, sizeof h);
            sodium_memzero(&st, sizeof st);
        }

        // playfair usa tabelle globali e non è documentato come rientrante:
        // una sola decifratura alla volta (accade una volta per sessione, il costo è nullo).
        std::mutex gPlayfairMutex;

    } // namespace

    bool init() { return sodium_init() >= 0; }

    // ---------------------------------------------------------------- Ed25519Identity

    std::unique_ptr<Ed25519Identity> Ed25519Identity::fromSeed(const uint8_t seed[32]) {
        if (!init()) return nullptr;
        std::unique_ptr<Ed25519Identity> id(new Ed25519Identity());
        id->sk_ = static_cast<unsigned char*>(sodium_malloc(crypto_sign_SECRETKEYBYTES));
        if (id->sk_ == nullptr) return nullptr;
        if (crypto_sign_seed_keypair(id->pk_.data(), id->sk_, seed) != 0) return nullptr;
        sodium_mprotect_readonly(id->sk_);   // una scrittura accidentale termina il processo invece di corrompere la chiave
        return id;
    }

    Ed25519Identity::~Ed25519Identity() {
        if (sk_ != nullptr) sodium_free(sk_);   // sodium_free azzera prima di rilasciare
    }

    bool Ed25519Identity::sign(uint8_t sig[64], const uint8_t* msg, size_t len) const {
        return crypto_sign_detached(sig, nullptr, msg, len, sk_) == 0;
    }

    // ---------------------------------------------------------------- PairingSession

    PairingSession::~PairingSession() { wipe(); }

    void PairingSession::wipe() {
        sodium_memzero(ourEcdhSk_.data(), ourEcdhSk_.size());
        sodium_memzero(shared_.data(), shared_.size());
    }

    bool PairingSession::setup(const uint8_t* body, size_t len) {
        // Il body è la chiave Ed25519 del client: quella valida è la copia che arriva in
        // pair-verify, quindi qui basta controllarne la dimensione.
        if (body == nullptr || len != 32) return false;
        // Ricominciare il pairing invalida quello precedente sulla stessa connessione.
        wipe();
        state_ = State::SetupDone;
        return true;
    }

    void PairingSession::deriveKeyIv(uint8_t key[16], uint8_t iv[16]) const {
        sha512Prefix16(reinterpret_cast<const uint8_t*>(kSaltKey), sizeof kSaltKey - 1, shared_.data(), 32, key);
        sha512Prefix16(reinterpret_cast<const uint8_t*>(kSaltIv), sizeof kSaltIv - 1, shared_.data(), 32, iv);
    }

    bool PairingSession::verifyStep1(const Ed25519Identity& id, const uint8_t* body, size_t len,
                                     std::array<uint8_t, 96>& out) {
        // Si accetta anche senza pair-setup sulla stessa connessione: iOS che ha già associato
        // la TV salta pair-setup e parte da qui (osservato con iOS 27.2). pair-setup scambia solo
        // chiavi pubbliche; l'autenticazione vera è la verifica della firma nella fase 2.
        if (len != 4 + 32 + 32 || body[0] != 1) return false;
        std::memcpy(theirEcdhPk_.data(), body + 4, 32);
        std::memcpy(theirEdPk_.data(), body + 36, 32);

        randombytes_buf(ourEcdhSk_.data(), 32);
        crypto_scalarmult_base(ourEcdhPk_.data(), ourEcdhSk_.data());
        // crypto_scalarmult fallisce se il risultato è tutto zero: chiave del client di ordine
        // basso, che renderebbe il segreto condiviso prevedibile.
        if (crypto_scalarmult(shared_.data(), ourEcdhSk_.data(), theirEcdhPk_.data()) != 0) {
            wipe();
            state_ = State::Idle;
            return false;
        }
        sodium_memzero(ourEcdhSk_.data(), 32);   // serve solo per S: da qui in poi è inutile

        uint8_t msg[64], sig[64], key[16], iv[16];
        std::memcpy(msg, ourEcdhPk_.data(), 32);
        std::memcpy(msg + 32, theirEcdhPk_.data(), 32);
        if (!id.sign(sig, msg, sizeof msg)) return false;

        deriveKeyIv(key, iv);
        {
            AesCtr ctr(key, iv);
            ctr.apply(sig, sizeof sig);
        }
        sodium_memzero(key, sizeof key);
        sodium_memzero(iv, sizeof iv);

        std::memcpy(out.data(), ourEcdhPk_.data(), 32);
        std::memcpy(out.data() + 32, sig, 64);
        state_ = State::Step1Done;
        return true;
    }

    bool PairingSession::verifyStep2(const uint8_t* body, size_t len) {
        if (state_ != State::Step1Done || len != 4 + 64 || body[0] != 0) return false;

        uint8_t key[16], iv[16], sig[64], msg[64];
        deriveKeyIv(key, iv);
        {
            // Il client cifra la sua firma proseguendo lo stesso keystream usato per la nostra:
            // si scartano i primi 64 byte.
            AesCtr ctr(key, iv);
            ctr.skip(64);
            std::memcpy(sig, body + 4, 64);
            ctr.apply(sig, sizeof sig);
        }
        sodium_memzero(key, sizeof key);
        sodium_memzero(iv, sizeof iv);

        std::memcpy(msg, theirEcdhPk_.data(), 32);
        std::memcpy(msg + 32, ourEcdhPk_.data(), 32);
        if (crypto_sign_verify_detached(sig, msg, sizeof msg, theirEdPk_.data()) != 0) {
            wipe();
            state_ = State::Idle;
            return false;
        }
        state_ = State::Verified;
        return true;
    }

    // ---------------------------------------------------------------- pair-setup-pin

    bool PairingSession::pinBegin(const std::string& user, const std::string& pin,
                                  std::vector<uint8_t>& salt, std::vector<uint8_t>& publicB) {
        // Ogni nuova fase 1 annulla il tentativo precedente sulla stessa connessione.
        srp_ = std::make_unique<srp::Server>();
        pinProven_ = false;
        pinDone_ = false;
        return srp_->begin(user, pin, salt, publicB);
    }

    bool PairingSession::pinProof(const uint8_t* A, size_t lenA, const uint8_t* M1, size_t lenM1,
                                  std::array<uint8_t, srp::kProofSize>& M2) {
        if (!srp_) return false;
        if (!srp_->verify(A, lenA, M1, lenM1, M2)) {
            srp_.reset();   // PIN sbagliato: il tentativo è consumato
            return false;
        }
        pinProven_ = true;
        return true;
    }

    bool PairingSession::pinExchange(const Ed25519Identity& id, const uint8_t* epk, size_t lenEpk,
                                     const uint8_t* tag, size_t lenTag,
                                     std::array<uint8_t, 32>& outEpk, std::array<uint8_t, 16>& outTag) {
        if (!srp_ || !pinProven_ || lenEpk != 32 || lenTag != 16) return false;

        // Chiave e IV da K, come per pair-verify ma con sale "Pair-Setup-...". L'IV è da 16 byte
        // (GCM con IV non standard: mbedTLS lo gestisce come previsto dalla specifica) e il suo
        // ultimo byte viene incrementato prima di decifrare e di nuovo prima di cifrare la
        // risposta: è il "nonce" non documentato che iOS si aspetta (vedi UxPlay pairing.c).
        static constexpr char kSaltKey2[] = "Pair-Setup-AES-Key";
        static constexpr char kSaltIv2[] = "Pair-Setup-AES-IV";
        const auto& K = srp_->sessionKey();
        uint8_t key[16], iv[16], clientPk[32];
        sha512Prefix16(reinterpret_cast<const uint8_t*>(kSaltKey2), sizeof kSaltKey2 - 1, K.data(), K.size(), key);
        sha512Prefix16(reinterpret_cast<const uint8_t*>(kSaltIv2), sizeof kSaltIv2 - 1, K.data(), K.size(), iv);
        iv[15]++;

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
                  // auth_decrypt verifica il tag prima di restituire il testo in chiaro:
                  // una chiave manomessa non arriva mai al resto del codice.
                  mbedtls_gcm_auth_decrypt(&gcm, 32, iv, 16, nullptr, 0, tag, 16, epk, clientPk) == 0;
        if (ok) {
            iv[15]++;
            ok = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, iv, 16, nullptr, 0,
                                           id.publicKey(), outEpk.data(), 16, outTag.data()) == 0;
        }
        mbedtls_gcm_free(&gcm);
        mbedtls_platform_zeroize(key, sizeof key);
        mbedtls_platform_zeroize(iv, sizeof iv);
        srp_.reset();   // K non serve più
        pinProven_ = false;
        if (!ok) return false;

        std::memcpy(pinClientPk_.data(), clientPk, 32);
        pinDone_ = true;
        state_ = State::SetupDone;   // ora può seguire il normale pair-verify
        return true;
    }

    // ---------------------------------------------------------------- FairPlay

    FairPlay::~FairPlay() { sodium_memzero(keyMsg_.data(), keyMsg_.size()); }

    bool FairPlay::handle(const uint8_t* body, size_t len, std::vector<uint8_t>& out) {
        // Tutti i messaggi iniziano con "FPLY" e la versione 3: le altre versioni non sono
        // state ricostruite pubblicamente.
        if (len < 16 || std::memcmp(body, "FPLY", 4) != 0 || body[4] != 0x03) return false;

        if (len == 16) {
            const uint8_t mode = body[14];
            if (mode >= 4) return false;   // UxPlay non lo controlla: lettura fuori dalla tabella
            out.assign(fp_reply_message[mode], fp_reply_message[mode] + 142);
            haveKeyMsg_ = false;
            return true;
        }
        if (len == 164) {
            // playfair (omg_hax.c, decryptMessage) usa body[12] come indice di una tabella
            // [4][144] senza controllarlo: un valore >= 4 farebbe leggere memoria arbitraria.
            if (body[12] >= 4) return false;
            std::memcpy(keyMsg_.data(), body, 164);
            haveKeyMsg_ = true;
            out.assign(fp_header, fp_header + 12);
            out.insert(out.end(), body + 144, body + 164);
            return true;
        }
        return false;
    }

    bool FairPlay::decryptKey(const uint8_t* ekey, size_t len, uint8_t out[16]) const {
        if (!haveKeyMsg_ || len != 72) return false;
        // playfair dichiara parametri non const: lavora su copie locali.
        unsigned char msg[164], cipher[72];
        std::memcpy(msg, keyMsg_.data(), sizeof msg);
        std::memcpy(cipher, ekey, sizeof cipher);
        {
            std::lock_guard<std::mutex> lock(gPlayfairMutex);
            playfair_decrypt(msg, cipher, out);
        }
        sodium_memzero(msg, sizeof msg);
        return true;
    }

    // ---------------------------------------------------------------- derivazioni

    void deriveSessionKey(const uint8_t fpKey[16], const uint8_t shared[32], uint8_t out[16]) {
        sha512Prefix16(fpKey, 16, shared, 32, out);
    }

    void deriveMirrorKey(const uint8_t sessionKey[16], uint64_t streamConnectionId, uint8_t key[16], uint8_t iv[16]) {
        char sk[48], si[48];
        const int nk = std::snprintf(sk, sizeof sk, "AirPlayStreamKey%" PRIu64, streamConnectionId);
        const int ni = std::snprintf(si, sizeof si, "AirPlayStreamIV%" PRIu64, streamConnectionId);
        sha512Prefix16(reinterpret_cast<const uint8_t*>(sk), static_cast<size_t>(nk), sessionKey, 16, key);
        sha512Prefix16(reinterpret_cast<const uint8_t*>(si), static_cast<size_t>(ni), sessionKey, 16, iv);
    }

    // ---------------------------------------------------------------- AES

    AesCtr::AesCtr(const uint8_t key[16], const uint8_t iv[16]) {
        mbedtls_aes_init(&ctx_);
        mbedtls_aes_setkey_enc(&ctx_, key, 128);   // in CTR si usa sempre la cifratura del contatore
        std::memcpy(counter_, iv, 16);
        std::memset(block_, 0, sizeof block_);
    }

    AesCtr::~AesCtr() {
        mbedtls_aes_free(&ctx_);   // azzera anche la key schedule
        mbedtls_platform_zeroize(counter_, sizeof counter_);
        mbedtls_platform_zeroize(block_, sizeof block_);
    }

    void AesCtr::apply(uint8_t* data, size_t len) {
        if (len == 0) return;
        mbedtls_aes_crypt_ctr(&ctx_, len, &off_, counter_, block_, data, data);
    }

    void AesCtr::skip(size_t len) {
        uint8_t zeros[64] = {};
        while (len > 0) {
            const size_t n = len < sizeof zeros ? len : sizeof zeros;
            apply(zeros, n);
            len -= n;
        }
    }

    AudioDecryptor::AudioDecryptor(const uint8_t key[16], const uint8_t iv[16]) {
        mbedtls_aes_init(&ctx_);
        mbedtls_aes_setkey_dec(&ctx_, key, 128);
        std::memcpy(iv_, iv, 16);
    }

    AudioDecryptor::~AudioDecryptor() {
        mbedtls_aes_free(&ctx_);
        mbedtls_platform_zeroize(iv_, sizeof iv_);
    }

    void AudioDecryptor::decrypt(uint8_t* data, size_t len) {
        const size_t full = len & ~static_cast<size_t>(15);
        if (full == 0) return;
        unsigned char iv[16];
        std::memcpy(iv, iv_, 16);   // mbedTLS aggiorna l'IV: ogni pacchetto riparte da "eiv"
        mbedtls_aes_crypt_cbc(&ctx_, MBEDTLS_AES_DECRYPT, full, iv, data, data);
    }

} // namespace irplay::crypto
