//! Audio RTP di AirPlay: intestazioni dei pacchetti e buffer di riordino.
//!
//! - Musica (ALAC/AAC): iOS manda con ~1,8 s di anticipo; i pacchetti si rimettono in ordine,
//!   quelli mancanti si chiedono di nuovo (0x80 0xD5) e, se non arrivano entro 150 ms, si
//!   saltano. I numeri di sequenza sono a 16 bit e ripartono da 0 dopo 65535.
//! - Duplicazione schermo (AAC-ELD): nessun riordino, si scartano duplicati e ritardatari.
//!
//! Macchina a stati pura: niente socket né orologi (il tempo arriva da fuori), così si può
//! confrontare con la versione C++ precedente su qualunque sequenza di pacchetti
//! (tools/hosttest/parser_diff.cpp). Comportamento identico a quella.

/// Pacchetti tenuti nel buffer (~2 s di ALAC da 352 campioni).
pub const SLOTS: usize = 256;
const GAP_WAIT_MS: i64 = 150;
const MAX_RESEND: u16 = 64;

/// Intestazione RTP dell'audio AirPlay (versione 2, tipo 0x60): (seq, timestamp).
pub fn parse_rtp(buf: &[u8]) -> Option<(u16, u32)> {
    if buf.len() <= 12 || buf[0] & 0xC0 != 0x80 || buf[1] & 0x7F != 0x60 {
        return None;
    }
    Some((u16::from_be_bytes([buf[2], buf[3]]), u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]])))
}

/// Pacchetto sulla porta di controllo.
#[derive(Debug, PartialEq, Eq)]
pub enum Control {
    /// Ritrasmissione: il pacchetto RTP originale inizia dal byte 4.
    Resend,
    /// Sincronizzazione: frame RTP che iOS considera in uscita adesso.
    Sync(u32),
    /// Altro (ignorato).
    Other,
}

/// Classifica un pacchetto della porta di controllo (solo per la musica).
pub fn classify_control(buf: &[u8]) -> Control {
    if buf.len() < 2 {
        return Control::Other;
    }
    match buf[1] & 0x7F {
        0x56 if buf.len() > 4 + 12 => Control::Resend,
        0x54 if buf.len() >= 20 => Control::Sync(u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]])),
        _ => Control::Other,
    }
}

#[derive(Default, Clone)]
struct Slot {
    used: bool,
    seq: u16,
    rtp: u32,
    data: Vec<u8>,
}

/// Cosa fare dopo [`Reorder::next`].
#[derive(Debug, PartialEq, Eq)]
pub enum Event {
    /// Niente da fare per ora.
    Idle,
    /// Riprodurre questo pacchetto (dati in [`Reorder::delivered`]).
    Deliver {
        /// Timestamp RTP.
        rtp: u32,
    },
    /// Chiedere di nuovo `count` pacchetti a partire da `first`.
    Resend {
        /// Primo mancante.
        first: u16,
        /// Quanti.
        count: u16,
    },
}

/// Stato del flusso audio.
pub struct Reorder {
    have_seq: bool,
    last_seq: u16,
    expected: u16,
    slots: Vec<Slot>,
    buffered: usize,
    gap_since: i64,
    delivered: Vec<u8>,
    /// Pacchetti persi (saltati o mai arrivati).
    pub lost: u64,
    /// Ritrasmissioni chieste.
    pub resend_asked: u64,
}

impl Default for Reorder {
    fn default() -> Self {
        Self::new()
    }
}

impl Reorder {
    /// Buffer vuoto.
    pub fn new() -> Reorder {
        Reorder {
            have_seq: false,
            last_seq: 0,
            expected: 0,
            slots: vec![Slot::default(); SLOTS],
            buffered: 0,
            gap_since: 0,
            delivered: Vec::new(),
            lost: 0,
            resend_asked: 0,
        }
    }

    /// Pacchetti in attesa.
    pub fn buffered(&self) -> usize {
        self.buffered
    }

    /// Dati dell'ultimo [`Event::Deliver`].
    pub fn delivered(&self) -> &[u8] {
        &self.delivered
    }

    fn reset_buffer(&mut self) {
        for s in &mut self.slots {
            s.used = false;
            s.data.clear();
        }
        self.buffered = 0;
        self.gap_since = 0;
    }

    /// FLUSH: si svuota; con `next_seq` noto si riparte da lì.
    pub fn flush(&mut self, next_seq: Option<u16>) {
        self.reset_buffer();
        match next_seq {
            Some(s) => {
                self.expected = s;
                self.have_seq = true;
            }
            None => self.have_seq = false,
        }
    }

