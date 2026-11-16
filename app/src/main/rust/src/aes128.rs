//! AES-128 per i flussi AirPlay: CTR (video) e CBC (audio), con la stessa semantica di
//! mbedTLS, che l'app usava prima.
//!
//! Motori:
//! - ARM a 32 bit con estensioni crittografiche ARMv8 (la gran parte delle TV: CPU a 64 bit
//!   con Android a 32 bit): istruzioni AESE/AESMC/AESD/AESIMC. RustCrypto non ha un motore
//!   hardware per questa architettura; quello software è a tempo costante ma, sulla TCL
//!   (Cortex-A55), fa 19 MB/s contro i ~680 MB/s delle istruzioni.
//! - Tutto il resto: il crate `aes` di RustCrypto, che usa da solo le istruzioni hardware su
//!   ARM a 64 bit e x86 e altrimenti un'implementazione software a tempo costante (fixslice).
//!
//! Anche l'espansione della chiave usa l'istruzione AESE (nessuna tabella indicizzata dalla
//! chiave). Le chiavi espanse sono azzerate quando il contesto viene liberato.

use aes::cipher::{BlockCipherDecrypt, BlockCipherEncrypt, KeyInit};
use zeroize::Zeroize;

/// Blocco AES.
pub type Block = [u8; 16];

/// Cifrario AES-128 (cifratura e decifratura di blocchi).
pub struct Aes128 {
    engine: Engine,
}

enum Engine {
    #[cfg(target_arch = "arm")]
    Arm(Box<arm::Keys>),
    Soft(Box<aes::Aes128>),
}

impl Aes128 {
    /// Motore migliore disponibile su questa CPU.
    pub fn new(key: &[u8; 16]) -> Self {
        #[cfg(target_arch = "arm")]
        if arm::available() {
            return Aes128 { engine: Engine::Arm(Box::new(arm::Keys::new(key))) };
        }
        Self::soft(key)
    }

    /// Motore RustCrypto (per i test, e dove le istruzioni mancano).
    pub fn soft(key: &[u8; 16]) -> Self {
        Aes128 { engine: Engine::Soft(Box::new(aes::Aes128::new(&(*key).into()))) }
    }

    /// true se usa le istruzioni ARMv8 a 32 bit.
    pub fn arm_hardware(&self) -> bool {
        match self.engine {
            #[cfg(target_arch = "arm")]
            Engine::Arm(_) => true,
            Engine::Soft(_) => false,
        }
    }

    /// Cifra blocchi sul posto.
    pub fn encrypt_blocks(&self, blocks: &mut [Block]) {
        if blocks.is_empty() {
            return;
        }
        match &self.engine {
            #[cfg(target_arch = "arm")]
            Engine::Arm(k) => k.encrypt(blocks),
            Engine::Soft(c) => c.encrypt_blocks(aes::Block::cast_slice_from_core_mut(blocks)),
        }
    }

    /// Decifra blocchi sul posto.
    pub fn decrypt_blocks(&self, blocks: &mut [Block]) {
        if blocks.is_empty() {
            return;
        }
        match &self.engine {
            #[cfg(target_arch = "arm")]
            Engine::Arm(k) => k.decrypt(blocks),
            Engine::Soft(c) => c.decrypt_blocks(aes::Block::cast_slice_from_core_mut(blocks)),
        }
    }
}

/// Blocchi elaborati per volta da CTR e CBC (1 KB sullo stack).
const BATCH: usize = 64;

fn increment(counter: &mut Block) {
    for b in counter.iter_mut().rev() {
        *b = b.wrapping_add(1);
        if *b != 0 {
            break;
        }
    }
}

/// AES-128-CTR con keystream continuo tra chiamate successive, contatore big-endian a 128
/// bit (come mbedtls_aes_crypt_ctr e come il flusso video di iOS).
pub struct Ctr {
    aes: Aes128,
    counter: Block,
    stream: Block,
    /// Byte già usati di `stream`; 0 = serve un blocco nuovo.
    off: usize,
}

impl Ctr {
    /// Nuovo flusso con chiave e IV (contatore iniziale).
    pub fn new(key: &[u8; 16], iv: &[u8; 16]) -> Self {
        Self::with(Aes128::new(key), iv)
    }

    /// Flusso su un cifrario dato (per i test dei due motori).
    pub fn with(aes: Aes128, iv: &[u8; 16]) -> Self {
        Ctr { aes, counter: *iv, stream: [0; 16], off: 0 }
    }

