//! HEVC (H.265) nella duplicazione dello schermo.
//!
//! Con il bit 42 delle funzioni AirPlay iOS manda il video in HEVC: un pacchetto di
//! configurazione "hvc1" con VPS, SPS e PPS e poi fotogrammi con NAL a prefisso di lunghezza
//! (4 byte), come in H.264.
//!
//! Bassa latenza: come per H.264 (vedi `sps_rewriter.h`), un decoder che legge
//! `sps_max_num_reorder_pics > 0` trattiene fotogrammi in attesa di un riordino che il
//! mirroring non fa. Qui VPS e SPS vengono riscritti con zero fotogrammi da riordinare e
//! nessun limite di latenza; il resto dei parametri è copiato bit per bit. Se nel flusso
//! compaiono B-slice si torna ai parametri originali (`slice_is_b`).
//!
//! Tutto il testo arriva dalla rete: ogni lettura è controllata e ogni errore restituisce
//! `None`, mai un panic.

/// Tipi di NAL HEVC usati qui.
pub const NAL_VPS: u8 = 32;
/// Sequence parameter set.
pub const NAL_SPS: u8 = 33;
/// Picture parameter set.
pub const NAL_PPS: u8 = 34;

/// Tipo del NAL (bit 1-6 del primo byte dell'intestazione a 2 byte).
pub fn nal_type(nal: &[u8]) -> Option<u8> {
    nal.first().map(|b| (b >> 1) & 0x3F)
}

/// Immagini di accesso casuale (BLA, IDR, CRA): da qui il decoder può ripartire.
pub fn is_irap(nal_type: u8) -> bool {
    (16..=21).contains(&nal_type)
}

/// Toglie i byte 0x03 di "emulation prevention" (00 00 03 -> 00 00).
pub fn unescape(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len());
    let mut zeros = 0usize;
    for &b in data {
        if zeros >= 2 && b == 0x03 {
            zeros = 0;
            continue;
        }
        zeros = if b == 0 { zeros + 1 } else { 0 };
        out.push(b);
    }
    out
}

/// Reinserisce gli 0x03 dove due zeri sono seguiti da un byte <= 3.
pub fn escape(rbsp: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(rbsp.len() + rbsp.len() / 64 + 2);
    let mut zeros = 0usize;
    for &b in rbsp {
        if zeros >= 2 && b <= 3 {
            out.push(0x03);
            zeros = 0;
        }
        zeros = if b == 0 { zeros + 1 } else { 0 };
        out.push(b);
    }
    out
}

struct BitReader<'a> {
    data: &'a [u8],
    pos: usize, // in bit
    end: usize, // primo bit da non leggere (il bit di stop dell'RBSP)
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8], end: usize) -> Self {
        BitReader { data, pos: 0, end: end.min(data.len() * 8) }
    }

    fn bit(&mut self) -> Option<u32> {
        if self.pos >= self.end {
            return None;
        }
        let b = (self.data[self.pos / 8] >> (7 - self.pos % 8)) & 1;
        self.pos += 1;
        Some(u32::from(b))
    }

    fn bits(&mut self, n: u32) -> Option<u32> {
        debug_assert!(n <= 32);
        let mut v = 0u32;
        for _ in 0..n {
            v = (v << 1) | self.bit()?;
        }
        Some(v)
    }

    fn flag(&mut self) -> Option<bool> {
        self.bit().map(|b| b == 1)
    }

    /// Exp-Golomb senza segno; al massimo 31 zeri iniziali.
    fn ue(&mut self) -> Option<u32> {
        let mut zeros = 0u32;
        while self.bit()? == 0 {
            zeros += 1;
            if zeros > 31 {
                return None;
            }
        }
        let rest = self.bits(zeros)?;
        Some(((1u64 << zeros) - 1 + u64::from(rest)).min(u64::from(u32::MAX)) as u32)
    }
}

#[derive(Default)]
struct BitWriter {
    out: Vec<u8>,
    cur: u8,
    n: u8,
}

impl BitWriter {
    fn bit(&mut self, b: u32) {
        self.cur = (self.cur << 1) | (b & 1) as u8;
        self.n += 1;
        if self.n == 8 {
            self.out.push(self.cur);
            self.cur = 0;
            self.n = 0;
        }
    }

    fn bits(&mut self, v: u32, n: u32) {
        for i in (0..n).rev() {
            self.bit((v >> i) & 1);
        }
    }

    fn ue(&mut self, v: u32) {
        let x = u64::from(v) + 1;
        let len = 64 - x.leading_zeros();
        for _ in 0..len - 1 {
            self.bit(0);
        }
        for i in (0..len).rev() {
            self.bit(((x >> i) & 1) as u32);
        }
    }

