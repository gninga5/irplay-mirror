/*
 * Versione C++ di riferimento, com'era nell'app prima del passaggio a Rust (fase 4).
 * Non è compilata nell'app: serve solo a tools/hosttest/parser_diff.cpp, che confronta
 * il risultato del parser Rust con questo su milioni di input validi e alterati.
 */
/*
 * plist_reader.cpp
 * Marker (byte alto = tipo, byte basso = info):
 *   0x0 null/bool/fill  0x1 intero  0x2 reale  0x3 data  0x4 dati  0x5 ASCII
 *   0x6 UTF-16BE  0x8 UID  0xA array  0xD dizionario
 * Lunghezze >= 15: info = 0xF, seguita da un oggetto intero con la lunghezza vera.
 * Gli interi di 8 byte sono con segno: vengono conservati come bit grezzi in uint64_t,
 * che è ciò che serve per streamConnectionID (UxPlay lo stampa come unsigned).
 */
#include "reference.h"

#include <cstring>

namespace ref::plist {
    using namespace irplay::plist;
    namespace {

        constexpr uint64_t kMaxObjects = 4096;
        constexpr int kMaxDepth = 16;
        constexpr size_t kMaxNodes = 8192;
        constexpr size_t kMaxOutputBytes = 256 * 1024;

        uint64_t readBE(const uint8_t* p, size_t n) {
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
            return v;
        }

