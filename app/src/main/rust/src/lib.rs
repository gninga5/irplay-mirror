//! Componente Rust di iRPlay.
//!
//! Interfaccia verso il C++: solo funzioni `extern "C"` con tipi semplici, dichiarate in
//! `app/src/main/cpp/rust_core.h`. Il codice `unsafe` sta solo in questa interfaccia.
//!
//! Nessun panic la attraversa: ogni funzione esportata lo intercetta con [`guard`] e lo
//! trasforma in un errore. Un pacchetto malevolo che facesse cadere in errore una libreria
//! fa perdere quel pacchetto, non chiude l'app (e Rust chiuderebbe comunque il processo,
//! invece di proseguire in uno stato indefinito, se un panic provasse a uscire nel C++).

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

pub mod aes128;
pub mod alac;
pub mod dmap;
pub mod ffi;
pub mod ffi_crypto;
pub mod hevc;
pub mod hls;
pub mod pairing;
pub mod plist;
pub mod rtsp;
pub mod srp;
pub mod text;

use std::panic::{AssertUnwindSafe, catch_unwind};

/// Versione dell'interfaccia C. Il C++ la controlla all'avvio: se non coincide con quella
/// che si aspetta, la libreria Rust è di un'altra build e non va usata.
pub const ABI_VERSION: u32 = 5;

/// Esegue `f` intercettando un eventuale panic: in quel caso restituisce `fallback`.
pub(crate) fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

/// Versione dell'interfaccia C esposta da questa libreria (vedi [`ABI_VERSION`]).
#[unsafe(no_mangle)]
pub extern "C" fn irplay_rust_abi_version() -> u32 {
    ABI_VERSION
}

/// Esito di [`irplay_hevc_config`]: configurazione non valida.
pub const HEVC_CONFIG_INVALID: i32 = 0;
/// Esito di [`irplay_hevc_config`]: parametri validi, copiati senza modifiche.
pub const HEVC_CONFIG_UNCHANGED: i32 = 1;
/// Esito di [`irplay_hevc_config`]: VPS e/o SPS riscritti per non riordinare fotogrammi.
pub const HEVC_CONFIG_LOW_LATENCY: i32 = 2;

const MAX_HEVC_CONFIG: usize = 64 * 1024;

/// Pacchetto di configurazione "hvc1" di iOS -> VPS, SPS e PPS in Annex-B (con start code),
/// da usare come csd-0 del decoder. Con `low_latency` VPS e SPS sono riscritti per non
/// riordinare fotogrammi (vedi [`hevc`]). `extra_bits` riceve num_extra_slice_header_bits
/// del PPS, da passare a [`irplay_hevc_frame`].
///
/// # Safety
/// `payload` deve puntare a `len` byte leggibili, `out` a `out_cap` byte scrivibili,
/// `out_len` ed `extra_bits` a variabili scrivibili. Nessun puntatore è conservato.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hevc_config(
    payload: *const u8,
    len: usize,
    low_latency: bool,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    extra_bits: *mut u8,
) -> i32 {
    if payload.is_null() || out.is_null() || out_len.is_null() || extra_bits.is_null() || len > MAX_HEVC_CONFIG {
        return HEVC_CONFIG_INVALID;
    }
    // SAFETY: puntatore non nullo e lunghezza garantita dal chiamante (vedi "Safety").
    let payload = unsafe { std::slice::from_raw_parts(payload, len) };
    let Some((csd, extra, patched)) = guard(None, || hevc_csd(payload, low_latency)) else { return HEVC_CONFIG_INVALID };
    if csd.len() > out_cap {
        return HEVC_CONFIG_INVALID;
    }
    // SAFETY: `out` ha `out_cap` >= csd.len() byte scrivibili; le aree non si sovrappongono
    // (csd è memoria nostra). `out_len` ed `extra_bits` sono scrivibili.
    unsafe {
        std::ptr::copy_nonoverlapping(csd.as_ptr(), out, csd.len());
        *out_len = csd.len();
        *extra_bits = extra;
    }
    if patched { HEVC_CONFIG_LOW_LATENCY } else { HEVC_CONFIG_UNCHANGED }
}

/// csd-0 HEVC in Annex-B, num_extra_slice_header_bits, e se VPS/SPS sono stati riscritti.
fn hevc_csd(payload: &[u8], low_latency: bool) -> Option<(Vec<u8>, u8, bool)> {
    let [vps, sps, pps] = hevc::parse_ios_config(payload)?;
    let extra = hevc::pps_extra_slice_header_bits(&pps)?;
    let (new_vps, new_sps) = if low_latency { (hevc::rewrite_vps(&vps), hevc::rewrite_sps(&sps)) } else { (None, None) };
    let patched = new_vps.is_some() || new_sps.is_some();
    let mut csd = Vec::with_capacity(vps.len() + sps.len() + pps.len() + 12);
    for nal in [new_vps.as_deref().unwrap_or(&vps), new_sps.as_deref().unwrap_or(&sps), &pps[..]] {
        csd.extend_from_slice(&[0, 0, 0, 1]);
        csd.extend_from_slice(nal);
    }
    Some((csd, extra, patched))
}