    /// Cifra o decifra sul posto, proseguendo il keystream.
    pub fn apply(&mut self, data: &mut [u8]) {
        let mut i = 0;
        while self.off != 0 && i < data.len() {
            data[i] ^= self.stream[self.off];
            self.off = (self.off + 1) & 15;
            i += 1;
        }
        let mut ks = [[0u8; 16]; BATCH];
        while data.len() - i >= 16 {
            let n = ((data.len() - i) / 16).min(BATCH);
            for b in &mut ks[..n] {
                *b = self.counter;
                increment(&mut self.counter);
            }
            self.aes.encrypt_blocks(&mut ks[..n]);
            for (d, k) in data[i..i + 16 * n].iter_mut().zip(ks[..n].as_flattened()) {
                *d ^= *k;
            }
            i += 16 * n;
        }
        ks.zeroize();
        if i < data.len() {
            self.stream = self.counter;
            increment(&mut self.counter);
            self.aes.encrypt_blocks(core::slice::from_mut(&mut self.stream));
            for d in &mut data[i..] {
                *d ^= self.stream[self.off];
                self.off += 1;
            }
        }
    }

    /// Consuma len byte di keystream senza produrre output.
    pub fn skip(&mut self, mut len: usize) {
        let mut zeros = [0u8; 64];
        while len > 0 {
            let n = len.min(zeros.len());
            self.apply(&mut zeros[..n]);
            len -= n;
        }
        zeros.zeroize();
    }
}

impl Drop for Ctr {
    fn drop(&mut self) {
        self.counter.zeroize();
        self.stream.zeroize();
    }
}

/// Decifratura AES-128-CBC dell'audio: chiave fissa, IV che riparte da capo a ogni pacchetto;
/// i byte finali che non formano un blocco restano com'erano (sono in chiaro).
pub struct Cbc {
    aes: Aes128,
    iv: Block,
}

impl Cbc {
    /// Nuovo decifratore.
    pub fn new(key: &[u8; 16], iv: &[u8; 16]) -> Self {
        Self::with(Aes128::new(key), iv)
    }

    /// Decifratore su un cifrario dato (per i test dei due motori).
    pub fn with(aes: Aes128, iv: &[u8; 16]) -> Self {
        Cbc { aes, iv: *iv }
    }

    /// Decifra sul posto i blocchi completi di un pacchetto.
    pub fn decrypt(&self, data: &mut [u8]) {
        let (blocks, _) = data.as_chunks_mut::<16>();
        let mut prev = self.iv;
        let mut saved = [[0u8; 16]; BATCH];
        for chunk in blocks.chunks_mut(BATCH) {
            saved[..chunk.len()].copy_from_slice(chunk);
            self.aes.decrypt_blocks(chunk);
            for (j, b) in chunk.iter_mut().enumerate() {
                let p = if j == 0 { &prev } else { &saved[j - 1] };
                for (x, y) in b.iter_mut().zip(p) {
                    *x ^= *y;
                }
            }
            prev = saved[chunk.len() - 1];
        }
    }
}

impl Drop for Cbc {
    fn drop(&mut self) {
        self.iv.zeroize();
    }
}

#[cfg(target_arch = "arm")]
mod arm {
    //! AES-128 con le estensioni crittografiche ARMv8 in modalità AArch32. Le direttive
    //! `.arch`/`.fpu` abilitano le istruzioni solo dentro questi blocchi: il resto della
    //! libreria resta ARMv7, e qui si entra solo se il kernel segnala HWCAP2_AES.
    use core::arch::asm;
    use core::sync::atomic::{AtomicU8, Ordering};
    use zeroize::Zeroize;

    unsafe extern "C" {
        fn getauxval(kind: core::ffi::c_ulong) -> core::ffi::c_ulong;
    }

    /// true se la CPU ha le istruzioni AES (risultato tenuto in memoria).
    pub fn available() -> bool {
        static CACHE: AtomicU8 = AtomicU8::new(0); // 0 non noto, 1 no, 2 sì
        match CACHE.load(Ordering::Relaxed) {
            1 => false,
            2 => true,
            _ => {
                const AT_HWCAP2: core::ffi::c_ulong = 26;
                const HWCAP2_AES: core::ffi::c_ulong = 1;
                // SAFETY: getauxval di bionic, senza precondizioni.
                let yes = unsafe { getauxval(AT_HWCAP2) } & HWCAP2_AES != 0;
                CACHE.store(if yes { 2 } else { 1 }, Ordering::Relaxed);
                yes
            }
        }
    }

