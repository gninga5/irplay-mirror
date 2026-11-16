# Modifiche rispetto all'originale Apple

Sorgenti presi da https://github.com/macosforge/alac (cartella `codec/`), commit in
`UPSTREAM_COMMIT`, licenza Apache-2.0 (`LICENSE`). Servono solo ai test sul Mac, per generare
pacchetti ALAC come quelli di iOS: l'app usa il decoder ALAC di Symphonia, in Rust (vedi
`app/src/main/rust/src/alac.rs`). `ag_dec.c` è qui perché contiene i parametri usati anche dall'encoder.

## EndianPortable.c: little-endian su ARM

L'originale definisce `TARGET_RT_LITTLE_ENDIAN` solo per `__i386__`, `__x86_64__` e Windows. Su
ARM (i Mac Apple Silicon) le funzioni `Swap*BtoN` non convertivano e i valori big-endian
venivano scritti al contrario. Aggiunto il caso `__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__`.
