//! Decoder ALAC (Apple Lossless) per l'audio "solo musica" di AirPlay.
//!
//! Usa il decoder di Symphonia (MPL-2.0), scritto interamente in Rust senza codice `unsafe`:
//! sostituisce FFmpeg, l'ultima libreria C che leggeva dati arrivati dalla rete.
//!
//! AirPlay usa sempre la stessa configurazione, che iOS non invia nel SETUP (era nel campo
//! "fmtp" dell'ANNOUNCE di AirPlay 1): 352 campioni per pacchetto, 16 bit, stereo, 44,1 kHz
//! ("96 352 0 16 40 10 14 2 255 0 0 44100"). La configurazione è nostra; il pacchetto arriva
//! dalla rete: Symphonia ne controlla i limiti, e qui si accettano solo frame stereo con al
//! più [`FRAME`] campioni.

use symphonia_codec_alac::AlacDecoder;
use symphonia_core::audio::{Audio, GenericAudioBufferRef};
use symphonia_core::codecs::audio::well_known::CODEC_ID_ALAC;
use symphonia_core::codecs::audio::{AudioCodecParameters, AudioDecoder, AudioDecoderOptions};
use symphonia_core::packet::PacketRef;
use symphonia_core::units::{Duration, Timestamp};

/// Campioni per canale in un pacchetto AirPlay.
pub const FRAME: usize = 352;
/// Canali (sempre stereo).
pub const CHANNELS: usize = 2;
/// Dimensione massima accettata per un pacchetto (un frame non compresso ne occupa ~1,4 KB).
pub const MAX_PACKET: usize = 8192;

/// ALACSpecificConfig (24 byte, big-endian), come nel campo "fmtp" di AirPlay 1.
const COOKIE: [u8; 24] = [
    0x00, 0x00, 0x01, 0x60, // frameLength 352
    0x00, // compatibleVersion
    0x10, // bitDepth 16
    0x28, 0x0A, 0x0E, // pb 40, mb 10, kb 14
    0x02, // numChannels
    0x00, 0xFF, // maxRun 255
    0x00, 0x00, 0x00, 0x00, // maxFrameBytes
    0x00, 0x00, 0x00, 0x00, // avgBitRate
    0x00, 0x00, 0xAC, 0x44, // sampleRate 44100
];

/// Decoder di un flusso ALAC di AirPlay.
pub struct Alac {
    dec: AlacDecoder,
}

impl Alac {
    /// None solo se Symphonia rifiutasse la nostra configurazione (non succede).
    pub fn new() -> Option<Self> {
        let mut params = AudioCodecParameters::new();
        params.for_codec(CODEC_ID_ALAC).with_sample_rate(44_100).with_extra_data(Box::from(&COOKIE[..]));
        AlacDecoder::try_new(&params, &AudioDecoderOptions::default()).ok().map(|dec| Alac { dec })
    }

