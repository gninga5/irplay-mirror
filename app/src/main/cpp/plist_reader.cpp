/*
 * plist_reader.cpp
 * I byte di rete li interpreta il parser Rust (rust/src/plist.rs), con tutti i limiti
 * descritti in plist_reader.h. Qui si ricostruisce il Value C++ dal risultato già
 * validato, leggendolo con funzioni che controllano ogni indice.
 */
#include "plist_reader.h"

#include <memory>

#include "rust_core.h"

namespace irplay::plist {
    namespace {

        struct DocFree {
            void operator()(PlistDoc* d) const { irplay_plist_free(d); }
        };

        // Profondità già limitata da Rust (16); il controllo qui è solo una difesa in più.
        constexpr int kMaxDepth = 32;

        std::optional<Value> build(const PlistDoc* doc, uint32_t id, int depth) {
            IrplayPlistNode n{};
            if (depth > kMaxDepth || !irplay_plist_node(doc, id, &n)) return std::nullopt;
            switch (n.kind) {
                case kPlistBool: return Value(n.uint != 0);
                case kPlistUInt: return Value(n.uint);
                case kPlistReal: return Value(n.real);
                case kPlistString: return Value(std::string(reinterpret_cast<const char*>(n.bytes), n.len));
                case kPlistData: return Value(Data(n.bytes, n.bytes + n.len));
                case kPlistArray:
                case kPlistDict: {
                    Array arr;
                    Dict dict;
                    for (size_t i = 0; i < n.count; ++i) {
                        uint32_t child = 0;
                        const uint8_t* key = nullptr;
                        size_t keyLen = 0;
                        if (!irplay_plist_child(doc, id, i, &child, &key, &keyLen)) return std::nullopt;
                        auto v = build(doc, child, depth + 1);
                        if (!v) return std::nullopt;
                        if (n.kind == kPlistArray) arr.push_back(std::move(*v));
                        else dict.set(std::string(reinterpret_cast<const char*>(key), keyLen), std::move(*v));
                    }
                    return n.kind == kPlistArray ? Value(std::move(arr)) : Value(std::move(dict));
                }
                default:
                    return std::nullopt;
            }
        }

    } // namespace

    std::optional<Value> parseBinary(const uint8_t* data, size_t len) {
        if (data == nullptr) return std::nullopt;
        const std::unique_ptr<PlistDoc, DocFree> doc(irplay_plist_parse(data, len));
        if (!doc) return std::nullopt;
        return build(doc.get(), 0, 0);
    }

} // namespace irplay::plist
