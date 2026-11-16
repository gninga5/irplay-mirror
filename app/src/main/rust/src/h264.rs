//! H.264 del mirroring iOS (ITU-T H.264 Annex B e §7.3.2.1, ISO/IEC 14496-15).
//!
//! - [`avcc_to_annexb`]: i NAL arrivano con lunghezze di 4 byte (AVCC); i decoder di MediaTek,
//!   Realtek e Amlogic vogliono gli start code (Annex-B). Conversione sul posto, senza copie.
//! - [`parse_avcc`]: SPS e PPS dal record avcC (pacchetto di tipo 1 del flusso video).
//! - [`patch_sps_low_latency`]: l'SPS di iOS non ha bitstream_restriction, quindi il decoder
//!   suppone fotogrammi fuori ordine e ne trattiene diversi (fino a 18 sul MediaTek della
//!   Sony). Si dichiara max_num_reorder_frames = 0, come fa Moonlight.
//! - [`slice_is_b`]: se compare uno slice B la riscrittura va annullata.
//!
//! Porting esatto della versione C++ precedente (verificato con il confronto in
//! tools/hosttest/parser_diff.cpp): ogni lettura è controllata, un SPS anomalo fa fallire la
//! riscrittura, mai la sessione.

fn unescape(p: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(p.len());
    let mut zeros = 0;
    for &b in p {
        if zeros >= 2 && b == 0x03 {
            zeros = 0;
            continue;
        }
        out.push(b);
        zeros = if b == 0 { zeros + 1 } else { 0 };
    }
    out
}

fn escape(rbsp: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(rbsp.len() + 8);
    let mut zeros = 0;
    for &b in rbsp {
        if zeros >= 2 && b <= 3 {
            out.push(0x03);
            zeros = 0;
        }
        out.push(b);
        zeros = if b == 0 { zeros + 1 } else { 0 };
    }
    out
}

struct Reader<'a> {
    d: &'a [u8],
    pos: usize,
    ok: bool,
}

impl Reader<'_> {
    fn bit_at(&self, p: usize) -> u32 {
        u32::from((self.d[p / 8] >> (7 - p % 8)) & 1)
    }
    fn bit(&mut self) -> u32 {
        if self.pos >= self.d.len() * 8 {
            self.ok = false;
            return 0;
        }
        let b = self.bit_at(self.pos);
        self.pos += 1;
        b
    }
    fn u(&mut self, n: u32) -> u32 {
        let mut v = 0u32;
        for _ in 0..n {
            v = (v << 1) | self.bit();
        }
        v
    }
    fn ue(&mut self) -> u32 {
        let mut zeros = 0;
        while self.ok && self.bit() == 0 {
            zeros += 1;
            if zeros > 31 {
                self.ok = false;
                return 0;
            }
        }
        if zeros == 0 {
            return 0;
        }
        ((1u32 << zeros) - 1).wrapping_add(self.u(zeros))
    }
    fn se(&mut self) -> i32 {
        let k = self.ue();
        if k & 1 == 1 { (k.div_ceil(2)) as i32 } else { -((k / 2) as i32) }
    }
}

struct Writer {
    out: Vec<u8>,
    n: usize,
}

impl Writer {
    fn bit(&mut self, b: u32) {
        if self.n.is_multiple_of(8) {
            self.out.push(0);
        }
        if b != 0 {
            *self.out.last_mut().expect("byte appena aggiunto") |= 0x80 >> (self.n % 8);
        }
        self.n += 1;
    }
    fn u(&mut self, n: u32, v: u32) {
        for i in (0..n).rev() {
            self.bit((v >> i) & 1);
        }
    }
    fn ue(&mut self, v: u32) {
        let x = u64::from(v) + 1;
        let len = 63 - x.leading_zeros();
        for _ in 0..len {
            self.bit(0);
        }
        for i in (0..=len).rev() {
            self.bit(((x >> i) & 1) as u32);
        }
    }
    fn copy(&mut self, r: &Reader<'_>, count: usize) {
        for i in 0..count {
            self.bit(r.bit_at(i));
        }
    }
    fn trailing(&mut self) {
        self.bit(1);
        while !self.n.is_multiple_of(8) {
            self.bit(0);
        }
    }
}

fn skip_scaling_list(r: &mut Reader<'_>, size: usize) {
    let (mut last, mut next) = (8i32, 8i32);
    for _ in 0..size {
        if !r.ok {
            break;
        }
        if next != 0 {
            next = last.wrapping_add(r.se()).wrapping_add(256) % 256;
        }
        last = if next == 0 { last } else { next };
    }
}

fn skip_hrd(r: &mut Reader<'_>) {
    let cpb_cnt = r.ue().wrapping_add(1);
    if cpb_cnt > 32 {
        r.u(32); // fuori specifica: si leggono 64 bit, il fallimento arriva più avanti
        r.u(32);
        return;
    }
    r.u(4);
    r.u(4);
    for _ in 0..cpb_cnt {
        if !r.ok {
            break;
        }
        r.ue();
        r.ue();
        r.u(1);
    }
    r.u(20); // quattro lunghezze da 5 bit
}

