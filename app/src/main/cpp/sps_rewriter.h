/*
 * sps_rewriter.h
 * Scopo: ridurre la latenza dei decoder hardware riscrivendo l'SPS H.264 di iOS.
 * Standard: ITU-T H.264 §7.3.2.1 (seq_parameter_set_data) e Annex E (VUI).
 *
 * Problema: l'SPS di iOS non contiene "bitstream_restriction". Senza max_num_reorder_frames
 * il decoder deve supporre che i frame possano arrivare fuori ordine (B-frame) e ne tiene
 * diversi nel buffer prima di restituire il primo: sul MediaTek MT5895 della Sony fino
 * a 18 frame in coda, centinaia di millisecondi. Il mirroring di iOS non usa B-frame, quindi
 * si può dichiarare max_num_reorder_frames = 0 e il decoder restituisce ogni frame subito.
 * Stessa tecnica usata da Moonlight per lo streaming di giochi.
 *
 * Sicurezza del cambiamento: se nel flusso compare uno slice B (sliceIsB), la riscrittura va
 * disattivata e si torna all'SPS originale, altrimenti i frame verrebbero mostrati in ordine
 * sbagliato.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace irplay::nal {

    /**
     * in: SPS Annex-B (start code + NAL tipo 7). out: SPS riscritto con max_num_reorder_frames = 0.
     * Restituisce false se l'SPS non è interpretabile o se dichiara già 0 frame di riordino:
     * in entrambi i casi si usa quello originale.
     */
    bool patchSpsLowLatency(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

    /** nal: unità NAL senza start code (header incluso). true se è uno slice di tipo B. */
    bool sliceIsB(const uint8_t* nal, size_t len);

} // namespace irplay::nal