    /// Chiavi di ciclo per cifratura e decifratura (11 da 16 byte ciascuna).
    pub struct Keys {
        enc: [[u8; 16]; 11],
        dec: [[u8; 16]; 11],
    }

    impl Drop for Keys {
        fn drop(&mut self) {
            self.enc.zeroize();
            self.dec.zeroize();
        }
    }

    /// SubWord(w) con AESE e chiave nulla: con le quattro colonne uguali ShiftRows non cambia
    /// nulla e resta solo SubBytes, a tempo costante.
    fn sub_word(w: u32) -> u32 {
        let out: u32;
        // SAFETY: chiamata solo se available(); usa solo q8-q9, dichiarati come sovrascritti.
        unsafe {
            asm!(
                ".arch armv8-a",
                ".fpu crypto-neon-fp-armv8",
                "vdup.32 q8, {w}",
                "vmov.i32 q9, #0",
                "aese.8 q8, q9",
                "vmov.32 {o}, d16[0]",
                ".arch armv7-a",
                ".fpu vfpv3-d16",
                w = in(reg) w,
                o = out(reg) out,
                out("d16") _, out("d17") _, out("d18") _, out("d19") _,
                options(nostack, nomem, pure),
            );
        }
        out
    }

    /// InvMixColumns di una chiave di ciclo (per la decifratura).
    fn inv_mix_columns(k: &mut [u8; 16]) {
        // SAFETY: chiamata solo se available(); legge e scrive 16 byte di `k`.
        unsafe {
            asm!(
                ".arch armv8-a",
                ".fpu crypto-neon-fp-armv8",
                "vld1.8 {{q8}}, [{p}]",
                "aesimc.8 q8, q8",
                "vst1.8 {{q8}}, [{p}]",
                ".arch armv7-a",
                ".fpu vfpv3-d16",
                p = in(reg) k.as_mut_ptr(),
                out("d16") _, out("d17") _,
                options(nostack),
            );
        }
    }

    impl Keys {
        /// Espansione della chiave (FIPS-197 §5.2).
        pub fn new(key: &[u8; 16]) -> Keys {
            const RCON: [u32; 10] = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36];
            let mut w = [0u32; 44];
            for (i, c) in key.as_chunks::<4>().0.iter().enumerate() {
                w[i] = u32::from_le_bytes(*c);
            }
            for i in 4..44 {
                let mut t = w[i - 1];
                if i % 4 == 0 {
                    t = sub_word(t).rotate_right(8) ^ RCON[i / 4 - 1];
                }
                w[i] = w[i - 4] ^ t;
            }
            let mut k = Keys { enc: [[0; 16]; 11], dec: [[0; 16]; 11] };
            for (r, rk) in k.enc.iter_mut().enumerate() {
                for c in 0..4 {
                    rk[4 * c..4 * c + 4].copy_from_slice(&w[4 * r + c].to_le_bytes());
                }
            }
            w.zeroize();
            // Cifrario inverso equivalente: chiavi in ordine inverso, quelle interne con
            // InvMixColumns.
            k.dec[0] = k.enc[10];
            for i in 1..10 {
                k.dec[i] = k.enc[10 - i];
                inv_mix_columns(&mut k.dec[i]);
            }
            k.dec[10] = k.enc[0];
            k
        }

