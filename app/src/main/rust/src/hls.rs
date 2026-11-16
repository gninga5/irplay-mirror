//! Controllo e riscrittura delle playlist HLS (RFC 8216) ricevute per l'AirPlay video.
//!
//! Con YouTube l'iPhone passa "mlhls://localhost/master.m3u8": le playlist esistono solo
//! sull'iPhone e arrivano una alla volta (FCUP). Qui:
//! - nella master playlist gli URI delle media playlist con il prefisso remoto diventano
//!   locali ([`LOCAL_PREFIX`]), serviti dalla memoria dal lettore di Kotlin;
//! - nelle media playlist di YouTube si espande #YT-EXT-CONDENSED-URL ("s/<v1>/<v2>/...").
//!
//! Testo di rete trattato come byte (non necessariamente UTF-8), con limiti su righe, URI e
//! dimensione del risultato. Stesso comportamento della versione C++ precedente.

use crate::text::{find, find_byte};

/// Schema con cui il lettore di Kotlin riconosce le playlist servite dalla memoria.
pub const LOCAL_PREFIX: &[u8] = b"irplay-hls://local";
/// Varianti video + tracce audio/sottotitoli al massimo.
pub const MAX_MEDIA_PLAYLISTS: usize = 48;
/// Lunghezza massima di un URI di playlist.
pub const MAX_URI_BYTES: usize = 4096;
/// Dimensione massima di una playlist prodotta.
pub const MAX_OUTPUT_BYTES: usize = 16 * 1024 * 1024;
const MAX_LINES: usize = 400_000; // ~ 2 giorni di segmenti da 0,5 s
const MAX_PARAMS: usize = 32;

/// Righe (LF o CRLF); None se sono troppe.
fn split_lines(text: &[u8]) -> Option<Vec<&[u8]>> {
    let mut out = Vec::new();
    let mut pos = 0;
    while pos < text.len() {
        let nl = find_byte(text, b'\n', pos).unwrap_or(text.len());
        let mut line = &text[pos..nl];
        if line.last() == Some(&b'\r') {
            line = &line[..line.len() - 1];
        }
        out.push(line);
        if out.len() > MAX_LINES {
            return None;
        }
        pos = nl + 1;
    }
    Some(out)
}

/// ASCII stampabile, niente spazi né virgolette, lunghezza limitata.
fn is_clean_uri(u: &[u8]) -> bool {
    !u.is_empty() && u.len() <= MAX_URI_BYTES && u.iter().all(|&c| c > 0x20 && c < 0x7F && c != b'"')
}

/// Valore dell'attributo NAME="..." (intero: preceduto da ':' o ','), come intervallo in `line`.
fn quoted_attr(line: &[u8], name: &[u8]) -> Option<(usize, usize)> {
    let mut from = 0;
    loop {
        let p = find(line, name, from)?;
        let boundary = p > 0 && (line[p - 1] == b':' || line[p - 1] == b',');
        let q = p + name.len();
        if boundary && q + 1 < line.len() && line[q] == b'=' && line[q + 1] == b'"' {
            let start = q + 2;
            let end = find_byte(line, b'"', start)?;
            return Some((start, end));
        }
        from = p + 1;
    }
}

fn attr<'a>(line: &'a [u8], name: &[u8]) -> Option<&'a [u8]> {
    quoted_attr(line, name).map(|(s, e)| &line[s..e])
}

fn contains(h: &[u8], n: &[u8]) -> bool {
    find(h, n, 0).is_some()
}

fn is_he_aac(codecs: &[u8]) -> bool {
    contains(codecs, b"mp4a.40.5") || contains(codecs, b"mp4a.40.29")
}

