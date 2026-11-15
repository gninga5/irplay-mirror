/*
 * nal_parser.h
 * Scopo: adatta l'H.264 del mirroring iOS al formato che si aspettano i decoder hardware.
 * Standard: ITU-T H.264 Annex B (start code 00 00 00 01) e ISO/IEC 14496-15
 * (formato AVCC con lunghezze di 4 byte, record avcC con SPS/PPS).
 *
 * iOS invia ogni frame come sequenza di NAL in formato AVCC. I decoder MediaTek,
 * Realtek e Amlogic accettano in MediaCodec solo Annex-B: alcuni decodificano AVCC
 * "per caso", altri producono macroblocchi o si bloccano senza errori.
 * La conversione avviene in place (lunghezza e start code sono entrambi di 4 byte),
 * quindi non ci sono copie per frame.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace irplay::nal {

    /**
     * Converte in place AVCC -> Annex-B. Restituisce false se le lunghezze non sono coerenti
     * con la dimensione del buffer (frame corrotto o chiave sbagliata): in quel caso il buffer
     * va scartato. keyframe = true se è presente un'unità IDR (tipo 5).
     * bSlice (facoltativo): diventa true se il frame contiene uno slice B (vedi sps_rewriter.h).
     */
    bool avccToAnnexB(uint8_t* data, size_t len, bool& keyframe, bool* bSlice = nullptr);

    /**
     * Estrae SPS e PPS dal record avcC (pacchetto di tipo 1 del flusso video) e li restituisce
     * già con lo start code Annex-B davanti. Si prende il primo SPS e il primo PPS: iOS ne invia uno.
     */
    bool parseAvcC(const uint8_t* data, size_t len, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);

} // namespace irplay::nal