    /// Bit di stop e zeri fino al byte successivo (rbsp_trailing_bits).
    fn finish(mut self) -> Vec<u8> {
        self.bit(1);
        while self.n != 0 {
            self.bit(0);
        }
        self.out
    }
}

/// Copia n bit dal lettore allo scrittore.
fn copy(r: &mut BitReader, w: &mut BitWriter, n: u32) -> Option<u32> {
    let mut last = 0;
    let mut left = n;
    while left > 0 {
        let k = left.min(32);
        last = r.bits(k)?;
        w.bits(last, k);
        left -= k;
    }
    Some(last)
}

fn copy_ue(r: &mut BitReader, w: &mut BitWriter) -> Option<u32> {
    let v = r.ue()?;
    w.ue(v);
    Some(v)
}

/// Posizione (in bit) dell'ultimo bit a 1: il bit di stop dell'RBSP.
fn stop_bit(rbsp: &[u8]) -> Option<usize> {
    let last = rbsp.iter().rposition(|&b| b != 0)?;
    let tz = rbsp[last].trailing_zeros() as usize;
    Some(last * 8 + (7 - tz))
}

/// profile_tier_level(1, max_sub_layers_minus1), copiato senza modifiche.
fn copy_profile_tier_level(r: &mut BitReader, w: &mut BitWriter, max_sub_layers_minus1: u32) -> Option<()> {
    copy(r, w, 96)?; // profilo generale (88 bit) + general_level_idc (8)
    let mut profile = [false; 8];
    let mut level = [false; 8];
    for i in 0..max_sub_layers_minus1 as usize {
        profile[i] = copy(r, w, 1)? == 1;
        level[i] = copy(r, w, 1)? == 1;
    }
    if max_sub_layers_minus1 > 0 {
        for _ in max_sub_layers_minus1..8 {
            copy(r, w, 2)?; // reserved_zero_2bits
        }
    }
    for i in 0..max_sub_layers_minus1 as usize {
        if profile[i] {
            copy(r, w, 88)?;
        }
        if level[i] {
            copy(r, w, 8)?;
        }
    }
    Some(())
}

/// Informazioni di ordinamento (dec_pic_buffering, num_reorder_pics, max_latency_increase)
/// riscritte con zero riordino e nessun limite di latenza. true se qualcosa è cambiato.
fn rewrite_ordering(r: &mut BitReader, w: &mut BitWriter, max_sub_layers_minus1: u32) -> Option<bool> {
    let present = copy(r, w, 1)? == 1;
    let first = if present { 0 } else { max_sub_layers_minus1 };
    let mut changed = false;
    for _ in first..=max_sub_layers_minus1 {
        copy_ue(r, w)?; // max_dec_pic_buffering_minus1: i riferimenti servono comunque
        let reorder = r.ue()?;
        let latency = r.ue()?;
        changed |= reorder != 0 || latency != 0;
        w.ue(0);
        w.ue(0);
    }
    Some(changed)
}

/// Copia i bit restanti fino al bit di stop e chiude l'RBSP.
fn finish(mut r: BitReader, mut w: BitWriter, header: &[u8]) -> Option<Vec<u8>> {
    let left = r.end.checked_sub(r.pos)?;
    copy(&mut r, &mut w, left as u32)?;
    let mut out = header.to_vec();
    out.extend(escape(&w.finish()));
    Some(out)
}

const MAX_PARAM_SET: usize = 4096;

/// VPS con zero fotogrammi da riordinare. None se non è un VPS valido o se non serve cambiarlo.
pub fn rewrite_vps(nal: &[u8]) -> Option<Vec<u8>> {
    if nal.len() < 3 || nal.len() > MAX_PARAM_SET || nal_type(nal)? != NAL_VPS {
        return None;
    }
    let rbsp = unescape(&nal[2..]);
    let mut r = BitReader::new(&rbsp, stop_bit(&rbsp)?);
    let mut w = BitWriter::default();
    copy(&mut r, &mut w, 4 + 1 + 1 + 6)?; // id, base_layer_internal, base_layer_available, max_layers_minus1
    let max_sub = copy(&mut r, &mut w, 3)?;
    if max_sub > 6 {
        return None;
    }
    copy(&mut r, &mut w, 1 + 16)?; // temporal_id_nesting, reserved_0xffff_16bits
    copy_profile_tier_level(&mut r, &mut w, max_sub)?;
    if !rewrite_ordering(&mut r, &mut w, max_sub)? {
        return None;
    }
    finish(r, w, &nal[..2])
}