        void appendUtf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        class Parser {
        public:
            Parser(const uint8_t* d, size_t len) : d_(d), len_(len) {}

            std::optional<Value> run() {
                if (len_ < 8 + 32 || std::memcmp(d_, "bplist00", 8) != 0) return std::nullopt;
                const uint8_t* t = d_ + len_ - 32;
                offSize_ = t[6];
                refSize_ = t[7];
                numObjects_ = readBE(t + 8, 8);
                const uint64_t top = readBE(t + 16, 8);
                tableOff_ = readBE(t + 24, 8);

                if (offSize_ < 1 || offSize_ > 8 || refSize_ < 1 || refSize_ > 8) return std::nullopt;
                if (numObjects_ == 0 || numObjects_ > kMaxObjects || top >= numObjects_) return std::nullopt;
                // numObjects_ <= 4096 e offSize_ <= 8: il prodotto non può andare in overflow.
                if (tableOff_ < 8 || tableOff_ > len_ - 32 ||
                    numObjects_ * offSize_ > len_ - 32 - tableOff_) return std::nullopt;

                Value out(false);
                if (!parse(top, 0, out)) return std::nullopt;
                return out;
            }

        private:
            // Posizione dell'oggetto idx; gli oggetti stanno tra l'header e la tabella degli offset.
            bool objectOffset(uint64_t idx, size_t& off) const {
                if (idx >= numObjects_) return false;
                const uint64_t o = readBE(d_ + tableOff_ + idx * offSize_, offSize_);
                if (o < 8 || o >= tableOff_) return false;
                off = static_cast<size_t>(o);
                return true;
            }

            // Byte disponibili da pos fino all'inizio della tabella degli offset.
            size_t avail(size_t pos) const { return pos < tableOff_ ? static_cast<size_t>(tableOff_) - pos : 0; }

            // Legge la lunghezza di un contenitore/stringa; pos avanza oltre il marker ed eventuale intero.
            bool readCount(size_t& pos, uint8_t info, uint64_t& count) const {
                pos += 1;
                if (info != 0x0F) { count = info; return true; }
                if (avail(pos) < 1) return false;
                const uint8_t m = d_[pos];
                if ((m >> 4) != 0x1 || (m & 0x0F) > 3) return false;
                const size_t n = size_t{1} << (m & 0x0F);
                if (avail(pos + 1) < n) return false;
                count = readBE(d_ + pos + 1, n);
                pos += 1 + n;
                return true;
            }

            bool charge(size_t bytes) {
                if (++nodes_ > kMaxNodes) return false;
                outBytes_ += bytes;
                return outBytes_ <= kMaxOutputBytes;
            }

            bool parse(uint64_t idx, int depth, Value& out) {
                if (depth > kMaxDepth) return false;
                size_t pos;
                if (!objectOffset(idx, pos)) return false;
                const uint8_t marker = d_[pos];
                const uint8_t type = marker >> 4, info = marker & 0x0F;

                switch (type) {
                    case 0x0:
                        if (!charge(0)) return false;
                        if (marker == 0x08 || marker == 0x00 || marker == 0x0F) { out = Value(false); return true; }
                        if (marker == 0x09) { out = Value(true); return true; }
                        return false;

                    case 0x1: {
                        if (info > 4 || !charge(0)) return false;
                        const size_t n = size_t{1} << info;
                        if (avail(pos + 1) < n) return false;
                        // Interi da 16 byte: si tengono gli 8 byte meno significativi.
                        const size_t skip = n > 8 ? n - 8 : 0;
                        out = Value(readBE(d_ + pos + 1 + skip, n - skip));
                        return true;
                    }

                    case 0x2:
                    case 0x3: {
                        if ((info != 2 && info != 3) || (type == 0x3 && info != 3) || !charge(0)) return false;
                        const size_t n = size_t{1} << info;
                        if (avail(pos + 1) < n) return false;
                        const uint64_t bits = readBE(d_ + pos + 1, n);
                        if (n == 4) {
                            const auto b32 = static_cast<uint32_t>(bits);
                            float f;
                            std::memcpy(&f, &b32, sizeof f);
                            out = Value(static_cast<double>(f));
                        } else {
                            double dv;
                            std::memcpy(&dv, &bits, sizeof dv);
                            out = Value(dv);
                        }
                        return true;
                    }

                    case 0x4:
                    case 0x5: {
                        uint64_t count;
                        if (!readCount(pos, info, count) || count > avail(pos) || !charge(count)) return false;
                        const uint8_t* p = d_ + pos;
                        if (type == 0x4) out = Value(Data(p, p + count));
                        else out = Value(std::string(reinterpret_cast<const char*>(p), count));
                        return true;
                    }

                    case 0x6: {
                        uint64_t count;
                        if (!readCount(pos, info, count) || count > avail(pos) / 2 || !charge(count * 3)) return false;
                        std::string s;
                        s.reserve(count);
                        const uint8_t* p = d_ + pos;
                        for (uint64_t i = 0; i < count; ++i) {
                            uint32_t cp = readBE(p + 2 * i, 2);
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count) {
                                const uint32_t lo = readBE(p + 2 * (i + 1), 2);
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    ++i;
                                } else {
                                    cp = 0xFFFD;
                                }
                            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                                cp = 0xFFFD;   // surrogato isolato: non è un carattere valido
                            }
                            appendUtf8(s, cp);
                        }
                        out = Value(std::move(s));
                        return true;
                    }

                    case 0x8: {
                        const size_t n = static_cast<size_t>(info) + 1;
                        if (n > 8 || avail(pos + 1) < n || !charge(0)) return false;
                        out = Value(readBE(d_ + pos + 1, n));
                        return true;
                    }

                    case 0xA: {
                        uint64_t count;
                        if (!readCount(pos, info, count) || count > kMaxObjects ||
                            count * refSize_ > avail(pos) || !charge(0)) return false;
                        Array arr;
                        arr.reserve(count);
                        for (uint64_t i = 0; i < count; ++i) {
                            Value child(false);
                            if (!parse(readBE(d_ + pos + i * refSize_, refSize_), depth + 1, child)) return false;
                            arr.push_back(std::move(child));
                        }
                        out = Value(std::move(arr));
                        return true;
                    }

                    case 0xD: {
                        uint64_t count;
                        if (!readCount(pos, info, count) || count > kMaxObjects ||
                            2 * count * refSize_ > avail(pos) || !charge(0)) return false;
                        Dict dict;
                        dict.keys.reserve(count);
                        dict.values.reserve(count);
                        for (uint64_t i = 0; i < count; ++i) {
                            Value key(false), val(false);
                            if (!parse(readBE(d_ + pos + i * refSize_, refSize_), depth + 1, key)) return false;
                            const std::string* k = key.asString();
                            if (k == nullptr) return false;   // le chiavi devono essere stringhe
                            if (!parse(readBE(d_ + pos + (count + i) * refSize_, refSize_), depth + 1, val)) return false;
                            dict.set(*k, std::move(val));
                        }
                        out = Value(std::move(dict));
                        return true;
                    }

                    default:
                        return false;   // set (0xC) e tipi riservati: iOS non li usa nei messaggi AirPlay
                }
            }

            const uint8_t* d_;
            size_t len_;
            uint8_t offSize_ = 0, refSize_ = 0;
            uint64_t numObjects_ = 0, tableOff_ = 0;
            size_t nodes_ = 0, outBytes_ = 0;
        };

    } // namespace

    std::optional<Value> parseBinary(const uint8_t* data, size_t len) {
        if (data == nullptr) return std::nullopt;
        return Parser(data, len).run();
    }

} // namespace ref::plist