/// YouTube offre HE-AAC (~48 kbps) e AAC-LC (128 kbps), entrambi DEFAULT=YES, e ExoPlayer
/// prende il primo, HE-AAC, che su un buon impianto suona ovattato. Se c'è almeno una
/// variante AAC-LC si tolgono le varianti HE-AAC e le tracce audio dei gruppi rimasti
/// senza varianti. Per ogni riga: se va tenuta.
fn prefer_aac_lc(lines: &[&[u8]]) -> Vec<bool> {
    let mut keep = vec![true; lines.len()];
    let have_lc = lines.iter().any(|l| {
        l.starts_with(b"#EXT-X-STREAM-INF:") && attr(l, b"CODECS").is_some_and(|c| contains(c, b"mp4a.40.2") && !is_he_aac(c))
    });
    if !have_lc {
        return keep;
    }
    let mut used_groups: Vec<&[u8]> = Vec::new();
    for i in 0..lines.len() {
        if !lines[i].starts_with(b"#EXT-X-STREAM-INF:") {
            continue;
        }
        if attr(lines[i], b"CODECS").is_some_and(is_he_aac) {
            keep[i] = false;
            // La riga URI della variante è la prossima che non è un tag né vuota.
            if let Some(j) = (i + 1..lines.len()).find(|&j| !lines[j].is_empty() && lines[j][0] != b'#') {
                keep[j] = false;
            }
        } else if let Some(g) = attr(lines[i], b"AUDIO") {
            used_groups.push(g);
        }
    }
    for i in 0..lines.len() {
        if !lines[i].starts_with(b"#EXT-X-MEDIA:") || !contains(lines[i], b"TYPE=AUDIO") {
            continue;
        }
        if let Some(g) = attr(lines[i], b"GROUP-ID")
            && !used_groups.contains(&g)
        {
            keep[i] = false;
        }
    }
    keep
}

/// Testo che inizia con "#EXTM3U", senza byte NUL.
pub fn is_playlist(text: &[u8]) -> bool {
    text.starts_with(b"#EXTM3U") && !text.contains(&0)
}

/// URL http o https assoluto, ASCII stampabile senza spazi, al massimo 8 KB, con un host e
/// senza credenziali. Altri schemi (file:, content:, javascript: ...) sono rifiutati: il
/// lettore non deve aprire file locali della TV su richiesta di un dispositivo della rete.
pub fn is_safe_http_url(url: &[u8]) -> bool {
    if url.is_empty() || url.len() > 8192 || !url.iter().all(|&c| c > 0x20 && c < 0x7F && c != b'"') {
        return false;
    }
    let rest = if url.starts_with(b"https://") {
        8
    } else if url.starts_with(b"http://") {
        7
    } else {
        return false;
    };
    let host_end = url[rest..].iter().position(|c| b"/?#".contains(c)).map_or(url.len(), |p| p + rest);
    let host = &url[rest..host_end];
    // "user:pass@host" nasconde il vero host a chi legge i log.
    !host.is_empty() && !host.contains(&b'@')
}

/// Prefisso remoto di una location HLS ("mlhls://localhost/master.m3u8" -> "mlhls://localhost"),
/// come lunghezza: il prefisso è l'inizio di `location`.
pub fn remote_prefix_len(location: &[u8]) -> Option<usize> {
    if !is_clean_uri(location) {
        return None;
    }
    let p = find(location, b"/master.m3u8", 0)?;
    if p == 0 {
        return None;
    }
    let colon = find(location, b"://", 0)?;
    if colon == 0 || colon > p {
        return None;
    }
    Some(p)
}

fn to_local(remote: &[u8], prefix: &[u8], out: &mut Vec<u8>) {
    out.extend_from_slice(LOCAL_PREFIX);
    out.extend_from_slice(&remote[prefix.len().min(remote.len())..]);
}

fn add_unique(v: &mut Vec<Vec<u8>>, s: &[u8]) {
    if !v.iter().any(|x| x == s) {
        v.push(s.to_vec());
    }
}

/// Master playlist riscritta.
#[derive(Debug, PartialEq, Eq)]
pub struct Master {
    /// Playlist per il lettore.
    pub text: Vec<u8>,
    /// URI remoti da chiedere all'iPhone, senza doppioni.
    pub media_uris: Vec<Vec<u8>>,
}