    /// Musica: mette un pacchetto nel buffer.
    pub fn insert(&mut self, seq: u16, rtp: u32, payload: &[u8]) {
        if !self.have_seq {
            self.have_seq = true;
            self.expected = seq;
        }
        let delta = seq.wrapping_sub(self.expected) as i16;
        if delta < 0 {
            if delta > -1000 {
                return; // duplicato o arrivato dopo essere stato saltato
            }
            self.reset_buffer(); // salto all'indietro grande: il flusso è ripartito
            self.expected = seq;
        } else if delta as usize >= SLOTS {
            self.reset_buffer(); // troppo avanti: si riparte da qui
            self.expected = seq;
        }
        let slot = &mut self.slots[usize::from(seq) % SLOTS];
        if slot.used {
            return; // stesso pacchetto ricevuto due volte
        }
        slot.used = true;
        slot.seq = seq;
        slot.rtp = rtp;
        slot.data.clear();
        slot.data.extend_from_slice(payload);
        self.buffered += 1;
    }

    /// Musica: prossima azione all'istante `now` (ms). Va chiamata finché non dà Idle.
    pub fn next(&mut self, now: i64) -> Event {
        loop {
            let idx = usize::from(self.expected) % SLOTS;
            if self.slots[idx].used && self.slots[idx].seq == self.expected {
                let slot = &mut self.slots[idx];
                slot.used = false;
                std::mem::swap(&mut self.delivered, &mut slot.data);
                slot.data.clear();
                let rtp = slot.rtp;
                self.buffered -= 1;
                self.expected = self.expected.wrapping_add(1);
                self.gap_since = 0;
                return Event::Deliver { rtp };
            }
            if self.buffered == 0 {
                return Event::Idle;
            }
            // Manca il pacchetto atteso ma ne sono arrivati di successivi.
            let next = (1..SLOTS)
                .map(|i| self.expected.wrapping_add(i as u16))
                .find(|&s| {
                    let sl = &self.slots[usize::from(s) % SLOTS];
                    sl.used && sl.seq == s
                })
                .unwrap_or(self.expected);
            if next == self.expected {
                self.reset_buffer(); // stato incoerente: si svuota
                return Event::Idle;
            }
            if self.gap_since == 0 {
                self.gap_since = now;
                let missing = next.wrapping_sub(self.expected);
                self.resend_asked += 1;
                return Event::Resend { first: self.expected, count: missing.min(MAX_RESEND) };
            }
            if now - self.gap_since < GAP_WAIT_MS {
                return Event::Idle;
            }
            self.lost += u64::from(next.wrapping_sub(self.expected));
            self.expected = next; // non è arrivato in tempo: si salta
            self.gap_since = 0;
        }
    }

    /// Duplicazione schermo: true se il pacchetto va riprodotto (né duplicato né in ritardo).
    pub fn accept_live(&mut self, seq: u16) -> bool {
        if self.have_seq {
            let delta = seq.wrapping_sub(self.last_seq) as i16;
            if delta <= 0 && delta > -1000 {
                return false;
            }
            if delta > 1 && delta < 1000 {
                self.lost += (delta - 1) as u64;
            }
        }
        self.have_seq = true;
        self.last_seq = seq;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn drain(r: &mut Reorder, now: i64) -> Vec<Event> {
        let mut v = Vec::new();
        loop {
            match r.next(now) {
                Event::Idle => return v,
                e => v.push(e),
            }
        }
    }

    #[test]
    fn reorders_and_wraps() {
        let mut r = Reorder::new();
        r.insert(65534, 1, b"a");
        r.insert(0, 3, b"c");
        assert_eq!(drain(&mut r, 1000), vec![Event::Deliver { rtp: 1 }, Event::Resend { first: 65535, count: 1 }]);
        r.insert(65535, 2, b"b");
        assert_eq!(drain(&mut r, 1010), vec![Event::Deliver { rtp: 2 }, Event::Deliver { rtp: 3 }]);
        assert_eq!(r.delivered(), b"c");
        assert_eq!(r.lost, 0);
    }

    #[test]
    fn skips_after_timeout() {
        let mut r = Reorder::new();
        r.insert(10, 0, b"x");
        drain(&mut r, 0);
        r.insert(13, 0, b"y");
        assert_eq!(drain(&mut r, 100), vec![Event::Resend { first: 11, count: 2 }]);
        assert_eq!(drain(&mut r, 200), vec![]);
        assert_eq!(drain(&mut r, 260), vec![Event::Deliver { rtp: 0 }]);
        assert_eq!(r.lost, 2);
    }

    #[test]
    fn live_and_headers() {
        let mut r = Reorder::new();
        assert!(r.accept_live(5));
        assert!(!r.accept_live(5));
        assert!(r.accept_live(9));
        assert_eq!(r.lost, 3);
        assert!(r.accept_live(9u16.wrapping_sub(2000)));
        assert_eq!(parse_rtp(&[0x80, 0xE0, 0, 7, 0, 0, 1, 0, 0, 0, 0, 0, 9]), Some((7, 256)));
        assert_eq!(parse_rtp(&[0x80, 0xE0, 0, 7, 0, 0, 1, 0, 0, 0, 0, 0]), None);
        assert_eq!(classify_control(&[0x80, 0xD4, 0, 4, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]), Control::Sync(9));
        assert_eq!(classify_control(&[0x80, 0xD6]), Control::Other);
    }
}
