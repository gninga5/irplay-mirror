//! Componente Rust di iRPlay.
//!
//! Interfaccia verso il C++: solo funzioni `extern "C"` con tipi semplici, dichiarate in
//! `app/src/main/cpp/rust_core.h`. Il codice `unsafe` sta solo in questa interfaccia, e
//! nessun panic la attraversa (il profilo di compilazione usa `panic = "abort"`).

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

/// Versione dell'interfaccia C. Il C++ la controlla all'avvio: se non coincide con quella
/// che si aspetta, la libreria Rust è di un'altra build e non va usata.
pub const ABI_VERSION: u32 = 1;

/// Versione dell'interfaccia C esposta da questa libreria (vedi [`ABI_VERSION`]).
#[unsafe(no_mangle)]
pub extern "C" fn irplay_rust_abi_version() -> u32 {
    ABI_VERSION
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_stable() {
        assert_eq!(irplay_rust_abi_version(), 1);
    }
}
