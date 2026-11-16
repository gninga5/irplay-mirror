/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 6).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp.
 */
/*
 * nal_parser.cpp
 * Layout avcC (ISO/IEC 14496-15 §5.2.4.1):
 *   [0] version=1 [1] profile [2] compat [3] level [4] 0xFC|lengthSizeMinusOne
 *   [5] 0xE0|numSPS, poi per ogni SPS: lunghezza 16 bit BE + dati
 *   numPPS (8 bit), poi per ogni PPS: lunghezza 16 bit BE + dati
 */
#include "reference.h"


namespace ref::nal {
    namespace {
        constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};

        uint32_t be32(const uint8_t* p) {
            return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        }
        uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

        void appendAnnexB(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
            out.assign(kStartCode, kStartCode + 4);
            out.insert(out.end(), nal, nal + len);
        }
    } // namespace

    bool avccToAnnexB(uint8_t* data, size_t len, bool& keyframe, bool* bSlice) {
        keyframe = false;
        size_t pos = 0;
        while (pos < len) {
            if (len - pos < 5) return false;                // lunghezza + almeno l'header NAL
            const uint32_t n = be32(data + pos);
            if (n == 0 || n > len - pos - 4) return false;
            const uint8_t header = data[pos + 4];
            if (header & 0x80) return false;                // forbidden_zero_bit: con dati decifrati bene è sempre 0
            if ((header & 0x1F) == 5) keyframe = true;
            if (bSlice && !*bSlice && sliceIsB(data + pos + 4, n)) *bSlice = true;
            data[pos] = 0; data[pos + 1] = 0; data[pos + 2] = 0; data[pos + 3] = 1;
            pos += 4 + n;
        }
        return len > 0;
    }

    bool parseAvcC(const uint8_t* data, size_t len, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
        if (data == nullptr || len < 7 || data[0] != 1) return false;
        size_t pos = 5;
        const unsigned numSps = data[pos++] & 0x1F;
        if (numSps == 0) return false;
        for (unsigned i = 0; i < numSps; ++i) {
            if (len - pos < 2) return false;
            const size_t n = be16(data + pos);
            pos += 2;
            if (n == 0 || n > len - pos) return false;
            if (i == 0) appendAnnexB(sps, data + pos, n);
            pos += n;
        }
        if (len - pos < 1) return false;
        const unsigned numPps = data[pos++];
        if (numPps == 0) return false;
        for (unsigned i = 0; i < numPps; ++i) {
            if (len - pos < 2) return false;
            const size_t n = be16(data + pos);
            pos += 2;
            if (n == 0 || n > len - pos) return false;
            if (i == 0) appendAnnexB(pps, data + pos, n);
            pos += n;
        }
        // Tipi NAL attesi: 7 (SPS) e 8 (PPS). Altro significa codec diverso (es. HEVC "hvc1").
        return (sps[4] & 0x1F) == 7 && (pps[4] & 0x1F) == 8;
    }

} // namespace ref::nal