/// Fotogramma HEVC (NAL a prefisso di lunghezza di 4 byte) convertito sul posto in Annex-B.
/// false se le lunghezze non tornano: il fotogramma va scartato.
///
/// # Safety
/// `buf` deve puntare a `len` byte scrivibili; `keyframe` e `b_slice` a variabili scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hevc_frame(buf: *mut u8, len: usize, extra_bits: u8, keyframe: *mut bool, b_slice: *mut bool) -> bool {
    if buf.is_null() || keyframe.is_null() || b_slice.is_null() || len == 0 {
        return false;
    }
    // SAFETY: puntatore non nullo e lunghezza garantita dal chiamante; accesso esclusivo
    // per la durata della chiamata.
    let data = unsafe { std::slice::from_raw_parts_mut(buf, len) };
    match guard(None, || hevc::frame_to_annexb(data, extra_bits)) {
        Some(info) => {
            // SAFETY: variabili scrivibili per contratto.
            unsafe {
                *keyframe = info.keyframe;
                *b_slice = info.b_slice;
            }
            true
        }
        None => false,
    }
}

/// Decoder ALAC per il C++ (vedi [`alac`]). Se un panic lo interrompe, lo stato interno
/// potrebbe essere incoerente: si ricrea al pacchetto successivo.
pub struct AlacHandle {
    dec: Option<alac::Alac>,
}

/// Nuovo decoder ALAC per AirPlay (352 campioni, 16 bit, stereo, 44,1 kHz). Null se non
/// disponibile. Va liberato con [`irplay_alac_free`].
#[unsafe(no_mangle)]
pub extern "C" fn irplay_alac_new() -> *mut AlacHandle {
    match guard(None, alac::Alac::new) {
        Some(dec) => Box::into_raw(Box::new(AlacHandle { dec: Some(dec) })),
        None => std::ptr::null_mut(),
    }
}

/// Decodifica un pacchetto ALAC in PCM 16 bit stereo interleaved (`out_cap` campioni i16
/// disponibili, almeno 704). In `out_frames` i campioni per canale. false se il pacchetto
/// non è valido.
///
/// # Safety
/// `handle` deve venire da [`irplay_alac_new`] e non essere usato da altri thread durante
/// la chiamata; `input` deve puntare a `len` byte leggibili, `out` a `out_cap` i16
/// scrivibili, `out_frames` a una variabile scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_alac_decode(
    handle: *mut AlacHandle,
    input: *const u8,
    len: usize,
    out: *mut i16,
    out_cap: usize,
    out_frames: *mut usize,
) -> bool {
    const SAMPLES: usize = alac::FRAME * alac::CHANNELS;
    if handle.is_null() || input.is_null() || out.is_null() || out_frames.is_null() || out_cap < SAMPLES || len == 0 || len > alac::MAX_PACKET {
        return false;
    }
    // SAFETY: puntatori validi ed esclusivi per contratto (vedi "Safety").
    let (h, input) = unsafe { (&mut *handle, std::slice::from_raw_parts(input, len)) };
    let mut pcm = [0i16; SAMPLES];
    let res = catch_unwind(AssertUnwindSafe(|| {
        if h.dec.is_none() {
            h.dec = alac::Alac::new();
        }
        h.dec.as_mut().and_then(|d| d.decode(input, &mut pcm))
    }));
    let n = match res {
        Ok(Some(n)) => n,
        Ok(None) => return false,
        Err(_) => {
            h.dec = None;
            return false;
        }
    };
    // SAFETY: `out` ha almeno SAMPLES >= n * CHANNELS i16 scrivibili; `out_frames` scrivibile.
    unsafe {
        std::ptr::copy_nonoverlapping(pcm.as_ptr(), out, n * alac::CHANNELS);
        *out_frames = n;
    }
    true
}

/// Libera un decoder creato da [`irplay_alac_new`] (null ammesso).
///
/// # Safety
/// `handle` deve venire da [`irplay_alac_new`] e non essere più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_alac_free(handle: *mut AlacHandle) {
    if !handle.is_null() {
        // SAFETY: creato con Box::into_raw in irplay_alac_new, liberato una volta sola.
        drop(unsafe { Box::from_raw(handle) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_stable() {
        assert_eq!(irplay_rust_abi_version(), 5);
    }

    #[test]
    fn guard_turns_panic_into_fallback() {
        std::panic::set_hook(Box::new(|_| {}));
        assert_eq!(guard(7, || -> i32 { panic!("prova") }), 7);
        let _ = std::panic::take_hook();
        assert_eq!(guard(7, || 1), 1);
    }

    #[test]
    fn alac_ffi_rejects_bad_arguments() {
        let h = irplay_alac_new();
        assert!(!h.is_null());
        let mut out = [0i16; 704];
        let mut n = 0usize;
        let pk = [0u8; 16];
        unsafe {
            assert!(!irplay_alac_decode(std::ptr::null_mut(), pk.as_ptr(), pk.len(), out.as_mut_ptr(), out.len(), &mut n));
            assert!(!irplay_alac_decode(h, pk.as_ptr(), pk.len(), out.as_mut_ptr(), 703, &mut n));
            assert!(!irplay_alac_decode(h, pk.as_ptr(), 0, out.as_mut_ptr(), out.len(), &mut n));
            irplay_alac_free(h);
            irplay_alac_free(std::ptr::null_mut());
        }
    }
}
