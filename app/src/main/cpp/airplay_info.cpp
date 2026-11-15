/*
 * airplay_info.cpp
 * I valori fissi (formati audio, latenze, display) sono quelli usati da UxPlay/RPiPlay,
 * verificati con iOS 12-18. Alcuni campi numerici sono booleani "false" anziché 0:
 * è quello che inviano le implementazioni funzionanti, e iOS li accetta in questa forma.
 */
#include "airplay_info.h"

#include <algorithm>
#include <string_view>

#include "plist_writer.h"

namespace irplay {
    namespace {

        using plist::Array;
        using plist::Data;
        using plist::Dict;

        // Bitmask dei formati audio accettati in ingresso e in uscita (PCM, ALAC, AAC, AAC-ELD...).
        constexpr int kAudioFormats = 0x3FFFFFC;
        // Il display è annunciato a 1080p: iOS adatta la risoluzione dello stream a questi valori.
        // La risoluzione reale della TV verrà passata da Kotlin nella Fase 5.
        constexpr int kDisplayWidth = 1920;
        constexpr int kDisplayHeight = 1080;

        bool isPrintableAscii(std::string_view s, size_t minLen, size_t maxLen) {
            if (s.size() < minLen || s.size() > maxLen) return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
        }

        // Nome del ricevitore scelto dall'utente: UTF-8 valido (niente sequenze sovralunghe o
        // surrogati), 1-63 byte come un'etichetta DNS, nessun carattere di controllo né i
        // caratteri di direzione del testo (U+202A-202E, U+2066-2069), che permetterebbero di
        // mostrare su iOS un nome diverso da quello reale.
        bool isValidName(std::string_view s) {
            if (s.empty() || s.size() > 63) return false;
            size_t i = 0;
            while (i < s.size()) {
                const auto c = static_cast<unsigned char>(s[i]);
                size_t n;
                uint32_t cp;
                if (c < 0x80) { n = 1; cp = c; }
                else if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
                else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
                else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
                else return false;
                if (i + n > s.size()) return false;
                for (size_t k = 1; k < n; ++k) {
                    const auto cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) return false;
                    cp = (cp << 6) | (cc & 0x3F);
                }
                static constexpr uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
                if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
                if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
                if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)) return false;
                i += n;
            }
            return true;
        }

        bool isHex(char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        }

        // Formato fisso "XX:XX:XX:XX:XX:XX".
        bool isMac(std::string_view s) {
            if (s.size() != 17) return false;
            for (size_t i = 0; i < s.size(); ++i) {
                if (i % 3 == 2 ? s[i] != ':' : !isHex(s[i])) return false;
            }
            return true;
        }

        // Formato fisso 8-4-4-4-12.
        bool isUuid(std::string_view s) {
            if (s.size() != 36) return false;
            for (size_t i = 0; i < s.size(); ++i) {
                const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                if (dash ? s[i] != '-' : !isHex(s[i])) return false;
            }
            return true;
        }

        // Il record deve essere una sequenza completa di <lunghezza><stringa> senza voci vuote:
        // un record troncato verrebbe confrontato da iOS con quello mDNS e scartato.
        bool isTxtRecord(const std::vector<uint8_t>& txt) {
            if (txt.empty() || txt.size() > 1300) return false;   // un record mDNS deve stare in un pacchetto
            size_t pos = 0;
            while (pos < txt.size()) {
                const size_t len = txt[pos];
                if (len == 0 || pos + 1 + len > txt.size()) return false;
                pos += 1 + len;
            }
            return true;
        }

        bool isValid(const ReceiverInfo& i) {
            return isValidName(i.name)
                   && isMac(i.deviceId)
                   && isUuid(i.pairingId)
                   && i.publicKey.size() == 32
                   && i.features != 0
                   && isPrintableAscii(i.model, 1, 32)
                   && isPrintableAscii(i.sourceVersion, 1, 16)
                   && isTxtRecord(i.txtAirPlay)
                   && i.maxFps >= 15 && i.maxFps <= 60;
        }

        Dict audioFormat(int type) {
            Dict d;
            d.set("type", type)
                    .set("audioInputFormats", kAudioFormats)
                    .set("audioOutputFormats", kAudioFormats);
            return d;
        }

        Dict audioLatency(int type) {
            Dict d;
            d.set("type", type)
                    .set("audioType", "default")
                    .set("inputLatencyMicros", false)
                    .set("outputLatencyMicros", false);
            return d;
        }

        Dict display(const ReceiverInfo& i) {
            Dict d;
            d.set("uuid", i.pairingId)            // stabile tra i riavvii, come richiede iOS
                    .set("widthPhysical", false)
                    .set("heightPhysical", false)
                    .set("width", kDisplayWidth)
                    .set("height", kDisplayHeight)
                    .set("widthPixels", kDisplayWidth)
                    .set("heightPixels", kDisplayHeight)
                    .set("rotation", false)
                    .set("refreshRate", 1.0 / 60.0)  // periodo di refresh in secondi, non frequenza
                    // 60 fps dimezzano l'intervallo tra frame (16,7 ms invece di 33): meno latenza
                    // percepita. Kotlin sceglie 60 solo se il decoder regge 1080p60, perché il
                    // bitrate raddoppia e i decoder Amlogic economici non ce la fanno.
                    .set("maxFPS", i.maxFps)
                    .set("overscanned", false)
                    .set("features", 14);
            return d;
        }

    } // namespace

    std::optional<InfoPlists> buildInfoPlists(const ReceiverInfo& i) {
        if (!isValid(i)) return std::nullopt;

        Dict txt;
        txt.set("txtAirPlay", Data(i.txtAirPlay));

        Dict full;
        full.set("txtAirPlay", Data(i.txtAirPlay))
                .set("features", i.features)
                .set("name", i.name)
                .set("audioFormats", Array{audioFormat(100), audioFormat(101)})
                .set("pi", i.pairingId)
                .set("vv", 2)
                .set("statusFlags", 68)        // valore di UxPlay/RPiPlay: con altri valori iOS può chiedere un PIN
                .set("keepAliveLowPower", true)
                .set("sourceVersion", i.sourceVersion)
                .set("pk", Data(i.publicKey))
                .set("keepAliveSendStatsAsBody", true)
                .set("deviceID", i.deviceId)
                .set("audioLatencies", Array{audioLatency(100), audioLatency(101)})
                .set("model", i.model)
                .set("macAddress", i.deviceId)
                .set("displays", Array{display(i)});

        auto fullBin = plist::toBinary(full);
        auto txtBin = plist::toBinary(txt);
        if (!fullBin || !txtBin) return std::nullopt;
        return InfoPlists{std::move(*fullBin), std::move(*txtBin)};
    }

    bool requestsTxtAirPlay(const std::vector<uint8_t>& body) {
        constexpr std::string_view needle = "txtAirPlay";
        const std::string_view hay(reinterpret_cast<const char*>(body.data()), body.size());
        return hay.find(needle) != std::string_view::npos;
    }

} // namespace irplay
