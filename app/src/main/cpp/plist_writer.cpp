/*
 * plist_writer.cpp
 * Layout prodotto:
 *   "bplist00" | oggetti | tabella offset (4 byte ciascuno) | trailer di 32 byte
 *   trailer = 6 byte a zero, offsetIntSize, objectRefSize, numObjects (8 byte),
 *             topObject (8 byte), offsetTableOffset (8 byte)
 * Riferimenti agli oggetti da 2 byte: fino a 65535 oggetti (a /info ne servono meno di 100).
 * Gli interi sono sempre scritti su 8 byte (marker 0x13): codifica più semplice e
 * sempre valida, che costa pochi byte in più.
 */
#include "plist_writer.h"

#include <cstring>

namespace irplay::plist {

    Dict& Dict::set(std::string key, Value value) {
        keys.push_back(std::move(key));
        values.push_back(std::move(value));
        return *this;
    }

    const Value* Dict::find(std::string_view key) const {
        for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }

    namespace {

        constexpr uint8_t kRefSize = 2;
        constexpr uint8_t kOffsetSize = 4;

        struct Obj {
            const Value* value;        // null quando l'oggetto è una chiave di dizionario
            const std::string* key;
        };

// Appiattisce l'albero assegnando a ogni oggetto un indice, come richiede il formato.
// Per i dizionari vengono prima tutte le chiavi, poi tutti i valori.
        struct Flattener {
            std::vector<Obj> objs;
            std::vector<std::vector<uint32_t>> refs;
            bool ok = true;

            uint32_t addKey(const std::string& k) {
                const auto idx = static_cast<uint32_t>(objs.size());
                objs.push_back({nullptr, &k});
                refs.emplace_back();
                return idx;
            }

            uint32_t add(const Value& v) {
                const auto idx = static_cast<uint32_t>(objs.size());
                objs.push_back({&v, nullptr});
                refs.emplace_back();
                std::vector<uint32_t> r;
                if (const auto* a = std::get_if<Array>(&v.v)) {
                    for (const auto& c : *a) r.push_back(add(c));
                } else if (const auto* d = std::get_if<Dict>(&v.v)) {
                    if (d->keys.size() != d->values.size()) ok = false;
                    for (const auto& k : d->keys) r.push_back(addKey(k));
                    for (const auto& c : d->values) r.push_back(add(c));
                }
                refs[idx] = std::move(r);   // accesso per indice: refs può essere stato riallocato durante la ricorsione
                return idx;
            }
        };

        void putBE(Data& out, uint64_t value, int bytes) {
            for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }

        void putInt(Data& out, uint64_t value) {
            out.push_back(0x13);            // intero su 2^3 = 8 byte
            putBE(out, value, 8);
        }

        void putMarker(Data& out, uint8_t type, size_t len) {
            if (len < 15) {
                out.push_back(static_cast<uint8_t>((type << 4) | len));
            } else {
                out.push_back(static_cast<uint8_t>((type << 4) | 0x0F));
                putInt(out, len);           // la lunghezza segue come oggetto intero
            }
        }

        void putAscii(Data& out, const std::string& s) {
            // Tipo 0x5 = stringa ASCII. Un nome non ASCII richiederebbe UTF-16BE (tipo 0x6):
            // per ora i byte non ASCII diventano '?', perché il nome del ricevitore è fisso ("iRPlay").
            putMarker(out, 0x5, s.size());
            for (unsigned char c : s) out.push_back(c < 0x80 ? c : static_cast<uint8_t>('?'));
        }

    } // namespace

    std::optional<Data> toBinary(const Value& root) {
        Flattener f;
        f.add(root);
        if (!f.ok || f.objs.size() > 0xFFFF) return std::nullopt;

        Data out = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        std::vector<uint64_t> offsets;
        offsets.reserve(f.objs.size());

        for (size_t i = 0; i < f.objs.size(); ++i) {
            offsets.push_back(out.size());
            const Obj& o = f.objs[i];
            if (o.key) {
                putAscii(out, *o.key);
                continue;
            }
            const auto& v = o.value->v;
            if (const auto* b = std::get_if<bool>(&v)) {
                out.push_back(*b ? 0x09 : 0x08);
            } else if (const auto* n = std::get_if<uint64_t>(&v)) {
                putInt(out, *n);
            } else if (const auto* d = std::get_if<double>(&v)) {
                uint64_t bits;
                std::memcpy(&bits, d, sizeof bits);   // IEEE-754 a 64 bit, scritto big-endian
                out.push_back(0x23);
                putBE(out, bits, 8);
            } else if (const auto* s = std::get_if<std::string>(&v)) {
                putAscii(out, *s);
            } else if (const auto* data = std::get_if<Data>(&v)) {
                putMarker(out, 0x4, data->size());
                out.insert(out.end(), data->begin(), data->end());
            } else if (const auto* a = std::get_if<Array>(&v)) {
                putMarker(out, 0xA, a->size());
                for (uint32_t r : f.refs[i]) putBE(out, r, kRefSize);
            } else if (const auto* dict = std::get_if<Dict>(&v)) {
                putMarker(out, 0xD, dict->keys.size());
                for (uint32_t r : f.refs[i]) putBE(out, r, kRefSize);
            }
        }

        const uint64_t offsetTable = out.size();
        if (offsetTable > 0xFFFFFFFFu) return std::nullopt;
        for (uint64_t off : offsets) putBE(out, off, kOffsetSize);

        out.insert(out.end(), 6, 0);
        out.push_back(kOffsetSize);
        out.push_back(kRefSize);
        putBE(out, f.objs.size(), 8);
        putBE(out, 0, 8);               // l'oggetto radice è sempre il primo
        putBE(out, offsetTable, 8);
        return out;
    }

} // namespace irplay::plist