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
 * Il segreto Ed25519 sta in memoria allocata con sodium_malloc (pagine di guardia,
 * esclusa dai core dump, azzerata al rilascio) e in sola lettura dopo la creazione.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <mbedtls/aes.h>

namespace irplay::crypto {

    /** Inizializza libsodium. Va chiamata prima di tutto il resto; idempotente. */
    bool init();

    class Ed25519Identity {
    public:
        /** Deriva la coppia di chiavi dal seed da 32 byte. Il seed non viene conservato. */
        static std::unique_ptr<Ed25519Identity> fromSeed(const uint8_t seed[32]);
        ~Ed25519Identity();
        Ed25519Identity(const Ed25519Identity&) = delete;
        Ed25519Identity& operator=(const Ed25519Identity&) = delete;

        const uint8_t* publicKey() const { return pk_.data(); }
        bool sign(uint8_t sig[64], const uint8_t* msg, size_t len) const;

    private:
        Ed25519Identity() = default;
        std::array<uint8_t, 32> pk_{};
        unsigned char* sk_ = nullptr;   // 64 byte, sodium_malloc
    };

    /** Stato del pairing di una connessione RTSP. Non thread-safe: la usa solo il thread della connessione. */
    class PairingSession {
    public:
        ~PairingSession();

        /** POST /pair-setup: body = chiave pubblica Ed25519 del client (32 byte). */
        bool setup(const uint8_t* body, size_t len);
        /** POST /pair-verify, fase 1 (body[0] == 1). out = pk X25519 nostra (32) + firma cifrata (64). */
        bool verifyStep1(const Ed25519Identity& id, const uint8_t* body, size_t len, std::array<uint8_t, 96>& out);
        /** POST /pair-verify, fase 2 (body[0] == 0): verifica la firma del client. */
        bool verifyStep2(const uint8_t* body, size_t len);

        bool verified() const { return state_ == State::Verified; }
        /** Segreto X25519 condiviso; valido solo se verified(). */
        const uint8_t* sharedSecret() const { return shared_.data(); }

    private:
        enum class State { Idle, SetupDone, Step1Done, Verified };
        void deriveKeyIv(uint8_t key[16], uint8_t iv[16]) const;
        void wipe();

        State state_ = State::Idle;
        std::array<uint8_t, 32> ourEcdhPk_{}, ourEcdhSk_{}, theirEcdhPk_{}, theirEdPk_{}, shared_{};
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

    /** K = SHA-512(fpKey || shared)[0..15] (punto 4 della catena). */
    void deriveSessionKey(const uint8_t fpKey[16], const uint8_t shared[32], uint8_t out[16]);
    /** Chiave e IV del flusso video (punto 5). */
    void deriveMirrorKey(const uint8_t sessionKey[16], uint64_t streamConnectionId, uint8_t key[16], uint8_t iv[16]);

    /** AES-128-CTR con keystream continuo tra chiamate successive (come il flusso video di iOS). */
    class AesCtr {
    public:
        AesCtr(const uint8_t key[16], const uint8_t iv[16]);
        ~AesCtr();
        AesCtr(const AesCtr&) = delete;
        AesCtr& operator=(const AesCtr&) = delete;
        void apply(uint8_t* data, size_t len);
        /** Consuma len byte di keystream senza produrre output. */
        void skip(size_t len);

    private:
        mbedtls_aes_context ctx_;
        unsigned char counter_[16];
        unsigned char block_[16];
        size_t off_ = 0;
    };

    /** Decifratura audio (punto 6): chiave fissa, IV ripartito a ogni pacchetto. */
    class AudioDecryptor {
    public:
        AudioDecryptor(const uint8_t key[16], const uint8_t iv[16]);
        ~AudioDecryptor();
        AudioDecryptor(const AudioDecryptor&) = delete;
        AudioDecryptor& operator=(const AudioDecryptor&) = delete;
        /** Decifra in place i blocchi completi; la coda < 16 byte resta com'è (è in chiaro). */
        void decrypt(uint8_t* data, size_t len);

    private:
        mbedtls_aes_context ctx_;
        unsigned char iv_[16];
    };

} // namespace irplay::crypto
