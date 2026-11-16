//! Informazioni sul brano che iOS manda con SET_PARAMETER durante l'audio AirPlay.
//!
//! - application/x-dmap-tagged: elementi tag (4 caratteri) | lunghezza (4 byte big-endian)
//!   | dati; il contenitore "mlit" racchiude "minm" (titolo), "asar" (artista), "asal"
//!   (album). Si esamina un solo livello di contenitore.
//! - text/parameters "progress: inizio/attuale/fine": tre timestamp RTP.
//!
//! I risultati sono posizioni nel buffer ricevuto: nessuna copia, nessuna allocazione.

/// Porzione [off, off+len) del buffer di ingresso.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Span {
    /// Inizio.
    pub off: usize,
    /// Lunghezza.
    pub len: usize,
}

/// Campi del brano trovati (lunghezza 0 se assenti).
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Metadata {
    /// Titolo ("minm").
    pub title: Span,
    /// Artista ("asar").
    pub artist: Span,
    /// Album ("asal").
    pub album: Span,
}

/// Titoli più lunghi vengono troncati, al confine di un carattere UTF-8.
const MAX_FIELD: usize = 256;

fn clip(d: &[u8], off: usize, n: usize) -> Span {
    let mut len = n;
    if len > MAX_FIELD {
        len = MAX_FIELD;
        while len > 0 && d[off + len] & 0xC0 == 0x80 {
            len -= 1;
        }
    }
    Span { off, len }
}

fn walk(d: &[u8], start: usize, end: usize, out: &mut Metadata, allow_container: bool) -> bool {
    let mut pos = start;
    while pos < end {
        if end - pos < 8 {
            return false;
        }
        let tag = &d[pos..pos + 4];
        let n = u32::from_be_bytes([d[pos + 4], d[pos + 5], d[pos + 6], d[pos + 7]]) as usize;
        if n > end - pos - 8 {
            return false;
        }
        let at = pos + 8;
        match tag {
            b"mlit" => {
                if !allow_container || !walk(d, at, at + n, out, false) {
                    return false;
                }
            }
            b"minm" => out.title = clip(d, at, n),
            b"asar" => out.artist = clip(d, at, n),
            b"asal" => out.album = clip(d, at, n),
            _ => {}
        }
        pos = at + n;
    }
    true
}

/// false se il corpo non è un DMAP valido; i campi già trovati restano in `out`.
pub fn parse_dmap(d: &[u8], out: &mut Metadata) -> bool {
    *out = Metadata::default();
    walk(d, 0, d.len(), out, true)
}

fn parse_uint(s: &mut &[u8]) -> Option<u32> {
    let mut v: u64 = 0;
    let mut i = 0;
    while i < s.len() && s[i].is_ascii_digit() {
        v = v * 10 + u64::from(s[i] - b'0');
        if v > 0xFFFF_FFFF {
            return None;
        }
        i += 1;
    }
    if i == 0 {
        return None;
    }
    *s = &s[i..];
    Some(v as u32)
}

/// "progress: a/b/c" -> (inizio, posizione secondo iOS, fine) in timestamp RTP.
pub fn parse_progress(d: &[u8]) -> Option<(u32, u32, u32)> {
    const PREFIX: &[u8] = b"progress:";
    if d.len() < PREFIX.len() || d.len() > 128 || &d[..PREFIX.len()] != PREFIX {
        return None;
    }
    let mut s = &d[PREFIX.len()..];
    while s.first() == Some(&b' ') {
        s = &s[1..];
    }
    let a = parse_uint(&mut s)?;
    s = s.strip_prefix(b"/")?;
    let b = parse_uint(&mut s)?;
    s = s.strip_prefix(b"/")?;
    let c = parse_uint(&mut s)?;
    Some((a, b, c))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn el(tag: &[u8; 4], data: &[u8]) -> Vec<u8> {
        let mut v = tag.to_vec();
        v.extend_from_slice(&(data.len() as u32).to_be_bytes());
        v.extend_from_slice(data);
        v
    }

    #[test]
    fn metadata() {
        let inner = [el(b"minm", b"Song"), el(b"asar", b"Artist"), el(b"xxxx", b"?"), el(b"asal", b"Album")].concat();
        let d = el(b"mlit", &inner);
        let mut m = Metadata::default();
        assert!(parse_dmap(&d, &mut m));
        let get = |s: Span| &d[s.off..s.off + s.len];
        assert_eq!((get(m.title), get(m.artist), get(m.album)), (&b"Song"[..], &b"Artist"[..], &b"Album"[..]));
        // contenitore annidato e lunghezze incoerenti
        assert!(!parse_dmap(&el(b"mlit", &el(b"mlit", b"")), &mut m));
        assert!(!parse_dmap(&d[..d.len() - 1], &mut m));
        // troncamento al confine UTF-8: 255 'a' e poi "è" (2 byte) -> 255 byte
        let long = [vec![b'a'; 255], "èèè".as_bytes().to_vec()].concat();
        assert!(parse_dmap(&el(b"minm", &long), &mut m));
        assert_eq!(m.title.len, 255);
    }

    #[test]
    fn progress() {
        assert_eq!(parse_progress(b"progress: 10/20/30\r\n"), Some((10, 20, 30)));
        assert_eq!(parse_progress(b"progress:4294967295/0/1"), Some((u32::MAX, 0, 1)));
        assert_eq!(parse_progress(b"progress: 4294967296/0/1"), None);
        assert_eq!(parse_progress(b"progress: 1/2"), None);
        assert_eq!(parse_progress(b"progress: -1/2/3"), None);
        assert_eq!(parse_progress(b"volume: 1/2/3"), None);
    }
}