fn write_restriction(w: &mut Writer, max_ref_frames: u32) {
    w.u(1, 1); // motion_vectors_over_pic_boundaries_flag
    w.ue(2); // max_bytes_per_pic_denom
    w.ue(1); // max_bits_per_mb_denom
    w.ue(16); // log2_max_mv_length_horizontal
    w.ue(16); // log2_max_mv_length_vertical
    w.ue(0); // max_num_reorder_frames: nessun riordino
    w.ue(if max_ref_frames > 0 { max_ref_frames } else { 1 }); // max_dec_frame_buffering
}

const HIGH_PROFILES: [u32; 13] = [100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135];

/// SPS Annex-B (start code + NAL tipo 7) -> SPS con max_num_reorder_frames = 0. None se non
/// interpretabile o se dichiara già 0 fotogrammi di riordino.
pub fn patch_sps_low_latency(input: &[u8]) -> Option<Vec<u8>> {
    if input.len() < 8 || input[..4] != [0, 0, 0, 1] {
        return None;
    }
    let header = input[4];
    if header & 0x1F != 7 {
        return None;
    }
    let rbsp = unescape(&input[5..]);
    let mut r = Reader { d: &rbsp, pos: 0, ok: true };

    let profile = r.u(8);
    r.u(8); // constraint flags
    r.u(8); // level_idc
    r.ue(); // seq_parameter_set_id
    if HIGH_PROFILES.contains(&profile) {
        let chroma = r.ue();
        if chroma == 3 {
            r.u(1);
        }
        r.ue();
        r.ue();
        r.u(1);
        if r.u(1) == 1 {
            // seq_scaling_matrix_present_flag
            let lists = if chroma == 3 { 12 } else { 8 };
            for i in 0..lists {
                if !r.ok {
                    break;
                }
                if r.u(1) == 1 {
                    skip_scaling_list(&mut r, if i < 6 { 16 } else { 64 });
                }
            }
        }
    }
    r.ue(); // log2_max_frame_num_minus4
    let poc_type = r.ue();
    if poc_type == 0 {
        r.ue();
    } else if poc_type == 1 {
        r.u(1);
        r.se();
        r.se();
        let cycle = r.ue();
        if cycle > 255 {
            return None;
        }
        for _ in 0..cycle {
            if !r.ok {
                break;
            }
            r.se();
        }
    }
    let max_ref_frames = r.ue();
    r.u(1); // gaps_in_frame_num_value_allowed_flag
    r.ue(); // pic_width_in_mbs_minus1
    r.ue(); // pic_height_in_map_units_minus1
    if r.u(1) == 0 {
        r.u(1); // mb_adaptive_frame_field_flag
    }
    r.u(1); // direct_8x8_inference_flag
    if r.u(1) == 1 {
        // frame cropping
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    if !r.ok || max_ref_frames > 16 {
        return None;
    }

    let mut w = Writer { out: Vec::new(), n: 0 };
    let vui_flag_pos = r.pos;
    if r.u(1) == 0 {
        // Nessuna VUI: se ne aggiunge una minima con la sola bitstream_restriction.
        w.copy(&r, vui_flag_pos);
        w.u(1, 1); // vui_parameters_present_flag
        for _ in 0..8 {
            w.u(1, 0); // aspect, overscan, signal, chroma_loc, timing, nal_hrd, vcl_hrd, pic_struct
        }
        w.u(1, 1); // bitstream_restriction_flag
        write_restriction(&mut w, max_ref_frames);
    } else {
        if r.u(1) == 1 && r.u(8) == 255 {
            r.u(16);
            r.u(16);
        }
        if r.u(1) == 1 {
            r.u(1); // overscan
        }
        if r.u(1) == 1 {
            r.u(3);
            r.u(1);
            if r.u(1) == 1 {
                r.u(24);
            }
        }
        if r.u(1) == 1 {
            r.ue();
            r.ue();
        }
        if r.u(1) == 1 {
            r.u(32);
            r.u(32);
            r.u(1);
        }
        let nal_hrd = r.u(1) == 1;
        if nal_hrd {
            skip_hrd(&mut r);
        }
        let vcl_hrd = r.u(1) == 1;
        if vcl_hrd {
            skip_hrd(&mut r);
        }
        if nal_hrd || vcl_hrd {
            r.u(1); // low_delay_hrd_flag
        }
        r.u(1); // pic_struct_present_flag
        if !r.ok {
            return None;
        }
        let restriction_pos = r.pos;
        if r.u(1) == 1 {
            r.u(1);
            r.ue();
            r.ue();
            r.ue();
            r.ue();
            let reorder = r.ue();
            r.ue();
            if !r.ok || reorder == 0 {
                return None; // già a bassa latenza
            }
        }
        w.copy(&r, restriction_pos);
        w.u(1, 1);
        write_restriction(&mut w, max_ref_frames);
    }
    if !r.ok {
        return None;
    }
    w.trailing();
    let mut out = vec![0, 0, 0, 1, header];
    out.extend_from_slice(&escape(&w.out));
    Some(out)
}

/// NAL senza start code (header incluso): true se è uno slice di tipo B.
pub fn slice_is_b(nal: &[u8]) -> bool {
    if nal.len() < 2 {
        return false;
    }
    let t = nal[0] & 0x1F;
    if t != 1 && t != 5 {
        return false;
    }
    let rbsp = unescape(&nal[1..nal.len().min(17)]);
    let mut r = Reader { d: &rbsp, pos: 0, ok: true };
    r.ue(); // first_mb_in_slice
    let slice_type = r.ue();
    r.ok && slice_type % 5 == 1
}

/// Esito di [`avcc_to_annexb`].
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Frame {
    /// Contiene un'unità IDR (tipo 5).
    pub keyframe: bool,
    /// Contiene uno slice B (solo se richiesto).
    pub b_slice: bool,
}

/// AVCC -> Annex-B sul posto. None se le lunghezze non tornano (frame corrotto o chiave
/// sbagliata): il buffer va scartato.
pub fn avcc_to_annexb(data: &mut [u8], check_b: bool) -> Option<Frame> {
    let mut f = Frame::default();
    let len = data.len();
    let mut pos = 0;
    while pos < len {
        if len - pos < 5 {
            return None; // lunghezza + almeno l'header NAL
        }
        let n = u32::from_be_bytes([data[pos], data[pos + 1], data[pos + 2], data[pos + 3]]) as usize;
        if n == 0 || n > len - pos - 4 {
            return None;
        }
        let header = data[pos + 4];
        if header & 0x80 != 0 {
            return None; // forbidden_zero_bit: con dati decifrati bene è sempre 0
        }
        if header & 0x1F == 5 {
            f.keyframe = true;
        }
        if check_b && !f.b_slice && slice_is_b(&data[pos + 4..pos + 4 + n]) {
            f.b_slice = true;
        }
        data[pos..pos + 4].copy_from_slice(&[0, 0, 0, 1]);
        pos += 4 + n;
    }
    (len > 0).then_some(f)
}

/// SPS e PPS (con start code) dal record avcC; si prende il primo di ciascuno.
pub fn parse_avcc(d: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    if d.len() < 7 || d[0] != 1 {
        return None;
    }
    let mut pos = 5;
    let take = |pos: &mut usize, count: usize| -> Option<Vec<u8>> {
        if count == 0 {
            return None;
        }
        let mut first = None;
        for i in 0..count {
            if d.len() - *pos < 2 {
                return None;
            }
            let n = usize::from(u16::from_be_bytes([d[*pos], d[*pos + 1]]));
            *pos += 2;
            if n == 0 || n > d.len() - *pos {
                return None;
            }
            if i == 0 {
                let mut v = vec![0, 0, 0, 1];
                v.extend_from_slice(&d[*pos..*pos + n]);
                first = Some(v);
            }
            *pos += n;
        }
        first
    };
    let num_sps = usize::from(d[pos] & 0x1F);
    pos += 1;
    let sps = take(&mut pos, num_sps)?;
    if d.len() - pos < 1 {
        return None;
    }
    let num_pps = usize::from(d[pos]);
    pos += 1;
    let pps = take(&mut pos, num_pps)?;
    // Tipi attesi: 7 (SPS) e 8 (PPS). Altro vuol dire un codec diverso (es. HEVC "hvc1").
    (sps[4] & 0x1F == 7 && pps[4] & 0x1F == 8).then_some((sps, pps))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slices_and_frames() {
        assert!(!slice_is_b(&[0x41, 0x9a, 0x00]));
        assert!(slice_is_b(&[0x01, 0b1010_0000]));
        let mut f = vec![0, 0, 0, 2, 0x65, 0x88, 0, 0, 0, 2, 0x01, 0b1010_0000];
        assert_eq!(avcc_to_annexb(&mut f, true), Some(Frame { keyframe: true, b_slice: true }));
        assert_eq!(&f[..4], &[0, 0, 0, 1]);
        assert_eq!(avcc_to_annexb(&mut [0, 0, 0, 9, 0x65], false), None);
        assert_eq!(avcc_to_annexb(&mut [0, 0, 0, 1, 0x85], false), None);
        assert_eq!(avcc_to_annexb(&mut [], false), None);
    }

    #[test]
    fn avcc_record() {
        let rec = [1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 3, 0x67, 1, 2, 1, 0, 2, 0x68, 9];
        let (s, p) = parse_avcc(&rec).unwrap();
        assert_eq!(s, [0, 0, 0, 1, 0x67, 1, 2]);
        assert_eq!(p, [0, 0, 0, 1, 0x68, 9]);
        let mut hvc = rec;
        hvc[8] = 0x40;
        assert_eq!(parse_avcc(&hvc), None);
        assert_eq!(parse_avcc(&rec[..rec.len() - 1]), None);
    }

    #[test]
    fn writer_ue_roundtrip() {
        for v in [0u32, 1, 2, 16, 255, 65_535, u32::MAX - 1] {
            let mut w = Writer { out: Vec::new(), n: 0 };
            w.ue(v);
            w.trailing();
            let mut r = Reader { d: &w.out, pos: 0, ok: true };
            assert_eq!(r.ue(), v);
        }
    }
}
