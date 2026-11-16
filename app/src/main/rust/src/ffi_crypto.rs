//! Interfaccia C della crittografia (dichiarata in rust_core.h). Le chiavi e gli stati
//! restano in memoria di Rust dietro handle opachi; il C++ riceve solo i risultati
//! (chiavi pubbliche, messaggi cifrati) e libera ogni handle con la funzione `_free`.
//! Nessun panic esce di qui ((`guard` in lib.rs)).

use crate::aes128::{Cbc, Ctr};
use crate::guard;
use crate::pairing::{self, Identity, Pairing};
use subtle::ConstantTimeEq;
use zeroize::Zeroize;

/// # Safety
/// `p` nullo con len 0, oppure `len` byte leggibili.
unsafe fn input<'a>(p: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if p.is_null() {
        return None;
    }
    // SAFETY: contratto della funzione.
    Some(unsafe { std::slice::from_raw_parts(p, len) })
}

/// # Safety
/// `p` nullo con len 0, oppure `len` byte scrivibili ed esclusivi.
unsafe fn output<'a>(p: *mut u8, len: usize) -> Option<&'a mut [u8]> {
    if len == 0 {
        return Some(&mut []);
    }
    if p.is_null() {
        return None;
    }
    // SAFETY: contratto della funzione.
    Some(unsafe { std::slice::from_raw_parts_mut(p, len) })
}

/// # Safety
/// `p` nullo oppure N byte leggibili.
unsafe fn array<'a, const N: usize>(p: *const u8) -> Option<&'a [u8; N]> {
    // SAFETY: contratto della funzione.
    (!p.is_null()).then(|| unsafe { &*p.cast::<[u8; N]>() })
}

/// # Safety
/// `p` nullo oppure N byte scrivibili.
unsafe fn write<const N: usize>(p: *mut u8, v: &[u8; N]) -> bool {
    if p.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), p, N) };
    true
}

/// Intero casuale uniforme in [0, n).
///
/// # Safety
/// `out` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_random_below(n: u32, out: *mut u32) -> bool {
    if n == 0 || out.is_null() {
        return false;
    }
    let Some(v) = guard(None, || pairing::random_below(n)) else { return false };
    // SAFETY: scrivibile per contratto.
    unsafe { *out = v };
    true
}

/// Azzera memoria in modo che il compilatore non possa eliminarlo.
///
/// # Safety
/// `p` deve puntare a `len` byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_secure_zero(p: *mut u8, len: usize) {
    // SAFETY: contratto della funzione.
    if let Some(b) = unsafe { output(p, len) } {
        b.zeroize();
    }
}

/// Confronto a tempo costante di due buffer della stessa lunghezza.
///
/// # Safety
/// `a` e `b` devono puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_ct_equal(a: *const u8, b: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    match unsafe { (input(a, len), input(b, len)) } {
        (Some(x), Some(y)) => bool::from(x.ct_eq(y)),
        _ => false,
    }
}

/// true se AES usa le istruzioni ARMv8 a 32 bit (per il log).
#[unsafe(no_mangle)]
pub extern "C" fn irplay_aes_arm_hardware() -> bool {
    guard(false, || crate::aes128::Aes128::new(&[0; 16]).arm_hardware())
}

// ------------------------------------------------------------------------- identità

/// Identità Ed25519 dal seed (32 byte); null se il seed manca.
///
/// # Safety
/// `seed` deve puntare a 32 byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_identity_new(seed: *const u8) -> *mut Identity {
    // SAFETY: contratto della funzione.
    let Some(s) = (unsafe { array::<32>(seed) }) else { return std::ptr::null_mut() };
    guard(None, || Some(Box::new(Identity::from_seed(s)))).map_or(std::ptr::null_mut(), Box::into_raw)
}

/// Chiave pubblica (32 byte).
///
/// # Safety
/// `id` da [`irplay_identity_new`]; `out` 32 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_identity_public(id: *const Identity, out: *mut u8) -> bool {
    if id.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let id = unsafe { &*id };
    let Some(pk) = guard(None, || Some(id.public())) else { return false };
    // SAFETY: contratto della funzione.
    unsafe { write(out, &pk) }
}

/// Firma Ed25519 (64 byte).
///
/// # Safety
/// `id` da [`irplay_identity_new`]; `msg` `len` byte leggibili; `sig` 64 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_identity_sign(id: *const Identity, msg: *const u8, len: usize, sig: *mut u8) -> bool {
    if id.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (id, Some(m)) = (unsafe { &*id }, unsafe { input(msg, len) }) else { return false };
    let Some(s) = guard(None, || Some(id.sign(m))) else { return false };
    // SAFETY: contratto della funzione.
    unsafe { write(sig, &s) }
}

/// Libera un'identità (azzera il segreto).
///
/// # Safety
/// `id` da [`irplay_identity_new`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_identity_free(id: *mut Identity) {
    if !id.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(id) });
    }
}

// --------------------------------------------------------------------------- pairing

