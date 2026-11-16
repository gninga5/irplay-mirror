//! Lettura dei messaggi RTSP/1.0 e HTTP/1.1 che iOS usa sulla stessa porta TCP.
//!
//! Ogni byte è ostile: limiti a intestazioni e corpo, un solo Content-Length, niente
//! Transfer-Encoding (request smuggling). I risultati sono posizioni nel buffer ricevuto.

use crate::dmap::Span;
use crate::text::{find, find_byte};

/// Intestazioni al massimo (byte prima di "\r\n\r\n").
pub const MAX_HEADER_BYTES: usize = 16 * 1024;
/// Corpo normale: fp-setup 164 B, i plist di SETUP pochi KB.
pub const MAX_BODY_BYTES: usize = 64 * 1024;
/// SET_PARAMETER con la copertina dell'album.
pub const MAX_ARTWORK_BYTES: usize = 2 * 1024 * 1024;
/// POST /action con una playlist HLS intera.
pub const MAX_PLAYLIST_BYTES: usize = 4 * 1024 * 1024;
/// Righe di intestazione al massimo.
pub const MAX_HEADERS: usize = 64;

/// Richiesta completa.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Request {
    /// Byte da togliere dal buffer (intestazioni + corpo).
    pub consumed: usize,
    /// Metodo.
    pub method: Span,
    /// URI.
    pub uri: Span,
    /// "RTSP/1.0" o "HTTP/1.1".
    pub protocol: Span,
    /// Intestazioni (nome, valore), già senza spazi ai lati.
    pub headers: Vec<(Span, Span)>,
    /// Corpo.
    pub body: Span,
}

/// Esito della lettura.
#[derive(Debug, PartialEq, Eq)]
pub enum Parse<T> {
    /// Servono altri byte.
    Incomplete,
    /// Malformato o oltre i limiti: la connessione va chiusa.
    Invalid,
    /// Messaggio completo.
    Complete(T),
}

fn span(start: usize, end: usize) -> Span {
    Span { off: start, len: end - start }
}

/// Toglie spazi e tabulazioni ai lati di buf[start..end].
fn trim(buf: &[u8], mut start: usize, mut end: usize) -> Span {
    while start < end && (buf[start] == b' ' || buf[start] == b'\t') {
        start += 1;
    }
    while end > start && (buf[end - 1] == b' ' || buf[end - 1] == b'\t') {
        end -= 1;
    }
    span(start, end)
}

fn at(buf: &[u8], s: Span) -> &[u8] {
    &buf[s.off..s.off + s.len]
}

/// Solo cifre decimali (al più 8), niente segno né spazi: rifiuta "-1", "1e9", "123abc".
fn parse_content_length(v: &[u8], max_body: usize) -> Option<usize> {
    if v.is_empty() || v.len() > 8 || !v.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let n = v.iter().fold(0usize, |n, &c| n * 10 + usize::from(c - b'0'));
    (n <= max_body).then_some(n)
}

/// Limite al corpo secondo la prima riga: solo copertine e playlist possono essere grandi.
fn max_body_for(method: &[u8], uri: &[u8]) -> usize {
    if method == b"SET_PARAMETER" {
        MAX_ARTWORK_BYTES
    } else if method == b"POST" && uri == b"/action" {
        MAX_PLAYLIST_BYTES
    } else {
        MAX_BODY_BYTES
    }
}

/// Legge una richiesta dall'inizio di `buf`.
pub fn parse_request(buf: &[u8]) -> Parse<Request> {
    let Some(end) = find(buf, b"\r\n\r\n", 0) else {
        return if buf.len() > MAX_HEADER_BYTES { Parse::Invalid } else { Parse::Incomplete };
    };
    if end > MAX_HEADER_BYTES {
        return Parse::Invalid;
    }
    let head = &buf[..end];
    let eol = find(head, b"\r\n", 0);
    let line_end = eol.unwrap_or(head.len());
    let line = &head[..line_end];
    let sp1 = find_byte(line, b' ', 0);
    let sp2 = sp1.and_then(|s| find_byte(line, b' ', s + 1));
    let (Some(sp1), Some(sp2)) = (sp1, sp2) else { return Parse::Invalid };
    if sp1 == 0 || sp2 == sp1 + 1 {
        return Parse::Invalid;
    }
    let mut req = Request { method: span(0, sp1), uri: span(sp1 + 1, sp2), protocol: span(sp2 + 1, line_end), ..Default::default() };
    let protocol = at(buf, req.protocol);
    if protocol != b"RTSP/1.0" && protocol != b"HTTP/1.1" {
        return Parse::Invalid;
    }
    let max_body = max_body_for(at(buf, req.method), at(buf, req.uri));

    let mut content_length = None;
    let mut pos = eol.map_or(head.len(), |e| e + 2);
    while pos < head.len() {
        let next = find(head, b"\r\n", pos).unwrap_or(head.len());
        let Some(colon) = find_byte(&head[..next], b':', pos) else { return Parse::Invalid };
        if colon == pos {
            return Parse::Invalid;
        }
        let name = trim(buf, pos, colon);
        let value = trim(buf, colon + 1, next);
        // Due Content-Length, o Transfer-Encoding, farebbero leggere una richiesta "nascosta"
        // dentro un'altra.
        if at(buf, name).eq_ignore_ascii_case(b"Transfer-Encoding") {
            return Parse::Invalid;
        }
        if at(buf, name).eq_ignore_ascii_case(b"Content-Length") {
            if content_length.is_some() {
                return Parse::Invalid;
            }
            let Some(n) = parse_content_length(at(buf, value), max_body) else { return Parse::Invalid };
            content_length = Some(n);
        }
        if req.headers.len() >= MAX_HEADERS {
            return Parse::Invalid;
        }
        req.headers.push((name, value));
        pos = next + 2;
    }
    let total = end + 4 + content_length.unwrap_or(0); // entrambi i termini sono limitati
    if buf.len() < total {
        return Parse::Incomplete;
    }
    req.body = span(end + 4, total);
    req.consumed = total;
    Parse::Complete(req)
}