        pub fn encrypt(&self, blocks: &mut [[u8; 16]]) {
            // SAFETY: chiamata solo se available(); legge 176 byte di chiavi e legge/scrive
            // blocks.len() * 16 byte (> 0, controllato dal chiamante). Registri usati
            // dichiarati come sovrascritti.
            unsafe {
                asm!(
                    ".arch armv8-a",
                    ".fpu crypto-neon-fp-armv8",
                    "vld1.8 {{q0}}, [{k}]!", "vld1.8 {{q1}}, [{k}]!", "vld1.8 {{q2}}, [{k}]!",
                    "vld1.8 {{q3}}, [{k}]!", "vld1.8 {{q4}}, [{k}]!", "vld1.8 {{q5}}, [{k}]!",
                    "vld1.8 {{q6}}, [{k}]!", "vld1.8 {{q7}}, [{k}]!", "vld1.8 {{q8}}, [{k}]!",
                    "vld1.8 {{q9}}, [{k}]!", "vld1.8 {{q10}}, [{k}]",
                    "2:",
                    "vld1.8 {{q11}}, [{p}]",
                    "aese.8 q11, q0", "aesmc.8 q11, q11",
                    "aese.8 q11, q1", "aesmc.8 q11, q11",
                    "aese.8 q11, q2", "aesmc.8 q11, q11",
                    "aese.8 q11, q3", "aesmc.8 q11, q11",
                    "aese.8 q11, q4", "aesmc.8 q11, q11",
                    "aese.8 q11, q5", "aesmc.8 q11, q11",
                    "aese.8 q11, q6", "aesmc.8 q11, q11",
                    "aese.8 q11, q7", "aesmc.8 q11, q11",
                    "aese.8 q11, q8", "aesmc.8 q11, q11",
                    "aese.8 q11, q9",
                    "veor q11, q11, q10",
                    "vst1.8 {{q11}}, [{p}]!",
                    "subs {n}, {n}, #1",
                    "bne 2b",
                    ".arch armv7-a",
                    ".fpu vfpv3-d16",
                    k = inout(reg) self.enc.as_ptr() => _,
                    p = inout(reg) blocks.as_mut_ptr() => _,
                    n = inout(reg) blocks.len() => _,
                    out("d0") _, out("d1") _, out("d2") _, out("d3") _, out("d4") _, out("d5") _,
                    out("d6") _, out("d7") _, out("d8") _, out("d9") _, out("d10") _, out("d11") _,
                    out("d12") _, out("d13") _, out("d14") _, out("d15") _, out("d16") _, out("d17") _,
                    out("d18") _, out("d19") _, out("d20") _, out("d21") _, out("d22") _, out("d23") _,
                    options(nostack),
                );
            }
        }

