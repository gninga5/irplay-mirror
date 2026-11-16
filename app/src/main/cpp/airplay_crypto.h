/*
 * airplay_crypto.h
 * Scopo: tutta la crittografia del protocollo AirPlay "legacy" (AirPlay 1 con pairing Ed25519),
 * l'unico che iOS usa verso un ricevitore che annuncia il bit 27 nelle feature flags.
 *
 * Catena delle chiavi (verificata sul codice di UxPlay/RPiPlay):
 *  1. pair-setup:  scambio delle chiavi pubbliche Ed25519 (nessun segreto).
 *  2. pair-verify: X25519 effimero -> segreto condiviso S (32 byte). Le firme Ed25519 su
 *     (pk_ecdh_a || pk_ecdh_b) viaggiano cifrate con AES-128-CTR, chiave e IV =
 *     SHA-512("Pair-Verify-AES-Key"|"Pair-Verify-AES-IV" || S)[0..15].
 *  3. fp-setup:    handshake FairPlay a 2 fasi; il messaggio da 164 byte della fase 2
 *     serve a playfair per decifrare "ekey".
 *  4. SETUP:       chiave di sessione K = SHA-512(FairPlay(ekey) || S)[0..15];
 *                  se il client non ha fatto pair-verify, K = FairPlay(ekey).
 *  5. Video:       AES-128-CTR, chiave/IV = SHA-512("AirPlayStreamKey<id>"|"AirPlayStreamIV<id>" || K)[0..15],
 *                  keystream continuo su tutti i pacchetti video.
 *  6. Audio:       AES-128-CBC con chiave K e IV = "eiv", IV ripartito a ogni pacchetto RTP;
 *                  i byte finali che non formano un blocco da 16 sono in chiaro.
 *
 * Implementazione in Rust (rust/src/pairing.rs, srp.rs, aes128.rs: RustCrypto e dalek, AES con
 * le istruzioni ARMv8 dove ci sono). Qui solo involucri: segreti, chiavi di ciclo e stati
 * restano in memoria di Rust dietro handle opachi, azzerati al rilascio; il segreto X25519
 * non esce mai (sessionKey lo usa al suo interno). FairPlay resta in C (playfair).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rust_core.h"

namespace irplay::srp {
    constexpr size_t kSessionKeySize = 40;   // 2 x SHA-1 (variante Apple)
    constexpr size_t kProofSize = 20;        // SHA-1
}

namespace irplay::crypto {

    /** Compatibilità: le primitive non richiedono inizializzazione. */
    inline bool init() { return true; }

    /** Azzera memoria in modo che il compilatore non possa eliminarlo. */
    inline void secureZero(void* p, size_t n) { irplay_secure_zero(static_cast<uint8_t*>(p), n); }

    /** Confronto a tempo costante di due buffer di n byte. */
    inline bool equalConstantTime(const void* a, const void* b, size_t n) {
        return irplay_ct_equal(static_cast<const uint8_t*>(a), static_cast<const uint8_t*>(b), n);
    }

    /** Intero uniforme in [0, n) dal generatore del kernel. */
    inline bool randomBelow(uint32_t n, uint32_t& out) { return irplay_random_below(n, &out); }

    class Ed25519Identity {
    public:
        /** Deriva la coppia di chiavi dal seed da 32 byte. Il seed non viene conservato. */
        static std::unique_ptr<Ed25519Identity> fromSeed(const uint8_t seed[32]);
        ~Ed25519Identity();
        Ed25519Identity(const Ed25519Identity&) = delete;
        Ed25519Identity& operator=(const Ed25519Identity&) = delete;

        const uint8_t* publicKey() const { return pk_.data(); }
        bool sign(uint8_t sig[64], const uint8_t* msg, size_t len) const;
        const IrplayIdentity* handle() const { return h_; }

    private:
        Ed25519Identity() = default;
        std::array<uint8_t, 32> pk_{};
        IrplayIdentity* h_ = nullptr;
    };

    /** Stato del pairing di una connessione RTSP. Non thread-safe: la usa solo il thread della connessione. */
    class PairingSession {
    public:
        PairingSession();
        ~PairingSession();
        PairingSession(const PairingSession&) = delete;
        PairingSession& operator=(const PairingSession&) = delete;

        /** POST /pair-setup: body = chiave pubblica Ed25519 del client (32 byte). */
        bool setup(const uint8_t* body, size_t len);
        /** POST /pair-verify, fase 1 (body[0] == 1). out = pk X25519 nostra (32) + firma cifrata (64). */
        bool verifyStep1(const Ed25519Identity& id, const uint8_t* body, size_t len, std::array<uint8_t, 96>& out);
        /** POST /pair-verify, fase 2 (body[0] == 0): verifica la firma del client. */
        bool verifyStep2(const uint8_t* body, size_t len);

        bool verified() const;
        /** Chiave Ed25519 del client ricevuta in pair-verify fase 1 (valida dopo verifyStep1). */
        const uint8_t* clientEdKey() const;

        // ---- pair-setup-pin (PIN mostrato sulla TV) ----
        // Fase 1: user = identificativo inviato da iOS, pin = 4 cifre. Restituisce salt e B.
        bool pinBegin(const std::string& user, const std::string& pin,
                      std::vector<uint8_t>& salt, std::vector<uint8_t>& publicB);
        // Fase 2: verifica la prova SRP del client; in uscita la prova della TV (20 byte).
        bool pinProof(const uint8_t* A, size_t lenA, const uint8_t* M1, size_t lenM1,
                      std::array<uint8_t, srp::kProofSize>& M2);
        // Fase 3: decifra (AES-GCM) la chiave Ed25519 del client e cifra la nostra.
        bool pinExchange(const Ed25519Identity& id, const uint8_t* epk, size_t lenEpk,
                         const uint8_t* tag, size_t lenTag,
                         std::array<uint8_t, 32>& outEpk, std::array<uint8_t, 16>& outTag);
        /** true dopo un pair-setup-pin completo su questa connessione. */
        bool pinAuthenticated() const;
        /** Chiave Ed25519 del client autenticata con il PIN (valida se pinAuthenticated()). */
        const uint8_t* pinClientKey() const;
        /** K = SHA-512(fpKey || S)[0..15] (punto 4 della catena); false senza pair-verify. */
        bool sessionKey(const uint8_t fpKey[16], uint8_t out[16]) const;

    private:
        IrplayPairing* h_ = nullptr;
        mutable std::array<uint8_t, 32> clientEdKey_{}, pinClientKey_{};
    };

    class FairPlay {
    public:
        ~FairPlay();
        /** POST /fp-setup: fase 1 (16 byte -> 142) o fase 2 (164 byte -> 32). */
        bool handle(const uint8_t* body, size_t len, std::vector<uint8_t>& out);
        /** Decifra la chiave AES (72 byte "ekey" -> 16 byte). Richiede la fase 2 completata. */
        bool decryptKey(const uint8_t* ekey, size_t len, uint8_t out[16]) const;

    private:
        std::array<uint8_t, 164> keyMsg_{};
        bool haveKeyMsg_ = false;
    };

    /** Chiave e IV del flusso video (punto 5). */
    void deriveMirrorKey(const uint8_t sessionKey[16], uint64_t streamConnectionId, uint8_t key[16], uint8_t iv[16]);

    /** AES-128-CTR con keystream continuo tra chiamate successive (come il flusso video di iOS). */
    class AesCtr {
    public:
        AesCtr(const uint8_t key[16], const uint8_t iv[16]) : h_(irplay_ctr_new(key, iv)) {}
        ~AesCtr() { irplay_ctr_free(h_); }
        AesCtr(const AesCtr&) = delete;
        AesCtr& operator=(const AesCtr&) = delete;
        void apply(uint8_t* data, size_t len) { if (len > 0) irplay_ctr_apply(h_, data, len); }
        /** Consuma len byte di keystream senza produrre output. */
        void skip(size_t len) { irplay_ctr_skip(h_, len); }

    private:
        IrplayCtr* h_;
    };

    /** Decifratura audio (punto 6): chiave fissa, IV ripartito a ogni pacchetto. */
    class AudioDecryptor {
    public:
        AudioDecryptor(const uint8_t key[16], const uint8_t iv[16]) : h_(irplay_cbc_new(key, iv)) {}
        ~AudioDecryptor() { irplay_cbc_free(h_); }
        AudioDecryptor(const AudioDecryptor&) = delete;
        AudioDecryptor& operator=(const AudioDecryptor&) = delete;
        /** Decifra in place i blocchi completi; la coda < 16 byte resta com'è (è in chiaro). */
        void decrypt(uint8_t* data, size_t len) { if (len >= 16) irplay_cbc_decrypt(h_, data, len); }

    private:
        IrplayCbc* h_;
    };

} // namespace irplay::crypto
