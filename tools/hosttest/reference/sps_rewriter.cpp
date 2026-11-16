/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 6).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp.
 */
/*
 * sps_rewriter.cpp
 * Si lavora sull'RBSP (senza i byte 0x03 di "emulation prevention"): si legge l'SPS fino al
 * punto in cui inizia (o manca) la bitstream_restriction, si copiano quei bit invariati e si
 * riscrive la coda. Alla fine si reinseriscono gli 0x03 dove servono.
 * Tutte le letture sono controllate: un SPS anomalo fa fallire la riscrittura, mai la sessione.
 */

#include "reference.h"

namespace ref::nal {
    namespace {

        std::vector<uint8_t> unescape(const uint8_t* p, size_t n) {
            std::vector<uint8_t> out;
            out.reserve(n);
            int zeros = 0;
            for (size_t i = 0; i < n; ++i) {
                if (zeros >= 2 && p[i] == 0x03) { zeros = 0; continue; }
                out.push_back(p[i]);
                zeros = p[i] == 0 ? zeros + 1 : 0;
            }
            return out;
        }

        std::vector<uint8_t> escape(const std::vector<uint8_t>& rbsp) {
            std::vector<uint8_t> out;
            out.reserve(rbsp.size() + 8);
            int zeros = 0;
            for (uint8_t b : rbsp) {
                if (zeros >= 2 && b <= 3) { out.push_back(0x03); zeros = 0; }
                out.push_back(b);
                zeros = b == 0 ? zeros + 1 : 0;
            }
            return out;
        }

        class BitReader {
        public:
            explicit BitReader(const std::vector<uint8_t>& d) : d_(d) {}
            bool ok() const { return ok_; }
            size_t pos() const { return pos_; }

            uint32_t u(int n) {
                uint32_t v = 0;
                for (int i = 0; i < n; ++i) v = (v << 1) | bit();
                return v;
            }
            uint32_t ue() {
                int zeros = 0;
                while (ok_ && bit() == 0) {
                    if (++zeros > 31) { ok_ = false; return 0; }
                }
                if (zeros == 0) return 0;
                return ((1u << zeros) - 1) + u(zeros);
            }
            int32_t se() {
                const uint32_t k = ue();
                return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
            }
            bool bitAt(size_t p) const { return (d_[p / 8] >> (7 - p % 8)) & 1; }

        private:
            uint32_t bit() {
                if (pos_ >= d_.size() * 8) { ok_ = false; return 0; }
                const uint32_t b = bitAt(pos_);
                ++pos_;
                return b;
            }
            const std::vector<uint8_t>& d_;
            size_t pos_ = 0;
            bool ok_ = true;
        };

        class BitWriter {
        public:
            void bit(uint32_t b) {
                if (n_ % 8 == 0) out_.push_back(0);
                if (b) out_.back() |= static_cast<uint8_t>(0x80 >> (n_ % 8));
                ++n_;
            }
            void u(int n, uint32_t v) { for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1); }
            void ue(uint32_t v) {
                const uint64_t x = uint64_t{v} + 1;
                int len = 0;
                while ((x >> len) > 1) ++len;
                for (int i = 0; i < len; ++i) bit(0);
                for (int i = len; i >= 0; --i) bit(static_cast<uint32_t>((x >> i) & 1));
            }
            void copy(const BitReader& r, size_t count) { for (size_t i = 0; i < count; ++i) bit(r.bitAt(i)); }
            void trailing() {
                bit(1);
                while (n_ % 8) bit(0);
            }
            std::vector<uint8_t>& bytes() { return out_; }

        private:
            std::vector<uint8_t> out_;
            size_t n_ = 0;
        };

        void skipScalingList(BitReader& r, int size) {
            int last = 8, next = 8;
            for (int j = 0; j < size && r.ok(); ++j) {
                if (next != 0) next = (last + r.se() + 256) % 256;
                last = next == 0 ? last : next;
            }
        }

        void skipHrd(BitReader& r) {
            const uint32_t cpbCnt = r.ue() + 1;
            if (cpbCnt > 32) { r.u(64); return; }   // fuori specifica: forza il fallimento più avanti
            r.u(4);
            r.u(4);
            for (uint32_t i = 0; i < cpbCnt && r.ok(); ++i) {
                r.ue();
                r.ue();
                r.u(1);
            }
            r.u(20);   // quattro lunghezze da 5 bit
        }

