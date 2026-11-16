//! Componente Rust di iRPlay.
//!
//! Interfaccia verso il C++: solo funzioni `extern "C"` con tipi semplici, dichiarate in
//! `app/src/main/cpp/rust_core.h`. Il codice `unsafe` sta solo in questa interfaccia, e
//! nessun panic la attraversa (il profilo di compilazione usa `panic = "abort"`).

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

pub mod hevc;

/// Versione dell'interfaccia C. Il C++ la controlla all'avvio: se non coincide con quella
/// che si aspetta, la libreria Rust è di un'altra build e non va usata.
pub const ABI_VERSION: u32 = 2;

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
    let Some([vps, sps, pps]) = hevc::parse_ios_config(payload) else { return HEVC_CONFIG_INVALID };
    let Some(extra) = hevc::pps_extra_slice_header_bits(&pps) else { return HEVC_CONFIG_INVALID };
    let (new_vps, new_sps) = if low_latency { (hevc::rewrite_vps(&vps), hevc::rewrite_sps(&sps)) } else { (None, None) };
    let patched = new_vps.is_some() || new_sps.is_some();
    let mut csd = Vec::with_capacity(vps.len() + sps.len() + pps.len() + 12);
    for nal in [new_vps.as_deref().unwrap_or(&vps), new_sps.as_deref().unwrap_or(&sps), &pps[..]] {
        csd.extend_from_slice(&[0, 0, 0, 1]);
        csd.extend_from_slice(nal);
    }
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
    match hevc::frame_to_annexb(data, extra_bits) {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_stable() {
        assert_eq!(irplay_rust_abi_version(), 2);
    }
}