/// SPS con zero fotogrammi da riordinare. None se non è un SPS valido o se non serve cambiarlo.
pub fn rewrite_sps(nal: &[u8]) -> Option<Vec<u8>> {
    if nal.len() < 3 || nal.len() > MAX_PARAM_SET || nal_type(nal)? != NAL_SPS {
        return None;
    }
    let rbsp = unescape(&nal[2..]);
    let mut r = BitReader::new(&rbsp, stop_bit(&rbsp)?);
    let mut w = BitWriter::default();
    copy(&mut r, &mut w, 4)?; // sps_video_parameter_set_id
    let max_sub = copy(&mut r, &mut w, 3)?;
    if max_sub > 6 {
        return None;
    }
    copy(&mut r, &mut w, 1)?; // temporal_id_nesting
    copy_profile_tier_level(&mut r, &mut w, max_sub)?;
    copy_ue(&mut r, &mut w)?; // sps_seq_parameter_set_id
    if copy_ue(&mut r, &mut w)? == 3 {
        copy(&mut r, &mut w, 1)?; // separate_colour_plane_flag
    }
    copy_ue(&mut r, &mut w)?; // pic_width_in_luma_samples
    copy_ue(&mut r, &mut w)?; // pic_height_in_luma_samples
    if copy(&mut r, &mut w, 1)? == 1 {
        for _ in 0..4 {
            copy_ue(&mut r, &mut w)?; // conformance window
        }
    }
    copy_ue(&mut r, &mut w)?; // bit_depth_luma_minus8
    copy_ue(&mut r, &mut w)?; // bit_depth_chroma_minus8
    copy_ue(&mut r, &mut w)?; // log2_max_pic_order_cnt_lsb_minus4
    if !rewrite_ordering(&mut r, &mut w, max_sub)? {
        return None;
    }
    finish(r, w, &nal[..2])
}

/// Dal PPS: num_extra_slice_header_bits, che serve per leggere il tipo di slice.
pub fn pps_extra_slice_header_bits(nal: &[u8]) -> Option<u8> {
    if nal.len() < 3 || nal.len() > MAX_PARAM_SET || nal_type(nal)? != NAL_PPS {
        return None;
    }
    let rbsp = unescape(&nal[2..]);
    let mut r = BitReader::new(&rbsp, stop_bit(&rbsp)?);
    r.ue()?; // pps_pic_parameter_set_id
    r.ue()?; // pps_seq_parameter_set_id
    r.flag()?; // dependent_slice_segments_enabled_flag
    r.flag()?; // output_flag_present_flag
    Some(r.bits(3)? as u8)
}

/// true se il NAL è la prima slice di un'immagine di tipo B; None se non è una prima slice
/// o se l'intestazione non si legge.
pub fn slice_is_b(nal: &[u8], extra_slice_header_bits: u8) -> Option<bool> {
    let t = nal_type(nal)?;
    if t > 21 || nal.len() < 3 {
        return None; // non è una slice video
    }
    let head = unescape(&nal[2..nal.len().min(2 + 32)]);
    let mut r = BitReader::new(&head, head.len() * 8);
    if !r.flag()? {
        return None; // first_slice_segment_in_pic_flag: le slice successive non servono
    }
    if (16..=23).contains(&t) {
        r.flag()?; // no_output_of_prior_pics_flag
    }
    r.ue()?; // slice_pic_parameter_set_id
    r.bits(u32::from(extra_slice_header_bits))?;
    Some(r.ue()? == 0) // slice_type: 0 = B, 1 = P, 2 = I
}

/// Esito della conversione di un fotogramma.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct FrameInfo {
    /// Contiene un'immagine di accesso casuale.
    pub keyframe: bool,
    /// Contiene una B-slice: il riordino serve davvero.
    pub b_slice: bool,
}

/// Prefissi di lunghezza a 4 byte -> start code Annex-B, sul posto. None se le lunghezze non
/// tornano (il fotogramma va scartato).
pub fn frame_to_annexb(buf: &mut [u8], extra_slice_header_bits: u8) -> Option<FrameInfo> {
    let mut info = FrameInfo::default();
    let mut i = 0usize;
    while i < buf.len() {
        let len = u32::from_be_bytes(buf.get(i..i + 4)?.try_into().ok()?) as usize;
        let start = i + 4;
        if len < 2 || len > buf.len() - start {
            return None;
        }
        buf[i..start].copy_from_slice(&[0, 0, 0, 1]);
        let nal = &buf[start..start + len];
        let t = nal_type(nal)?;
        info.keyframe |= is_irap(t);
        if slice_is_b(nal, extra_slice_header_bits) == Some(true) {
            info.b_slice = true;
        }
        i = start + len;
    }
    Some(info)
}

