/*
 * plist_writer.h
 * Scopo: serializzatore minimale di Apple Binary Property List ("bplist00"),
 * usato nelle risposte a GET /info e, più avanti, a SETUP.
 * Formato: CoreFoundation CFBinaryPList (header di 8 byte, oggetti, tabella
 * degli offset, trailer di 32 byte, interi big-endian).
 * Qui la scrittura: i dati sono prodotti da noi, quindi non passa input ostile.
 * Lo stesso tipo Value è prodotto da plist_reader (input di rete, limiti dedicati).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace irplay::plist {

    struct Value;
    using Array = std::vector<Value>;
    using Data = std::vector<uint8_t>;

/** Dizionario che mantiene l'ordine di inserimento. Usare solo set(): chiavi e valori devono restare appaiati. */
    struct Dict {
        std::vector<std::string> keys;
        std::vector<Value> values;
        Dict& set(std::string key, Value value);
        /** Primo valore con quella chiave, oppure nullptr. Ricerca lineare: i plist di iOS hanno poche decine di chiavi. */
        const Value* find(std::string_view key) const;
    };

    struct Value {
        std::variant<bool, uint64_t, double, std::string, Data, Array, Dict> v;

        Value(bool b) : v(b) {}
        Value(uint64_t n) : v(n) {}
        // Il protocollo usa solo interi non negativi: un valore negativo viene portato a 0
        // invece di diventare un numero enorme dopo la conversione a unsigned.
        Value(int n) : v(static_cast<uint64_t>(n < 0 ? 0 : n)) {}
        Value(double d) : v(d) {}
        Value(std::string s) : v(std::move(s)) {}
        Value(const char* s) : v(std::string(s)) {}
        Value(Data d) : v(std::move(d)) {}
        Value(Array a) : v(std::move(a)) {}
        Value(Dict d) : v(std::move(d)) {}

        // Accesso tipizzato: nullptr se il tipo non è quello atteso. Chi legge input di rete
        // deve sempre controllare il risultato invece di presumere la struttura.
        const bool* asBool() const { return std::get_if<bool>(&v); }
        const uint64_t* asUInt() const { return std::get_if<uint64_t>(&v); }
        const std::string* asString() const { return std::get_if<std::string>(&v); }
        const Data* asData() const { return std::get_if<Data>(&v); }
        const Array* asArray() const { return std::get_if<Array>(&v); }
        const Dict* asDict() const { return std::get_if<Dict>(&v); }
    };

/** Serializza in formato bplist00. Restituisce nullopt se la struttura supera i limiti del formato scelto. */
    std::optional<Data> toBinary(const Value& root);

/**
 * Serializza in XML (text/x-apple-plist+xml). Lo usano le risposte e gli eventi dell'AirPlay
 * video, dove iOS si aspetta il formato testuale. Stringhe con caratteri speciali XML escaped.
 */
    std::string toXml(const Value& root);

} // namespace irplay::plist