        pub fn decrypt(&self, blocks: &mut [[u8; 16]]) {
            // SAFETY: come encrypt.
            unsafe {
                asm!(
                    ".arch armv8-a",
                    ".fpu crypto-neon-fp-armv8",
                    "vld1.8 {{q0}}, [{k}]!", "vld1.8 {{q1}}, [{k}]!", "vld1.8 {{q2}}, [{k}]!",
                    "vld1.8 {{q3}}, [{k}]!", "vld1.8 {{q4}}, [{k}]!", "vld1.8 {{q5}}, [{k}]!",
                    "vld1.8 {{q6}}, [{k}]!", "vld1.8 {{q7}}, [{k}]!", "vld1.8 {{q8}}, [{k}]!",
                    "vld1.8 {{q9}}, [{k}]!", "vld1.8 {{q10}}, [{k}]",
                    "2:",
                    "vld1.8 {{q11}}, [{p}]",
                    "aesd.8 q11, q0", "aesimc.8 q11, q11",
                    "aesd.8 q11, q1", "aesimc.8 q11, q11",
                    "aesd.8 q11, q2", "aesimc.8 q11, q11",
                    "aesd.8 q11, q3", "aesimc.8 q11, q11",
                    "aesd.8 q11, q4", "aesimc.8 q11, q11",
                    "aesd.8 q11, q5", "aesimc.8 q11, q11",
                    "aesd.8 q11, q6", "aesimc.8 q11, q11",
                    "aesd.8 q11, q7", "aesimc.8 q11, q11",
                    "aesd.8 q11, q8", "aesimc.8 q11, q11",
                    "aesd.8 q11, q9",
                    "veor q11, q11, q10",
                    "vst1.8 {{q11}}, [{p}]!",
                    "subs {n}, {n}, #1",
                    "bne 2b",
                    ".arch armv7-a",
                    ".fpu vfpv3-d16",
                    k = inout(reg) self.dec.as_ptr() => _,
                    p = inout(reg) blocks.as_mut_ptr() => _,
                    n = inout(reg) blocks.len() => _,
                    out("d0") _, out("d1") _, out("d2") _, out("d3") _, out("d4") _, out("d5") _,
                    out("d6") _, out("d7") _, out("d8") _, out("d9") _, out("d10") _, out("d11") _,
                    out("d12") _, out("d13") _, out("d14") _, out("d15") _, out("d16") _, out("d17") _,
                    out("d18") _, out("d19") _, out("d20") _, out("d21") _, out("d22") _, out("d23") _,
                    options(nostack),
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }
    fn arr(s: &str) -> [u8; 16] {
        hex(s).try_into().unwrap()
    }

    /// Entrambi i motori disponibili su questa CPU (su ARM a 32 bit con AES: hardware e software).
    fn engines(key: &[u8; 16]) -> Vec<Aes128> {
        let mut v = vec![Aes128::soft(key)];
        let best = Aes128::new(key);
        if best.arm_hardware() {
            v.push(best);
        }
        v
    }

    #[test]
    fn fips197_and_sp800_38a() {
        // FIPS-197 C.1
        for e in engines(&arr("000102030405060708090a0b0c0d0e0f")) {
            let mut b = [arr("00112233445566778899aabbccddeeff")];
            e.encrypt_blocks(&mut b);
            assert_eq!(b[0], arr("69c4e0d86a7b0430d8cdb78070b4c55a"));
            e.decrypt_blocks(&mut b);
            assert_eq!(b[0], arr("00112233445566778899aabbccddeeff"));
        }
        // SP 800-38A F.5.1 (CTR) e F.2.2 (CBC, decifratura)
        let key = arr("2b7e151628aed2a6abf7158809cf4f3c");
        let pt = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
        let ctr_ct = hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
        let cbc_ct = hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
        for e in engines(&key) {
            let hw = e.arm_hardware();
            let mut d = pt.clone();
            let mut c = Ctr::with(e, &arr("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"));
            c.apply(&mut d[..5]); // a pezzi: il keystream prosegue
            c.apply(&mut d[5..37]);
            c.apply(&mut d[37..]);
            assert_eq!(d, ctr_ct, "CTR (hardware: {hw})");
        }
        for e in engines(&key) {
            let mut d = cbc_ct.clone();
            d.extend_from_slice(b"coda"); // byte finali in chiaro
            Cbc::with(e, &arr("000102030405060708090a0b0c0d0e0f")).decrypt(&mut d);
            assert_eq!(&d[..64], &pt[..]);
            assert_eq!(&d[64..], b"coda");
        }
    }

    /// Su ARM a 32 bit il motore scelto deve seguire HWCAP2_AES (sulla TCL: hardware).
    #[cfg(target_arch = "arm")]
    #[test]
    fn arm_engine_follows_hwcap() {
        assert_eq!(Aes128::new(&[0; 16]).arm_hardware(), arm::available());
        eprintln!("AES hardware ARMv8 a 32 bit: {}", arm::available());
    }

    #[test]
    fn counter_wraps_like_mbedtls() {
        let key = [3u8; 16];
        let iv = arr("fffffffffffffffffffffffffffffffe");
        // contatori attesi: ...fe, ...ff, 00...00, 00...01
        let mut expected = [iv, arr("ffffffffffffffffffffffffffffffff"), [0; 16], arr("00000000000000000000000000000001")];
        Aes128::soft(&key).encrypt_blocks(&mut expected);
        let mut d = [0u8; 64];
        Ctr::new(&key, &iv).apply(&mut d);
        assert_eq!(&d[..], expected.as_flattened());
    }

    fn rng(s: &mut u64) -> u64 {
        *s ^= *s << 13;
        *s ^= *s >> 7;
        *s ^= *s << 17;
        *s
    }

    /// Motore scelto (hardware se c'è) contro RustCrypto: chiavi, IV e frammentazioni casuali.
    #[test]
    fn engines_agree() {
        let mut st = 0xC0FF_EE00_1234_5678u64;
        for _ in 0..2000 {
            let key: [u8; 16] = core::array::from_fn(|_| rng(&mut st) as u8);
            let iv: [u8; 16] = core::array::from_fn(|_| rng(&mut st) as u8);
            let len = (rng(&mut st) % 3000) as usize;
            let data: Vec<u8> = (0..len).map(|_| rng(&mut st) as u8).collect();
            let (mut a, mut b) = (data.clone(), data.clone());
            let (mut ca, mut cb) = (Ctr::new(&key, &iv), Ctr::with(Aes128::soft(&key), &iv));
            let mut pos = 0;
            while pos < len {
                let n = ((rng(&mut st) % 200) as usize).min(len - pos);
                ca.apply(&mut a[pos..pos + n]);
                cb.apply(&mut b[pos..pos + n]);
                pos += n;
            }
            assert_eq!(a, b);
            let (mut a, mut b) = (data.clone(), data);
            Cbc::new(&key, &iv).decrypt(&mut a);
            Cbc::with(Aes128::soft(&key), &iv).decrypt(&mut b);
            assert_eq!(a, b);
        }
    }

    #[test]
    fn skip_equals_apply() {
        let (key, iv) = ([9u8; 16], [1u8; 16]);
        let mut a = Ctr::new(&key, &iv);
        let mut b = Ctr::new(&key, &iv);
        let mut z = [0u8; 100];
        a.apply(&mut z[..3]);
        b.skip(3);
        let (mut x, mut y) = ([7u8; 70], [7u8; 70]);
        a.apply(&mut z[3..64]);
        b.skip(61);
        a.apply(&mut x);
        b.apply(&mut y);
        assert_eq!(x, y);
    }
}
