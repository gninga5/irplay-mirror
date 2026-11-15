/*
 * srp_server.h
 * Scopo: lato TV del pairing con PIN di AirPlay ("pair-setup-pin"), cioè SRP-6a: l'iPhone
 * dimostra di conoscere il PIN mostrato sullo schermo senza mai trasmetterlo, e le due parti
 * ottengono una chiave condivisa K con cui scambiarsi le chiavi pubbliche Ed25519.
 *
 * Parametri usati da Apple (ricavati da UxPlay, che è interoperabile con iOS):
 *  - gruppo RFC 5054 a 2048 bit, g = 2, hash SHA-1;
 *  - k = H(N | PAD(g)), u = H(PAD(A) | PAD(B))  (convenzioni RFC 5054);
 *  - x = H(s | H(I ":" PIN)), con I = identificativo inviato dall'iPhone;
 *  - variante Apple per la chiave di sessione: K = H(S | 00000000) | H(S | 00000001), 40 byte,
 *    usata intera anche in M1 e M2.
 *
 * Sicurezza: tutti i numeri ricevuti sono controllati (A mod N != 0, lunghezze massime), il
 * confronto della prova del client è a tempo costante, i segreti (b, S, K) vengono azzerati.
 * Un'istanza serve per un solo tentativo: dopo verify() fallita va scartata.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <mbedtls/bignum.h>

namespace irplay::srp {

    constexpr size_t kSessionKeySize = 40;   // 2 x SHA-1 (variante Apple)
    constexpr size_t kProofSize = 20;        // SHA-1

    class Server {
    public:
        Server();
        ~Server();
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        /**
         * Fase 1. user: identificativo del client (max 64 byte), pin: le 4 cifre mostrate sulla TV.
         * Restituisce salt (16 byte) e B (chiave effimera pubblica) da mandare all'iPhone.
         */
        bool begin(const std::string& user, const std::string& pin,
                   std::vector<uint8_t>& salt, std::vector<uint8_t>& publicB);

        /**
         * Fase 2. Verifica la prova M1 del client (che conosce il PIN solo se è corretta).
         * In caso di successo restituisce M2, la prova della TV, e rende disponibile la chiave K.
         */
        bool verify(const uint8_t* A, size_t lenA, const uint8_t* M1, size_t lenM1,
                    std::array<uint8_t, kProofSize>& M2);

        /** Chiave di sessione K; valida solo dopo verify() riuscita. */
        const std::array<uint8_t, kSessionKeySize>& sessionKey() const { return K_; }

    private:
        mbedtls_mpi N_, g_, k_, v_, b_, B_;
        std::string user_;
        std::vector<uint8_t> salt_;
        std::array<uint8_t, kSessionKeySize> K_{};
        bool begun_ = false;
        bool verified_ = false;
    };

} // namespace irplay::srp