        void writeRestriction(BitWriter& w, uint32_t maxRefFrames) {
            w.u(1, 1);   // motion_vectors_over_pic_boundaries_flag
            w.ue(2);     // max_bytes_per_pic_denom (valore predefinito dello standard)
            w.ue(1);     // max_bits_per_mb_denom
            w.ue(16);    // log2_max_mv_length_horizontal
            w.ue(16);    // log2_max_mv_length_vertical
            w.ue(0);     // max_num_reorder_frames: nessun riordino
            w.ue(maxRefFrames > 0 ? maxRefFrames : 1);   // max_dec_frame_buffering >= max_num_ref_frames
        }

    } // namespace

    bool patchSpsLowLatency(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        if (in.size() < 4 + 4 || in[0] != 0 || in[1] != 0 || in[2] != 0 || in[3] != 1) return false;
        const uint8_t header = in[4];
        if ((header & 0x1F) != 7) return false;
        const std::vector<uint8_t> rbsp = unescape(in.data() + 5, in.size() - 5);
        BitReader r(rbsp);

        const uint32_t profile = r.u(8);
        r.u(8);   // constraint flags
        r.u(8);   // level_idc
        r.ue();   // seq_parameter_set_id
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
            profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
            profile == 139 || profile == 134 || profile == 135) {
            const uint32_t chroma = r.ue();
            if (chroma == 3) r.u(1);
            r.ue();
            r.ue();
            r.u(1);
            if (r.u(1)) {   // seq_scaling_matrix_present_flag
                const int lists = chroma == 3 ? 12 : 8;
                for (int i = 0; i < lists && r.ok(); ++i) {
                    if (r.u(1)) skipScalingList(r, i < 6 ? 16 : 64);
                }
            }
        }
        r.ue();   // log2_max_frame_num_minus4
        const uint32_t pocType = r.ue();
        if (pocType == 0) {
            r.ue();
        } else if (pocType == 1) {
            r.u(1);
            r.se();
            r.se();
            const uint32_t cycle = r.ue();
            if (cycle > 255) return false;
            for (uint32_t i = 0; i < cycle && r.ok(); ++i) r.se();
        }
        const uint32_t maxRefFrames = r.ue();
        r.u(1);   // gaps_in_frame_num_value_allowed_flag
        r.ue();   // pic_width_in_mbs_minus1
        r.ue();   // pic_height_in_map_units_minus1
        if (!r.u(1)) r.u(1);   // frame_mbs_only_flag, mb_adaptive_frame_field_flag
        r.u(1);   // direct_8x8_inference_flag
        if (r.u(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }   // frame cropping
        if (!r.ok() || maxRefFrames > 16) return false;

        BitWriter w;
        const size_t vuiFlagPos = r.pos();
        if (!r.u(1)) {
            // Nessuna VUI: se ne aggiunge una minima con la sola bitstream_restriction.
            w.copy(r, vuiFlagPos);
            w.u(1, 1);   // vui_parameters_present_flag
            for (int i = 0; i < 8; ++i) w.u(1, 0);   // aspect, overscan, signal, chroma_loc, timing, nal_hrd, vcl_hrd, pic_struct
            w.u(1, 1);   // bitstream_restriction_flag
            writeRestriction(w, maxRefFrames);
        } else {
            if (r.u(1) && r.u(8) == 255) { r.u(16); r.u(16); }   // aspect_ratio_info
            if (r.u(1)) r.u(1);                                   // overscan
            if (r.u(1)) { r.u(3); r.u(1); if (r.u(1)) r.u(24); }  // video_signal_type
            if (r.u(1)) { r.ue(); r.ue(); }                       // chroma_loc_info
            if (r.u(1)) { r.u(32); r.u(32); r.u(1); }             // timing_info
            const bool nalHrd = r.u(1);
            if (nalHrd) skipHrd(r);
            const bool vclHrd = r.u(1);
            if (vclHrd) skipHrd(r);
            if (nalHrd || vclHrd) r.u(1);                         // low_delay_hrd_flag
            r.u(1);                                               // pic_struct_present_flag
            if (!r.ok()) return false;
            const size_t restrictionPos = r.pos();
            if (r.u(1)) {
                r.u(1); r.ue(); r.ue(); r.ue(); r.ue();
                const uint32_t reorder = r.ue();
                r.ue();
                if (!r.ok() || reorder == 0) return false;   // già a bassa latenza: niente da fare
            }
            w.copy(r, restrictionPos);
            w.u(1, 1);
            writeRestriction(w, maxRefFrames);
        }
        if (!r.ok()) return false;
        w.trailing();

        out.assign({0, 0, 0, 1, header});
        const std::vector<uint8_t> escaped = escape(w.bytes());
        out.insert(out.end(), escaped.begin(), escaped.end());
        return true;
    }

    bool sliceIsB(const uint8_t* nal, size_t len) {
        if (len < 2) return false;
        const uint8_t type = nal[0] & 0x1F;
        if (type != 1 && type != 5) return false;
        const std::vector<uint8_t> rbsp = unescape(nal + 1, len - 1 < 16 ? len - 1 : 16);
        BitReader r(rbsp);
        r.ue();   // first_mb_in_slice
        const uint32_t sliceType = r.ue();
        return r.ok() && sliceType % 5 == 1;
    }

} // namespace ref::nal
