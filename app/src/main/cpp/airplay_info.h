/*
 * airplay_info.h
 * Scopo: descrizione statica del ricevitore e costruzione della risposta a GET /info.
 * Protocollo: iOS chiama GET /info (RTSP/1.0) subito dopo aver trovato il servizio via
 * mDNS. Il corpo della risposta è un plist binario (application/x-apple-binary-plist)
 * con identità, feature flags, formati audio e caratteristiche del display.
 * Se il corpo della richiesta contiene il qualifier "txtAirPlay", iOS vuole solo il
 * record TXT di _airplay._tcp: serve a confrontarlo con quello visto via mDNS.
 *
 * I due plist vengono costruiti una volta sola in start(): i dati non cambiano
 * durante la vita del server, così le risposte non allocano niente per ogni richiesta
 * e un ReceiverInfo non valido blocca l'avvio invece di produrre risposte sbagliate.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace irplay {

    struct ReceiverInfo {
        std::string name;                 // nome mostrato su iOS: UTF-8 valido, 1-63 byte (limite mDNS)
        std::string deviceId;             // MAC virtuale "AA:BB:CC:DD:EE:FF"
        std::string pairingId;            // UUID "pi", 36 caratteri
        std::vector<uint8_t> publicKey;   // Ed25519, 32 byte
        uint64_t features = 0;
        std::string model;
        std::string sourceVersion;
        std::vector<uint8_t> txtAirPlay;  // TXT di _airplay._tcp già codificato (RFC 6763 §6.1)
        int maxFps = 30;                  // frame al secondo chiesti a iOS (30 o 60, secondo il decoder)
        bool hevc = false;                // HEVC per la duplicazione (bit 42 in features, deciso da Kotlin)
        int displayWidth = 1920;          // schermo annunciato: iOS adatta la risoluzione a questi valori
        int displayHeight = 1080;
    };

    struct InfoPlists {
        std::vector<uint8_t> full;
        std::vector<uint8_t> txtOnly;
    };

    /** Valida info e serializza i due plist. nullopt se un campo non rispetta il formato atteso. */
    std::optional<InfoPlists> buildInfoPlists(const ReceiverInfo& info);

    /**
     * Provvisorio (fino alla Fase 2E): cerca la stringa "txtAirPlay" nei byte del corpo.
     * In bplist00 le stringhe ASCII sono salvate in chiaro, quindi la ricerca funziona.
     * Un falso positivo restituisce solo un plist più corto, senza effetti sulla sicurezza.
     */
    bool requestsTxtAirPlay(const std::vector<uint8_t>& body);

} // namespace irplay
