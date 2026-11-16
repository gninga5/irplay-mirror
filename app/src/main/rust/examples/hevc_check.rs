//! Strumento dei test sul Mac (tools/hosttest/run_tests.sh): applica la parte HEVC del motore
//! a un flusso generato da gen_hevc.swift.
//! Scrive <prefisso>.p.vps/.p.sps (riscritti, o identici se non serve) e riassume fotogrammi
//! chiave e B-slice, confrontati con quanto dichiarato da VideoToolbox.
use irplay_core::hevc;
use std::fs;

fn main() {
    let prefix = std::env::args().nth(1).expect("uso: hevc_check <prefisso>");
    let read = |ext: &str| fs::read(format!("{prefix}.{ext}")).expect(ext);
    let (vps, sps, pps, frames) = (read("vps"), read("sps"), read("pps"), read("frames"));
    let new_vps = hevc::rewrite_vps(&vps);
    let new_sps = hevc::rewrite_sps(&sps);
    fs::write(format!("{prefix}.p.vps"), new_vps.as_deref().unwrap_or(&vps)).unwrap();
    fs::write(format!("{prefix}.p.sps"), new_sps.as_deref().unwrap_or(&sps)).unwrap();
    let extra = hevc::pps_extra_slice_header_bits(&pps).expect("PPS non valido");
    let (mut n, mut key_mismatch, mut b, mut bad) = (0, 0, 0, 0);
    let mut i = 0;
    while i + 5 <= frames.len() {
        let len = u32::from_le_bytes(frames[i..i + 4].try_into().unwrap()) as usize;
        let vt_key = frames[i + 4] == 1;
        let mut f = frames[i + 5..i + 5 + len].to_vec();
        match hevc::frame_to_annexb(&mut f, extra) {
            Some(info) => {
                key_mismatch += usize::from(info.keyframe != vt_key);
                b += usize::from(info.b_slice);
            }
            None => bad += 1,
        }
        n += 1;
        i += 5 + len;
    }
    println!(
        "{prefix}: VPS {} SPS {}, {n} fotogrammi, chiave diversi da VideoToolbox {key_mismatch}, con B-slice {b}, non validi {bad}",
        if new_vps.is_some() { "riscritto" } else { "invariato" },
        if new_sps.is_some() { "riscritto" } else { "invariato" }
    );
}
