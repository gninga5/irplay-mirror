/*
 * rust_core.h
 * Scopo: dichiarazioni delle funzioni del componente Rust (app/src/main/rust), libreria
 * statica collegata dentro libirplay_native.so. Ogni funzione qui deve corrispondere a una
 * `extern "C"` in Rust; kRustAbiVersion va aumentata insieme a ABI_VERSION in lib.rs.
 */
#pragma once

#include <cstdint>

namespace irplay {
    inline constexpr uint32_t kRustAbiVersion = 1;
}

extern "C" {
    uint32_t irplay_rust_abi_version();
}