    /// Decodifica un pacchetto in PCM 16 bit stereo interleaved. Restituisce i campioni per
    /// canale scritti in `out`, o None se il pacchetto non è valido.
    pub fn decode(&mut self, packet: &[u8], out: &mut [i16; FRAME * CHANNELS]) -> Option<usize> {
        if packet.is_empty() || packet.len() > MAX_PACKET {
            return None;
        }
        let pk = PacketRef::new(0, Timestamp::new(0), Duration::new(FRAME as u64), packet);
        let GenericAudioBufferRef::S32(buf) = self.dec.decode_ref(&pk).ok()? else { return None };
        let n = buf.frames();
        if n == 0 || n > FRAME || buf.num_planes() != CHANNELS {
            return None;
        }
        let (l, r) = (buf.plane(0)?, buf.plane(1)?);
        if l.len() < n || r.len() < n {
            return None;
        }
        // Symphonia allinea i campioni in alto nei 32 bit: i 16 bit originali sono quelli alti.
        for (i, o) in out.as_chunks_mut::<CHANNELS>().0.iter_mut().take(n).enumerate() {
            o[0] = (l[i] >> 16) as i16;
            o[1] = (r[i] >> 16) as i16;
        }
        Some(n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Scrittore di bit MSB-first per costruire pacchetti di prova.
    struct Bits(Vec<u8>, usize);
    impl Bits {
        fn put(&mut self, v: u32, n: u32) {
            for i in (0..n).rev() {
                if self.1 / 8 == self.0.len() {
                    self.0.push(0);
                }
                if (v >> i) & 1 == 1 {
                    self.0[self.1 / 8] |= 0x80 >> (self.1 % 8);
                }
                self.1 += 1;
            }
        }
    }

    /// Frame stereo non compresso (CPE con il bit "uncompressed"), eventualmente parziale.
    fn uncompressed(samples: &[(i16, i16)], partial: bool) -> Vec<u8> {
        let mut b = Bits(Vec::new(), 0);
        b.put(1, 3); // CPE
        b.put(0, 4); // tag
        b.put(0, 12); // riservato
        b.put(partial as u32, 1);
        b.put(0, 2); // shift
        b.put(1, 1); // non compresso
        if partial {
            b.put(samples.len() as u32, 32);
        }
        for &(l, r) in samples {
            b.put(l as u16 as u32, 16);
            b.put(r as u16 as u32, 16);
        }
        b.put(7, 3); // END
        b.0
    }

    fn sine(n: usize, phase: usize) -> Vec<(i16, i16)> {
        (0..n)
            .map(|i| {
                let t = (i + phase) as f64 / 44_100.0;
                ((t * 440.0 * std::f64::consts::TAU).sin() * 30_000.0) as i16
            })
            .map(|s| (s, s.wrapping_neg()))
            .collect()
    }

    #[test]
    fn uncompressed_frames_are_exact() {
        let mut a = Alac::new().unwrap();
        let mut out = [0i16; FRAME * CHANNELS];
        for (n, partial) in [(FRAME, false), (FRAME, true), (100, true), (1, true)] {
            let s = sine(n, n * 7);
            assert_eq!(a.decode(&uncompressed(&s, partial), &mut out), Some(n));
            for (i, &(l, r)) in s.iter().enumerate() {
                assert_eq!((out[2 * i], out[2 * i + 1]), (l, r));
            }
        }
        // estremi della scala
        let s = vec![(i16::MIN, i16::MAX); FRAME];
        assert_eq!(a.decode(&uncompressed(&s, false), &mut out), Some(FRAME));
        assert!(out.chunks(2).all(|c| c == [i16::MIN, i16::MAX]));
    }

    #[test]
    fn rejects_bad_packets() {
        let mut a = Alac::new().unwrap();
        let mut out = [0i16; FRAME * CHANNELS];
        assert_eq!(a.decode(&[], &mut out), None);
        assert_eq!(a.decode(&vec![0; MAX_PACKET + 1], &mut out), None);
        // frame parziale che dichiara 1000 campioni (> 352)
        let mut bad = uncompressed(&sine(4, 0), true);
        bad[2] = 0; bad[3] = 0; bad[4] = 0x03; bad[5] = 0xE8 >> 1;
        assert_eq!(a.decode(&bad, &mut out), None);
        // troncato
        let good = uncompressed(&sine(FRAME, 0), false);
        assert_eq!(a.decode(&good[..good.len() / 2], &mut out), None);
        // dopo gli errori il decoder funziona ancora
        assert_eq!(a.decode(&good, &mut out), Some(FRAME));
    }

    /// Generatore deterministico (xorshift) per il fuzzing.
    fn rng(state: &mut u64) -> u64 {
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        *state
    }

    #[test]
    fn fuzz_no_panic() {
        let mut a = Alac::new().unwrap();
        let mut out = [0i16; FRAME * CHANNELS];
        let base = [uncompressed(&sine(FRAME, 0), false), uncompressed(&sine(77, 3), true)];
        let mut st = 0x9E37_79B9_7F4A_7C15u64;
        for _ in 0..200_000 {
            let mut m = base[(rng(&mut st) % 2) as usize].clone();
            for _ in 0..1 + rng(&mut st) % 4 {
                let i = (rng(&mut st) as usize) % m.len();
                m[i] = rng(&mut st) as u8;
            }
            if rng(&mut st).is_multiple_of(8) {
                m.truncate(1 + (rng(&mut st) as usize) % m.len());
            }
            if let Some(n) = a.decode(&m, &mut out) {
                assert!((1..=FRAME).contains(&n));
            }
        }
        for _ in 0..50_000 {
            let len = 1 + (rng(&mut st) as usize) % 2048;
            let m: Vec<u8> = (0..len).map(|_| rng(&mut st) as u8).collect();
            if let Some(n) = a.decode(&m, &mut out) {
                assert!(n <= FRAME);
            }
        }
        assert_eq!(a.decode(&base[0], &mut out), Some(FRAME));
    }
}