/// Nuovo stato di pairing per una connessione.
#[unsafe(no_mangle)]
pub extern "C" fn irplay_pairing_new() -> *mut Pairing {
    guard(None, || Some(Box::new(Pairing::new()))).map_or(std::ptr::null_mut(), Box::into_raw)
}

/// Libera lo stato di pairing (azzera i segreti).
///
/// # Safety
/// `p` da [`irplay_pairing_new`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_free(p: *mut Pairing) {
    if !p.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(p) });
    }
}

/// # Safety
/// `p` nullo oppure da [`irplay_pairing_new`], usato da un solo thread.
unsafe fn pairing<'a>(p: *mut Pairing) -> Option<&'a mut Pairing> {
    // SAFETY: contratto della funzione.
    (!p.is_null()).then(|| unsafe { &mut *p })
}

/// POST /pair-setup.
///
/// # Safety
/// `p` da [`irplay_pairing_new`]; `body` `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_setup(p: *mut Pairing, body: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    let (Some(p), Some(b)) = (unsafe { pairing(p) }, unsafe { input(body, len) }) else { return false };
    guard(false, || p.setup(b))
}

/// POST /pair-verify fase 1: `out` riceve 96 byte.
///
/// # Safety
/// `p` e `id` validi; `body` `len` byte leggibili; `out` 96 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_verify1(p: *mut Pairing, id: *const Identity, body: *const u8, len: usize, out: *mut u8) -> bool {
    if id.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (Some(p), id, Some(b)) = (unsafe { pairing(p) }, unsafe { &*id }, unsafe { input(body, len) }) else { return false };
    let Some(r) = guard(None, || p.verify_step1(id, b)) else { return false };
    // SAFETY: contratto della funzione.
    unsafe { write(out, &r) }
}

/// POST /pair-verify fase 2.
///
/// # Safety
/// `p` valido; `body` `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_verify2(p: *mut Pairing, body: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    let (Some(p), Some(b)) = (unsafe { pairing(p) }, unsafe { input(body, len) }) else { return false };
    guard(false, || p.verify_step2(b))
}

/// pair-verify completato.
///
/// # Safety
/// `p` valido.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_verified(p: *mut Pairing) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { pairing(p) }.is_some_and(|p| p.verified())
}

/// Chiave Ed25519 del client ricevuta in pair-verify (32 byte).
///
/// # Safety
/// `p` valido; `out` 32 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_client_ed_key(p: *mut Pairing, out: *mut u8) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { pairing(p) }.is_some_and(|p| unsafe { write(out, &p.client_ed_key()) })
}

/// Chiave Ed25519 del client autenticata con il PIN (32 byte).
///
/// # Safety
/// `p` valido; `out` 32 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_pin_client_key(p: *mut Pairing, out: *mut u8) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { pairing(p) }.is_some_and(|p| unsafe { write(out, &p.pin_client_key()) })
}

/// pair-setup-pin completato su questa connessione.
///
/// # Safety
/// `p` valido.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_pin_authenticated(p: *mut Pairing) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { pairing(p) }.is_some_and(|p| p.pin_authenticated())
}

/// Chiave di sessione K dalla chiave FairPlay; false senza pair-verify.
///
/// # Safety
/// `p` valido; `fp_key` 16 byte leggibili; `out` 16 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_session_key(p: *mut Pairing, fp_key: *const u8, out: *mut u8) -> bool {
    // SAFETY: contratto della funzione.
    let (Some(p), Some(fp)) = (unsafe { pairing(p) }, unsafe { array::<16>(fp_key) }) else { return false };
    let Some(mut k) = guard(None, || p.session_key(fp)) else { return false };
    // SAFETY: contratto della funzione.
    let ok = unsafe { write(out, &k) };
    k.zeroize();
    ok
}

/// pair-setup-pin fase 1: salt (16 byte) e B (`b_len` byte, al massimo `b_cap`).
///
/// # Safety
/// `p` valido; ingressi leggibili per le lunghezze date; `salt` 16 byte e `b` `b_cap`
/// byte scrivibili; `b_len` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_pin_begin(
    p: *mut Pairing,
    user: *const u8,
    user_len: usize,
    pin: *const u8,
    pin_len: usize,
    salt: *mut u8,
    b: *mut u8,
    b_cap: usize,
    b_len: *mut usize,
) -> bool {
    if b_len.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (Some(p), Some(u), Some(pn)) = (unsafe { pairing(p) }, unsafe { input(user, user_len) }, unsafe { input(pin, pin_len) }) else {
        return false;
    };
    let Some((s, big_b)) = guard(None, || p.pin_begin(u, pn)) else { return false };
    // SAFETY: contratto della funzione.
    let Some(bo) = (unsafe { output(b, b_cap) }) else { return false };
    if big_b.len() > bo.len() {
        return false;
    }
    bo[..big_b.len()].copy_from_slice(&big_b);
    // SAFETY: contratto della funzione.
    unsafe {
        *b_len = big_b.len();
        write(salt, &s)
    }
}