/// Riscrive la master playlist. Se è già una media playlist (contiene #EXTINF) torna
/// espansa e senza URI da chiedere. None se non è valida.
pub fn rewrite_master(text: &[u8], prefix: &[u8]) -> Option<Master> {
    if !is_playlist(text) || prefix.is_empty() {
        return None;
    }
    if contains(text, b"#EXTINF") {
        return expand_condensed(text).map(|t| Master { text: t, media_uris: Vec::new() });
    }
    let lines = split_lines(text)?;
    let keep = prefer_aac_lc(&lines);
    let mut m = Master { text: Vec::with_capacity(text.len() + 1024), media_uris: Vec::new() };
    for (li, &line) in lines.iter().enumerate() {
        if !keep[li] {
            continue;
        }
        if line.is_empty() {
            m.text.push(b'\n');
            continue;
        }
        if line[0] == b'#' {
            // Tracce alternative: #EXT-X-MEDIA:TYPE=AUDIO,...,URI="mlhls://localhost/..."
            match quoted_attr(line, b"URI") {
                Some((s, e)) if line[s..e].starts_with(prefix) => {
                    let uri = &line[s..e];
                    if !is_clean_uri(uri) {
                        return None;
                    }
                    add_unique(&mut m.media_uris, uri);
                    m.text.extend_from_slice(&line[..s]);
                    to_local(uri, prefix, &mut m.text);
                    m.text.extend_from_slice(&line[e..]);
                }
                _ => m.text.extend_from_slice(line),
            }
        } else if line.starts_with(prefix) {
            // Riga URI di una variante (#EXT-X-STREAM-INF la precede).
            if !is_clean_uri(line) {
                return None;
            }
            add_unique(&mut m.media_uris, line);
            to_local(line, prefix, &mut m.text);
        } else {
            m.text.extend_from_slice(line); // altri URI: li rifiuterà il lettore
        }
        m.text.push(b'\n');
        if m.media_uris.len() > MAX_MEDIA_PLAYLISTS || m.text.len() > MAX_OUTPUT_BYTES {
            return None;
        }
    }
    if m.media_uris.is_empty() {
        return None;
    }
    Some(m)
}