/// VPS, SPS e PPS dal pacchetto di configurazione "hvc1" di iOS: da 0x75 in poi, ciascuno
/// preceduto da 0xA0/0xA1/0xA2, 00 01 00 e due byte di lunghezza (formato visto in UxPlay).
pub fn parse_ios_config(payload: &[u8]) -> Option<[Vec<u8>; 3]> {
    if payload.get(4..8)? != b"hvc1" {
        return None;
    }
    let mut off = 0x75usize;
    let mut out: [Vec<u8>; 3] = Default::default();
    for (k, marker) in [0xA0u8, 0xA1, 0xA2].into_iter().enumerate() {
        let head = payload.get(off..off + 5)?;
        if head[0] != marker || head[1] != 0 || head[2] != 1 || head[3] != 0 {
            return None;
        }
        let n = usize::from(head[4]);
        off += 5;
        let nal = payload.get(off..off + n)?;
        if n < 3 || nal_type(nal)? != NAL_VPS + k as u8 {
            return None;
        }
        out[k] = nal.to_vec();
        off += n;
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    // Parametri reali generati con VideoToolbox (tools/hosttest/gen_hevc.swift).
    const LOWLAT_VPS: &str = "40010c01ffff016000000300b00000030000030078170240";
    const LOWLAT_SPS: &str = "420101016000000300b00000030000030078a003c0801107cb8817b916452ffcb9fc4fe880";
    const LOWLAT_PPS: &str = "4401c072f45364";
    const REORDER_VPS: &str = "40010c01ffff016000000300b0000003000003007815c090";
    const REORDER_SPS: &str = "420101016000000300b00000030000030078a003c0801107cb8815ee459510";
    const UHD_SPS: &str = "420101016000000300b00000030000030096a001e020021c58817b916452ffcb9fc4fe88";

    /// Legge dall'SPS i campi che contano: dimensioni e (dec_pic_buffering, reorder, latency).
    fn sps_fields(nal: &[u8]) -> (u32, u32, Vec<(u32, u32, u32)>) {
        let rbsp = unescape(&nal[2..]);
        let mut r = BitReader::new(&rbsp, stop_bit(&rbsp).unwrap());
        r.bits(4).unwrap();
        let max_sub = r.bits(3).unwrap();
        r.bits(1).unwrap();
        let mut w = BitWriter::default();
        copy_profile_tier_level(&mut r, &mut w, max_sub).unwrap();
        r.ue().unwrap();
        assert_ne!(r.ue().unwrap(), 3);
        let (wd, ht) = (r.ue().unwrap(), r.ue().unwrap());
        if r.flag().unwrap() {
            for _ in 0..4 {
                r.ue().unwrap();
            }
        }
        for _ in 0..3 {
            r.ue().unwrap();
        }
        let present = r.flag().unwrap();
        let first = if present { 0 } else { max_sub };
        let ord = (first..=max_sub).map(|_| (r.ue().unwrap(), r.ue().unwrap(), r.ue().unwrap())).collect();
        (wd, ht, ord)
    }

    #[test]
    fn escape_roundtrip() {
        let mut seed = 0x1234_5678u32;
        for _ in 0..2000 {
            let len = (seed % 64) as usize;
            let data: Vec<u8> = (0..len)
                .map(|_| {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    [0u8, 0, 0, 1, 2, 3, 0x80, 0xFF][(seed % 8) as usize]
                })
                .collect();
            let esc = escape(&data);
            assert!(!esc.windows(3).any(|w| w[0] == 0 && w[1] == 0 && w[2] <= 2));
            assert_eq!(unescape(&esc), data);
        }
    }

    #[test]
    fn ue_roundtrip() {
        let values = [0u32, 1, 2, 3, 7, 8, 255, 256, 65535, 1 << 20, u32::MAX - 1];
        let mut w = BitWriter::default();
        for &v in &values {
            w.ue(v);
        }
        let data = w.finish();
        let mut r = BitReader::new(&data, stop_bit(&data).unwrap());
        for &v in &values {
            assert_eq!(r.ue(), Some(v));
        }
    }

    #[test]
    fn sps_rewrite_keeps_everything_but_reordering() {
        // 1080 righe sono codificate come 1088 e poi ritagliate (conformance window)
        for (orig, w, h) in [(LOWLAT_SPS, 1920, 1088), (REORDER_SPS, 1920, 1088), (UHD_SPS, 3840, 2160)] {
            let sps = hex(orig);
            let (w0, h0, ord0) = sps_fields(&sps);
            assert_eq!((w0, h0), (w, h));
            match rewrite_sps(&sps) {
                Some(patched) => {
                    let (w1, h1, ord1) = sps_fields(&patched);
                    assert_eq!((w1, h1), (w0, h0));
                    assert_eq!(ord1.len(), ord0.len());
                    for (a, b) in ord0.iter().zip(&ord1) {
                        assert_eq!(b.0, a.0); // riferimenti invariati
                        assert_eq!((b.1, b.2), (0, 0));
                    }
                    // seconda riscrittura: niente da cambiare
                    assert!(rewrite_sps(&patched).is_none());
                }
                None => assert!(ord0.iter().all(|o| o.1 == 0 && o.2 == 0)),
            }
        }
        // con il riordino attivo l'encoder dichiara fotogrammi da riordinare: va riscritto
        let (_, _, ord) = sps_fields(&hex(REORDER_SPS));
        assert!(ord.iter().any(|o| o.1 > 0));
        assert!(rewrite_sps(&hex(REORDER_SPS)).is_some());
    }

    #[test]
    fn vps_rewrite_is_consistent() {
        for v in [LOWLAT_VPS, REORDER_VPS] {
            let vps = hex(v);
            if let Some(p) = rewrite_vps(&vps) {
                assert_eq!(nal_type(&p), Some(NAL_VPS));
                assert!(rewrite_vps(&p).is_none());
            }
        }
    }

    #[test]
    fn pps_extra_bits() {
        assert_eq!(pps_extra_slice_header_bits(&hex(LOWLAT_PPS)), Some(0));
        assert_eq!(pps_extra_slice_header_bits(&hex(LOWLAT_SPS)), None);
    }

    #[test]
    fn ios_config() {
        let (vps, sps, pps) = (hex(LOWLAT_VPS), hex(LOWLAT_SPS), hex(LOWLAT_PPS));
        let mut p = vec![0u8; 0x75];
        p[4..8].copy_from_slice(b"hvc1");
        for (m, nal) in [(0xA0u8, &vps), (0xA1, &sps), (0xA2, &pps)] {
            p.extend([m, 0, 1, 0, nal.len() as u8]);
            p.extend(nal.iter());
        }
        let got = parse_ios_config(&p).unwrap();
        assert_eq!(got, [vps.clone(), sps.clone(), pps.clone()]);
        // troncato o con marcatori sbagliati: rifiutato
        for cut in 0..p.len() {
            let _ = parse_ios_config(&p[..cut]);
        }
        let mut bad = p.clone();
        bad[0x75] = 0xA1;
        assert!(parse_ios_config(&bad).is_none());
    }

    #[test]
    fn frames_and_slices() {
        // due NAL: un VPS e una slice IDR minima
        let mut buf = vec![0, 0, 0, 3, 0x40, 0x01, 0x0C, 0, 0, 0, 4, 0x26, 0x01, 0xAF, 0x80];
        let info = frame_to_annexb(&mut buf, 0).unwrap();
        assert!(info.keyframe);
        assert_eq!(&buf[..4], &[0, 0, 0, 1]);
        let mut short = vec![0, 0, 0, 9, 0x26, 0x01];
        assert!(frame_to_annexb(&mut short, 0).is_none());
    }

    /// Nessun panic con dati alterati o casuali.
    #[test]
    fn fuzz_no_panic() {
        let seeds = [hex(LOWLAT_VPS), hex(LOWLAT_SPS), hex(LOWLAT_PPS), hex(REORDER_SPS), hex(UHD_SPS)];
        let mut x = 0x9E37_79B9u32;
        let mut rnd = || {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            x
        };
        for _ in 0..200_000 {
            let mut m = seeds[(rnd() % seeds.len() as u32) as usize].clone();
            for _ in 0..1 + rnd() % 4 {
                let i = (rnd() as usize) % m.len();
                m[i] = rnd() as u8;
            }
            if rnd() % 6 == 0 {
                let n = (rnd() as usize) % m.len();
                m.truncate(n);
            }
            let _ = rewrite_sps(&m);
            let _ = rewrite_vps(&m);
            let _ = pps_extra_slice_header_bits(&m);
            let _ = slice_is_b(&m, (rnd() % 8) as u8);
            let mut f = m.clone();
            let _ = frame_to_annexb(&mut f, 0);
        }
    }
}