/// Riga di stato "NOME/x.y codice": iOS risponde agli eventi con HTTP/1.1, ad alcuni con
/// EVENT/1.0.
fn is_status_line(head: &[u8]) -> bool {
    let mut i = 0;
    while i < head.len() && i < 16 && head[i].is_ascii_uppercase() {
        i += 1;
    }
    if i == 0 || i + 9 > head.len() || head[i] != b'/' {
        return false;
    }
    let r = &head[i + 1..];
    r.len() >= 7
        && r[0].is_ascii_digit()
        && r[1] == b'.'
        && r[2].is_ascii_digit()
        && r[3] == b' '
        && r[4].is_ascii_digit()
        && r[5].is_ascii_digit()
        && r[6].is_ascii_digit()
}

/// Canale inverso (PTTH): risposta HTTP di iOS a una nostra POST /event, da scartare.
/// Complete(n): togliere n byte dal buffer.
pub fn discard_response(buf: &[u8]) -> Parse<usize> {
    let Some(end) = find(buf, b"\r\n\r\n", 0) else {
        return if buf.len() > MAX_HEADER_BYTES { Parse::Invalid } else { Parse::Incomplete };
    };
    let head = &buf[..end];
    if end > MAX_HEADER_BYTES || !is_status_line(head) {
        return Parse::Invalid;
    }
    // Le risposte a /event non hanno corpo, ma un Content-Length va rispettato.
    let mut content_length = None;
    let mut pos = find(head, b"\r\n", 0);
    while let Some(p) = pos {
        if p + 2 >= head.len() {
            break;
        }
        let start = p + 2;
        let next = find(head, b"\r\n", start);
        let line_end = next.unwrap_or(head.len());
        if let Some(colon) = find_byte(&head[..line_end], b':', start) {
            let name = trim(buf, start, colon);
            if at(buf, name).eq_ignore_ascii_case(b"Transfer-Encoding") {
                return Parse::Invalid;
            }
            if at(buf, name).eq_ignore_ascii_case(b"Content-Length") {
                if content_length.is_some() {
                    return Parse::Invalid;
                }
                let Some(n) = parse_content_length(at(buf, trim(buf, colon + 1, line_end)), MAX_BODY_BYTES) else {
                    return Parse::Invalid;
                };
                content_length = Some(n);
            }
        }
        pos = next;
    }
    let total = end + 4 + content_length.unwrap_or(0);
    if buf.len() < total {
        return Parse::Incomplete;
    }
    Parse::Complete(total)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(s: &[u8]) -> Parse<Request> {
        parse_request(s)
    }

    #[test]
    fn requests() {
        let m = b"SETUP rtsp://x/1 RTSP/1.0\r\nCSeq:  3 \r\nContent-Length: 4\r\n\r\nbodyNEXT";
        let Parse::Complete(r) = req(m) else { panic!() };
        assert_eq!(at(m, r.method), b"SETUP");
        assert_eq!(at(m, r.uri), b"rtsp://x/1");
        assert_eq!(r.headers.len(), 2);
        assert_eq!(at(m, r.headers[0].1), b"3");
        assert_eq!(at(m, r.body), b"body");
        assert_eq!(r.consumed, m.len() - 4);
        assert_eq!(req(&m[..m.len() - 6]), Parse::Incomplete);
        assert_eq!(req(b"GET /info RTSP/1.0\r\n"), Parse::Incomplete);
        for bad in [
            &b"GET /info RTSP/2.0\r\n\r\n"[..],
            b" GET /info RTSP/1.0\r\n\r\n",
            b"GET  RTSP/1.0\r\n\r\n",
            b"GET /info RTSP/1.0\r\nX\r\n\r\n",
            b"GET /info RTSP/1.0\r\n: v\r\n\r\n",
            b"POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"POST /x HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\nab",
            b"POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
            b"POST /x HTTP/1.1\r\nContent-Length: 65537\r\n\r\n",
        ] {
            assert_eq!(req(bad), Parse::Invalid, "{}", String::from_utf8_lossy(bad));
        }
        // limite al corpo secondo il metodo
        assert!(matches!(req(b"SET_PARAMETER x RTSP/1.0\r\nContent-Length: 2000000\r\n\r\n"), Parse::Incomplete));
        // troppi header
        let mut many = b"GET / HTTP/1.1\r\n".to_vec();
        for _ in 0..65 {
            many.extend_from_slice(b"A: b\r\n");
        }
        many.extend_from_slice(b"\r\n");
        assert_eq!(req(&many), Parse::Invalid);
        assert_eq!(req(&vec![b'a'; MAX_HEADER_BYTES + 1]), Parse::Invalid);
    }

    #[test]
    fn responses() {
        assert_eq!(discard_response(b"HTTP/1.1 200 OK\r\n\r\nX"), Parse::Complete(19));
        let ev = b"EVENT/1.0 200 OK\r\nContent-Length: 2\r\n\r\nab";
        assert_eq!(discard_response(ev), Parse::Complete(ev.len()));
        assert_eq!(discard_response(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na"), Parse::Incomplete);
        assert_eq!(discard_response(b"GET / HTTP/1.1\r\n\r\n"), Parse::Invalid);
        assert_eq!(discard_response(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: x\r\n\r\n"), Parse::Invalid);
    }
}
