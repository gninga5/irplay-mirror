/*
 * airplay_crypto.cpp
 * Involucri delle funzioni Rust (vedi airplay_crypto.h) e FairPlay (playfair, in C).
 */
#include "airplay_crypto.h"

#include <cstring>
#include <mutex>

extern "C" {
#include "playfair.h"
#include "fairplay_tables.h"
}

namespace irplay::crypto {
    namespace {
        // playfair usa tabelle globali e non è documentato come rientrante:
        // una sola decifratura alla volta (accade una volta per sessione, il costo è nullo).
        std::mutex gPlayfairMutex;
    } // namespace

    // ---------------------------------------------------------------- Ed25519Identity

    std::unique_ptr<Ed25519Identity> Ed25519Identity::fromSeed(const uint8_t seed[32]) {
        std::unique_ptr<Ed25519Identity> id(new Ed25519Identity());
        id->h_ = irplay_identity_new(seed);
        if (id->h_ == nullptr || !irplay_identity_public(id->h_, id->pk_.data())) return nullptr;
        return id;
    }

    Ed25519Identity::~Ed25519Identity() { irplay_identity_free(h_); }   // azzera il segreto

    bool Ed25519Identity::sign(uint8_t sig[64], const uint8_t* msg, size_t len) const {
        return irplay_identity_sign(h_, msg, len, sig);
    }

    // ---------------------------------------------------------------- PairingSession

    PairingSession::PairingSession() : h_(irplay_pairing_new()) {}
    PairingSession::~PairingSession() { irplay_pairing_free(h_); }   // azzera i segreti

    bool PairingSession::setup(const uint8_t* body, size_t len) {
        return body != nullptr && irplay_pairing_setup(h_, body, len);
    }

    bool PairingSession::verifyStep1(const Ed25519Identity& id, const uint8_t* body, size_t len,
                                     std::array<uint8_t, 96>& out) {
        return body != nullptr && irplay_pairing_verify1(h_, id.handle(), body, len, out.data());
    }

    bool PairingSession::verifyStep2(const uint8_t* body, size_t len) {
        return body != nullptr && irplay_pairing_verify2(h_, body, len);
    }

    bool PairingSession::verified() const { return irplay_pairing_verified(h_); }

    const uint8_t* PairingSession::clientEdKey() const {
        if (!irplay_pairing_client_ed_key(h_, clientEdKey_.data())) clientEdKey_.fill(0);
        return clientEdKey_.data();
    }

    bool PairingSession::pinBegin(const std::string& user, const std::string& pin,
                                  std::vector<uint8_t>& salt, std::vector<uint8_t>& publicB) {
        salt.assign(16, 0);
        publicB.assign(256, 0);
        size_t bLen = 0;
        const bool ok = irplay_pairing_pin_begin(h_, reinterpret_cast<const uint8_t*>(user.data()), user.size(),
                                                 reinterpret_cast<const uint8_t*>(pin.data()), pin.size(),
                                                 salt.data(), publicB.data(), publicB.size(), &bLen);
        if (!ok || bLen > publicB.size()) {
            salt.clear();
            publicB.clear();
            return false;
        }
        publicB.resize(bLen);
        return true;
    }

    bool PairingSession::pinProof(const uint8_t* A, size_t lenA, const uint8_t* M1, size_t lenM1,
                                  std::array<uint8_t, srp::kProofSize>& M2) {
        return irplay_pairing_pin_proof(h_, A, lenA, M1, lenM1, M2.data());
    }

    bool PairingSession::pinExchange(const Ed25519Identity& id, const uint8_t* epk, size_t lenEpk,
                                     const uint8_t* tag, size_t lenTag,
                                     std::array<uint8_t, 32>& outEpk, std::array<uint8_t, 16>& outTag) {
        return irplay_pairing_pin_exchange(h_, id.handle(), epk, lenEpk, tag, lenTag, outEpk.data(), outTag.data());
    }

    bool PairingSession::pinAuthenticated() const { return irplay_pairing_pin_authenticated(h_); }

    const uint8_t* PairingSession::pinClientKey() const {
        if (!irplay_pairing_pin_client_key(h_, pinClientKey_.data())) pinClientKey_.fill(0);
        return pinClientKey_.data();
    }

    bool PairingSession::sessionKey(const uint8_t fpKey[16], uint8_t out[16]) const {
        return irplay_pairing_session_key(h_, fpKey, out);
    }

    // ---------------------------------------------------------------- FairPlay

    FairPlay::~FairPlay() { secureZero(keyMsg_.data(), keyMsg_.size()); }

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
        secureZero(msg, sizeof msg);
        return true;
    }

    // ---------------------------------------------------------------- derivazioni

    void deriveMirrorKey(const uint8_t sessionKey[16], uint64_t streamConnectionId, uint8_t key[16], uint8_t iv[16]) {
        if (!irplay_derive_mirror_key(sessionKey, streamConnectionId, key, iv)) {
            // Non succede con puntatori validi; chiavi a zero farebbero solo fallire la decifratura.
            std::memset(key, 0, 16);
            std::memset(iv, 0, 16);
        }
    }

} // namespace irplay::crypto