/// pair-setup-pin fase 2: prova del client -> prova della TV (20 byte).
///
/// # Safety
/// `p` valido; ingressi leggibili; `m2` 20 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_pin_proof(p: *mut Pairing, a: *const u8, a_len: usize, m1: *const u8, m1_len: usize, m2: *mut u8) -> bool {
    // SAFETY: contratto della funzione.
    let (Some(p), Some(a), Some(m1)) = (unsafe { pairing(p) }, unsafe { input(a, a_len) }, unsafe { input(m1, m1_len) }) else {
        return false;
    };
    let Some(r) = guard(None, || p.pin_proof(a, m1)) else { return false };
    // SAFETY: contratto della funzione.
    unsafe { write(m2, &r) }
}

/// pair-setup-pin fase 3: chiave del client decifrata e verificata; in uscita la nostra
/// cifrata (32 byte) e il tag (16 byte).
///
/// # Safety
/// `p` e `id` validi; ingressi leggibili; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_pairing_pin_exchange(
    p: *mut Pairing,
    id: *const Identity,
    epk: *const u8,
    epk_len: usize,
    tag: *const u8,
    tag_len: usize,
    out_epk: *mut u8,
    out_tag: *mut u8,
) -> bool {
    if id.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (Some(p), id, Some(e), Some(t)) = (unsafe { pairing(p) }, unsafe { &*id }, unsafe { input(epk, epk_len) }, unsafe { input(tag, tag_len) }) else {
        return false;
    };
    let Some((oe, ot)) = guard(None, || p.pin_exchange(id, e, t)) else { return false };
    // SAFETY: contratto della funzione.
    unsafe { write(out_epk, &oe) && write(out_tag, &ot) }
}

// ------------------------------------------------------------------- chiavi e flussi

/// Chiave e IV del flusso video.
///
/// # Safety
/// `session_key` 16 byte leggibili; `key` e `iv` 16 byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_derive_mirror_key(session_key: *const u8, stream_connection_id: u64, key: *mut u8, iv: *mut u8) -> bool {
    // SAFETY: contratto della funzione.
    let Some(sk) = (unsafe { array::<16>(session_key) }) else { return false };
    let Some((mut k, mut i)) = guard(None, || Some(pairing::derive_mirror_key(sk, stream_connection_id))) else { return false };
    // SAFETY: contratto della funzione.
    let ok = unsafe { write(key, &k) && write(iv, &i) };
    k.zeroize();
    i.zeroize();
    ok
}

/// Nuovo flusso AES-128-CTR (keystream continuo).
///
/// # Safety
/// `key` e `iv` 16 byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_ctr_new(key: *const u8, iv: *const u8) -> *mut Ctr {
    // SAFETY: contratto della funzione.
    let (Some(k), Some(i)) = (unsafe { array::<16>(key) }, unsafe { array::<16>(iv) }) else { return std::ptr::null_mut() };
    guard(None, || Some(Box::new(Ctr::new(k, i)))).map_or(std::ptr::null_mut(), Box::into_raw)
}

/// Cifra o decifra sul posto proseguendo il keystream.
///
/// # Safety
/// `h` da [`irplay_ctr_new`], usato da un solo thread; `data` `len` byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_ctr_apply(h: *mut Ctr, data: *mut u8, len: usize) -> bool {
    if h.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (h, Some(d)) = (unsafe { &mut *h }, unsafe { output(data, len) }) else { return false };
    guard(false, || {
        h.apply(d);
        true
    })
}

/// Consuma len byte di keystream.
///
/// # Safety
/// `h` da [`irplay_ctr_new`], usato da un solo thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_ctr_skip(h: *mut Ctr, len: usize) -> bool {
    if h.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let h = unsafe { &mut *h };
    guard(false, || {
        h.skip(len);
        true
    })
}

/// Libera un flusso CTR (azzera chiavi e contatore).
///
/// # Safety
/// `h` da [`irplay_ctr_new`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_ctr_free(h: *mut Ctr) {
    if !h.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(h) });
    }
}

/// Nuovo decifratore AES-128-CBC dell'audio.
///
/// # Safety
/// `key` e `iv` 16 byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_cbc_new(key: *const u8, iv: *const u8) -> *mut Cbc {
    // SAFETY: contratto della funzione.
    let (Some(k), Some(i)) = (unsafe { array::<16>(key) }, unsafe { array::<16>(iv) }) else { return std::ptr::null_mut() };
    guard(None, || Some(Box::new(Cbc::new(k, i)))).map_or(std::ptr::null_mut(), Box::into_raw)
}

/// Decifra sul posto i blocchi completi di un pacchetto (la coda resta com'è).
///
/// # Safety
/// `h` da [`irplay_cbc_new`]; `data` `len` byte scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_cbc_decrypt(h: *const Cbc, data: *mut u8, len: usize) -> bool {
    if h.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let (h, Some(d)) = (unsafe { &*h }, unsafe { output(data, len) }) else { return false };
    guard(false, || {
        h.decrypt(d);
        true
    })
}

/// Libera un decifratore CBC.
///
/// # Safety
/// `h` da [`irplay_cbc_new`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_cbc_free(h: *mut Cbc) {
    if !h.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(h) });
    }
}
