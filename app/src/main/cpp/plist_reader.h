/*
 * plist_reader.h
 * Scopo: lettura dei plist binari ("bplist00") che iOS invia in SETUP, TEARDOWN e
 * GET /info. È il primo codice che interpreta strutture annidate ricevute dalla LAN,
 * quindi ogni campo del formato (trailer, tabella offset, riferimenti) è trattato
 * come ostile.
 * Formato: CoreFoundation CFBinaryPList, vedi il commento in plist_writer.cpp.
 *
 * Limiti:
 *  - numero di oggetti dichiarati nel trailer;
 *  - profondità di annidamento (la ricorsione non può esaurire lo stack del thread);
 *  - nodi e byte prodotti in totale: un plist può far riferire molte volte lo stesso
 *    oggetto (grafo aciclico), e senza questo limite 60 KB in ingresso potrebbero
 *    diventare centinaia di MB in memoria ("billion laughs").
 * I cicli non servono un controllo a parte: un ciclo supera il limite di profondità.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "plist_writer.h"

namespace irplay::plist {

    /** Interpreta un bplist00. nullopt se il formato non è valido o supera i limiti. */
    std::optional<Value> parseBinary(const uint8_t* data, size_t len);

} // namespace irplay::plist