/// Espande #YT-EXT-CONDENSED-URL; senza il tag il testo resta com'è (con l'a capo finale).
/// None se il tag è malformato.
pub fn expand_condensed(media: &[u8]) -> Option<Vec<u8>> {
    if !is_playlist(media) {
        return None;
    }
    let Some(tag) = find(media, b"#YT-EXT-CONDENSED-URL", 0) else {
        let mut out = media.to_vec();
        if out.last() != Some(&b'\n') {
            out.push(b'\n'); // RFC 8216: ogni riga termina con un a capo
        }
        return Some(out);
    };
    // #YT-EXT-CONDENSED-URL:BASE-URI="https://...",PARAMS="id,itag,...",PREFIX="s/"
    let tag_end = find_byte(media, b'\n', tag).unwrap_or(media.len());
    let tag_line = &media[tag..tag_end];
    let base = attr(tag_line, b"BASE-URI")?;
    let params_attr = attr(tag_line, b"PARAMS")?;
    let prefix = attr(tag_line, b"PREFIX")?;
    if prefix.is_empty() || params_attr.is_empty() {
        return None;
    }
    // I segmenti espansi diventano indirizzi scaricati dalla TV: devono essere http/https.
    if !is_safe_http_url(base) {
        return None;
    }
    let mut params: Vec<&[u8]> = Vec::new();
    let mut p = 0;
    while p <= params_attr.len() {
        let comma = find_byte(params_attr, b',', p).unwrap_or(params_attr.len());
        let param = &params_attr[p..comma];
        if !is_clean_uri(param) {
            return None;
        }
        params.push(param);
        if params.len() > MAX_PARAMS {
            return None;
        }
        p = comma + 1;
    }
    let lines = split_lines(media)?;
    let mut out = Vec::with_capacity(MAX_OUTPUT_BYTES.min(media.len() * 4));
    for line in lines {
        if !line.is_empty() && line[0] != b'#' && line.starts_with(prefix) {
            // "s/<v1>/.../<vN>" -> BASE/<p1>/<v1>/.../<pN>/<vN>; l'ultimo valore prende il resto.
            let mut rest = &line[prefix.len()..];
            out.extend_from_slice(base);
            out.push(b'/');
            for (i, param) in params.iter().enumerate() {
                let last = i + 1 == params.len();
                let value = if last {
                    rest
                } else {
                    let slash = find_byte(rest, b'/', 0)?;
                    let v = &rest[..slash];
                    rest = &rest[slash + 1..];
                    v
                };
                out.extend_from_slice(param);
                out.push(b'/');
                out.extend_from_slice(value);
                if !last {
                    out.push(b'/');
                }
            }
        } else {
            out.extend_from_slice(line);
        }
        out.push(b'\n');
        if out.len() > MAX_OUTPUT_BYTES {
            return None;
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const MASTER: &[u8] = b"#EXTM3U\n\
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"lo\",URI=\"mlhls://localhost/a48.m3u8\"\n\
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hi\",URI=\"mlhls://localhost/a128.m3u8\"\n\
#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"avc1.4d401f,mp4a.40.5\",AUDIO=\"lo\"\n\
mlhls://localhost/v1.m3u8\n\
#EXT-X-STREAM-INF:BANDWIDTH=2,CODECS=\"avc1.640028,mp4a.40.2\",AUDIO=\"hi\"\r\n\
mlhls://localhost/v2.m3u8\n";

    #[test]
    fn master_rewrite_prefers_aac_lc() {
        let m = rewrite_master(MASTER, b"mlhls://localhost").unwrap();
        assert_eq!(m.media_uris, vec![b"mlhls://localhost/a128.m3u8".to_vec(), b"mlhls://localhost/v2.m3u8".to_vec()]);
        let t = String::from_utf8(m.text).unwrap();
        assert!(t.contains("URI=\"irplay-hls://local/a128.m3u8\""));
        assert!(t.contains("\nirplay-hls://local/v2.m3u8\n"));
        assert!(!t.contains("v1.m3u8") && !t.contains("a48"));
        assert_eq!(rewrite_master(b"#EXTM3U\n#EXT-X-STREAM-INF:X\nhttp://x/v.m3u8\n", b"mlhls://localhost"), None);
        assert_eq!(rewrite_master(b"EXTM3U\n", b"mlhls://localhost"), None);
    }

    #[test]
    fn condensed() {
        let media = b"#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"https://r.example/v\",PARAMS=\"id,itag,range\",PREFIX=\"s/\"\n#EXTINF:5,\ns/abc/22/0-100/more\n";
        let out = String::from_utf8(expand_condensed(media).unwrap()).unwrap();
        assert!(out.ends_with("#EXTINF:5,\nhttps://r.example/v/id/abc/itag/22/range/0-100/more\n"));
        let bad = b"#EXTM3U\n#YT-EXT-CONDENSED-URL:BASE-URI=\"file:///x\",PARAMS=\"id\",PREFIX=\"s/\"\n";
        assert_eq!(expand_condensed(bad), None);
        assert_eq!(expand_condensed(b"#EXTM3U\n#EXTINF:1,\nx"), Some(b"#EXTM3U\n#EXTINF:1,\nx\n".to_vec()));
    }

    #[test]
    fn urls_and_prefixes() {
        assert!(is_safe_http_url(b"https://a.b/c?d"));
        assert!(!is_safe_http_url(b"https://user@a.b/"));
        assert!(!is_safe_http_url(b"file:///etc/hosts"));
        assert!(!is_safe_http_url(b"https:///x"));
        assert_eq!(remote_prefix_len(b"mlhls://localhost/master.m3u8"), Some(17));
        assert_eq!(remote_prefix_len(b"/master.m3u8"), None);
        assert_eq!(remote_prefix_len(b"localhost/master.m3u8"), None);
    }
}
