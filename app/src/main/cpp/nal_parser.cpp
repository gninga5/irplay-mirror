/*
 * nal_parser.cpp
 * Vedi nal_parser.h. I byte di rete li legge Rust (rust/src/h264.rs); qui gli involucri.
 */
#include "nal_parser.h"

#include <memory>

#include "rust_core.h"
#include "sps_rewriter.h"

namespace irplay::nal {
    namespace {
        struct ListFree {
            void operator()(ByteList* l) const { irplay_bytes_free(l); }
        };
        using List = std::unique_ptr<ByteList, ListFree>;

        bool item(const List& l, size_t i, std::vector<uint8_t>& out) {
            const uint8_t* d = nullptr;
            size_t n = 0;
            if (!irplay_bytes_get(l.get(), i, &d, &n)) return false;
            out.assign(d, d + n);
            return true;
        }
    } // namespace

    bool avccToAnnexB(uint8_t* data, size_t len, bool& keyframe, bool* bSlice) {
        bool b = false;
        const bool ok = data != nullptr && irplay_h264_annexb(data, len, bSlice != nullptr && !*bSlice, &keyframe, &b);
        if (!ok) keyframe = false;
        if (ok && bSlice && b) *bSlice = true;
        return ok;
    }

    bool parseAvcC(const uint8_t* data, size_t len, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
        if (data == nullptr) return false;
        const List l(irplay_h264_parse_avcc(data, len));
        return l && item(l, 0, sps) && item(l, 1, pps);
    }

    bool patchSpsLowLatency(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        const List l(irplay_h264_patch_sps(in.data(), in.size()));
        return l && item(l, 0, out);
    }

    bool sliceIsB(const uint8_t* nal, size_t len) {
        return nal != nullptr && irplay_h264_slice_is_b(nal, len);
    }

} // namespace irplay::nal
