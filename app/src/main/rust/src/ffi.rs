//! Interfaccia C dei parser (plist, DMAP, RTSP, HLS), dichiarata in rust_core.h.
//!
//! Regole comuni: un puntatore nullo con lunghezza 0 è un input vuoto; nullo con
//! lunghezza > 0 è un errore. Nessun panic esce di qui ((`guard` in lib.rs)). I risultati
//! sono posizioni nel buffer del chiamante (RTSP, DMAP, prefissi HLS) oppure memoria di
//! Rust raggiungibile solo da handle opachi, letti con funzioni che controllano i limiti
//! e liberati con la funzione `_free` corrispondente.

use crate::dmap::{self, Span};
use crate::guard;
use crate::{hls, plist, rtsp};

/// Byte di ingresso: None se il puntatore è nullo con lunghezza > 0.
///
/// # Safety
/// Se non nullo, `p` deve puntare a `len` byte leggibili per la durata della chiamata.
unsafe fn input<'a>(p: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if p.is_null() {
        return None;
    }
    // SAFETY: contratto della funzione.
    Some(unsafe { std::slice::from_raw_parts(p, len) })
}

// ---------------------------------------------------------------------------- plist

/// Plist interpretato (vedi [`plist::Flat`]).
pub struct PlistDoc(plist::Flat);

/// Un nodo per il C++. `bytes` resta valido finché il documento non è liberato.
#[repr(C)]
pub struct PlistNode {
    /// PLIST_* (vedi [`plist::kind`]).
    pub kind: u8,
    /// Booleano (0/1), intero o UID.
    pub uint: u64,
    /// Reale.
    pub real: f64,
    /// Byte di stringa o dati.
    pub bytes: *const u8,
    /// Numero di byte.
    pub len: usize,
    /// Figli di un array o dizionario.
    pub count: usize,
}

/// Interpreta un bplist00; null se non valido o oltre i limiti. Liberare con [`irplay_plist_free`].
///
/// # Safety
/// `data` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_plist_parse(data: *const u8, len: usize) -> *mut PlistDoc {
    // SAFETY: contratto della funzione.
    let Some(d) = (unsafe { input(data, len) }) else { return std::ptr::null_mut() };
    match guard(None, || plist::parse_binary(d).map(plist::Flat::new)) {
        Some(f) => Box::into_raw(Box::new(PlistDoc(f))),
        None => std::ptr::null_mut(),
    }
}

/// Legge il nodo `id` (0 = radice). false se l'indice non esiste.
///
/// # Safety
/// `doc` da [`irplay_plist_parse`], non ancora liberato; `out` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_plist_node(doc: *const PlistDoc, id: u32, out: *mut PlistNode) -> bool {
    if doc.is_null() || out.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let doc = unsafe { &*doc };
    let Some(n) = doc.0.nodes.get(id as usize) else { return false };
    // SAFETY: `out` scrivibile per contratto.
    unsafe {
        *out = PlistNode { kind: n.kind, uint: n.uint, real: n.real, bytes: n.bytes.as_ptr(), len: n.bytes.len(), count: n.kids.len() };
    }
    true
}

/// Figlio `i` del contenitore `id`: indice del nodo e, nei dizionari, la chiave (altrimenti
/// lunghezza 0). false se non esiste.
///
/// # Safety
/// `doc` da [`irplay_plist_parse`], non ancora liberato; puntatori di uscita scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_plist_child(
    doc: *const PlistDoc,
    id: u32,
    i: usize,
    child: *mut u32,
    key: *mut *const u8,
    key_len: *mut usize,
) -> bool {
    if doc.is_null() || child.is_null() || key.is_null() || key_len.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let doc = unsafe { &*doc };
    let Some(n) = doc.0.nodes.get(id as usize) else { return false };
    let Some(&c) = n.kids.get(i) else { return false };
    let k = n.keys.get(i).map_or(&[][..], |k| k.as_slice());
    // SAFETY: uscite scrivibili per contratto.
    unsafe {
        *child = c;
        *key = k.as_ptr();
        *key_len = k.len();
    }
    true
}

/// Libera un documento (null ammesso).
///
/// # Safety
/// `doc` da [`irplay_plist_parse`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_plist_free(doc: *mut PlistDoc) {
    if !doc.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(doc) });
    }
}

// ---------------------------------------------------------------------------- DMAP

/// Metadati DMAP: posizioni in `data`. false se non valido (i campi trovati restano).
///
/// # Safety
/// `data` deve puntare a `len` byte leggibili; `out` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_dmap_parse(data: *const u8, len: usize, out: *mut dmap::Metadata) -> bool {
    if out.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let Some(d) = (unsafe { input(data, len) }) else { return false };
    let (ok, m) = guard((false, dmap::Metadata::default()), || {
        let mut m = dmap::Metadata::default();
        (dmap::parse_dmap(d, &mut m), m)
    });
    // SAFETY: `out` scrivibile per contratto.
    unsafe { *out = m };
    ok
}

/// "progress: a/b/c" -> tre timestamp RTP.
///
/// # Safety
/// `data` deve puntare a `len` byte leggibili; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_progress_parse(data: *const u8, len: usize, start: *mut u32, current: *mut u32, end: *mut u32) -> bool {
    if start.is_null() || current.is_null() || end.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let Some(d) = (unsafe { input(data, len) }) else { return false };
    let Some((a, b, c)) = guard(None, || dmap::parse_progress(d)) else { return false };
    // SAFETY: uscite scrivibili per contratto.
    unsafe {
        (*start, *current, *end) = (a, b, c);
    }
    true
}

// ---------------------------------------------------------------------------- RTSP

/// Esito di [`irplay_rtsp_parse_request`] e [`irplay_rtsp_discard_response`]: malformato.
pub const RTSP_INVALID: i32 = -1;
/// Servono altri byte.
pub const RTSP_INCOMPLETE: i32 = 0;
/// Messaggio completo.
pub const RTSP_COMPLETE: i32 = 1;

/// Richiesta letta: posizioni nel buffer del chiamante.
#[repr(C)]
#[derive(Default)]
pub struct RtspHead {
    /// Byte da togliere dal buffer.
    pub consumed: usize,
    /// Metodo.
    pub method: Span,
    /// URI.
    pub uri: Span,
    /// Protocollo.
    pub protocol: Span,
    /// Corpo.
    pub body: Span,
    /// Intestazioni scritte in `headers` (coppie nome, valore).
    pub header_count: usize,
}

/// Legge una richiesta RTSP/HTTP dall'inizio di `buf`. `headers` riceve
/// 2 × header_count span (nome, valore); `headers_cap` è in span e deve valere almeno
/// 2 × 64.
///
/// # Safety
/// `buf` deve puntare a `len` byte leggibili; `out` scrivibile; `headers` a `headers_cap`
/// Span scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_rtsp_parse_request(
    buf: *const u8,
    len: usize,
    out: *mut RtspHead,
    headers: *mut Span,
    headers_cap: usize,
) -> i32 {
    if out.is_null() || headers.is_null() || headers_cap < 2 * rtsp::MAX_HEADERS {
        return RTSP_INVALID;
    }
    // SAFETY: contratto della funzione.
    let Some(b) = (unsafe { input(buf, len) }) else { return RTSP_INVALID };
    match guard(rtsp::Parse::Invalid, || rtsp::parse_request(b)) {
        rtsp::Parse::Invalid => RTSP_INVALID,
        rtsp::Parse::Incomplete => RTSP_INCOMPLETE,
        rtsp::Parse::Complete(r) => {
            // SAFETY: `headers` ha headers_cap >= 2 * MAX_HEADERS >= 2 * r.headers.len()
            // span scrivibili; `out` scrivibile.
            unsafe {
                for (i, (n, v)) in r.headers.iter().enumerate() {
                    *headers.add(2 * i) = *n;
                    *headers.add(2 * i + 1) = *v;
                }
                *out = RtspHead {
                    consumed: r.consumed,
                    method: r.method,
                    uri: r.uri,
                    protocol: r.protocol,
                    body: r.body,
                    header_count: r.headers.len(),
                };
            }
            RTSP_COMPLETE
        }
    }
}

/// Risposta HTTP sul canale inverso da scartare: con RTSP_COMPLETE, `consumed` byte da
/// togliere dal buffer.
///
/// # Safety
/// `buf` deve puntare a `len` byte leggibili; `consumed` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_rtsp_discard_response(buf: *const u8, len: usize, consumed: *mut usize) -> i32 {
    if consumed.is_null() {
        return RTSP_INVALID;
    }
    // SAFETY: contratto della funzione.
    let Some(b) = (unsafe { input(buf, len) }) else { return RTSP_INVALID };
    match guard(rtsp::Parse::Invalid, || rtsp::discard_response(b)) {
        rtsp::Parse::Invalid => RTSP_INVALID,
        rtsp::Parse::Incomplete => RTSP_INCOMPLETE,
        rtsp::Parse::Complete(n) => {
            // SAFETY: scrivibile per contratto.
            unsafe { *consumed = n };
            RTSP_COMPLETE
        }
    }
}

// ---------------------------------------------------------------------------- HLS

/// Elenco di sequenze di byte prodotte da Rust (playlist riscritte, URI).
pub struct ByteList(Vec<Vec<u8>>);

/// Numero di elementi (0 se null).
///
/// # Safety
/// `list` da una funzione di questo modulo, non ancora liberata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_bytes_count(list: *const ByteList) -> usize {
    if list.is_null() {
        return 0;
    }
    // SAFETY: contratto della funzione.
    unsafe { &*list }.0.len()
}

/// Elemento `i`: puntatore e lunghezza, validi finché la lista non è liberata.
///
/// # Safety
/// `list` da una funzione di questo modulo, non ancora liberata; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_bytes_get(list: *const ByteList, i: usize, data: *mut *const u8, len: *mut usize) -> bool {
    if list.is_null() || data.is_null() || len.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let list = unsafe { &*list };
    let Some(item) = list.0.get(i) else { return false };
    // SAFETY: uscite scrivibili per contratto.
    unsafe {
        *data = item.as_ptr();
        *len = item.len();
    }
    true
}

/// Libera una lista (null ammesso).
///
/// # Safety
/// `list` da una funzione di questo modulo, non più usata dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_bytes_free(list: *mut ByteList) {
    if !list.is_null() {
        // SAFETY: creata con Box::into_raw, liberata una volta sola.
        drop(unsafe { Box::from_raw(list) });
    }
}

pub(crate) fn into_list(v: Option<Vec<Vec<u8>>>) -> *mut ByteList {
    v.map_or(std::ptr::null_mut(), |v| Box::into_raw(Box::new(ByteList(v))))
}

/// Vedi [`hls::is_playlist`].
///
/// # Safety
/// `text` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hls_is_playlist(text: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { input(text, len) }.is_some_and(|t| guard(false, || hls::is_playlist(t)))
}

/// Vedi [`hls::is_safe_http_url`].
///
/// # Safety
/// `url` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hls_is_safe_http_url(url: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { input(url, len) }.is_some_and(|u| guard(false, || hls::is_safe_http_url(u)))
}

/// Lunghezza del prefisso remoto di una location HLS (vedi [`hls::remote_prefix_len`]).
///
/// # Safety
/// `location` deve puntare a `len` byte leggibili; `out` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hls_remote_prefix_len(location: *const u8, len: usize, out: *mut usize) -> bool {
    if out.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let Some(l) = (unsafe { input(location, len) }) else { return false };
    let Some(n) = guard(None, || hls::remote_prefix_len(l)) else { return false };
    // SAFETY: scrivibile per contratto.
    unsafe { *out = n };
    true
}

/// Master playlist riscritta: [testo, URI remoti...]; null se non valida.
///
/// # Safety
/// `text` e `prefix` devono puntare a `len` e `prefix_len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hls_rewrite_master(text: *const u8, len: usize, prefix: *const u8, prefix_len: usize) -> *mut ByteList {
    // SAFETY: contratto della funzione.
    let (Some(t), Some(p)) = (unsafe { input(text, len) }, unsafe { input(prefix, prefix_len) }) else {
        return std::ptr::null_mut();
    };
    into_list(guard(None, || {
        hls::rewrite_master(t, p).map(|m| std::iter::once(m.text).chain(m.media_uris).collect())
    }))
}

/// Media playlist con #YT-EXT-CONDENSED-URL espanso: `[testo]`; null se malformata.
///
/// # Safety
/// `text` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_hls_expand_condensed(text: *const u8, len: usize) -> *mut ByteList {
    // SAFETY: contratto della funzione.
    let Some(t) = (unsafe { input(text, len) }) else { return std::ptr::null_mut() };
    into_list(guard(None, || hls::expand_condensed(t).map(|o| vec![o])))
}

// ---------------------------------------------------------------------------- H.264

/// AVCC -> Annex-B sul posto (vedi [`crate::h264::avcc_to_annexb`]). false = frame da scartare.
///
/// # Safety
/// `buf` deve puntare a `len` byte scrivibili; `keyframe` e `b_slice` scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_h264_annexb(buf: *mut u8, len: usize, check_b: bool, keyframe: *mut bool, b_slice: *mut bool) -> bool {
    if buf.is_null() || len == 0 || keyframe.is_null() || b_slice.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let d = unsafe { std::slice::from_raw_parts_mut(buf, len) };
    let Some(f) = guard(None, || crate::h264::avcc_to_annexb(d, check_b)) else { return false };
    // SAFETY: scrivibili per contratto.
    unsafe {
        *keyframe = f.keyframe;
        *b_slice = f.b_slice;
    }
    true
}

/// Record avcC -> [SPS, PPS] con start code; null se non valido.
///
/// # Safety
/// `data` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_h264_parse_avcc(data: *const u8, len: usize) -> *mut ByteList {
    // SAFETY: contratto della funzione.
    let Some(d) = (unsafe { input(data, len) }) else { return std::ptr::null_mut() };
    into_list(guard(None, || crate::h264::parse_avcc(d).map(|(s, p)| vec![s, p])))
}

/// SPS riscritto per bassa latenza: `[SPS]`; null se non serve o non è possibile.
///
/// # Safety
/// `sps` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_h264_patch_sps(sps: *const u8, len: usize) -> *mut ByteList {
    // SAFETY: contratto della funzione.
    let Some(d) = (unsafe { input(sps, len) }) else { return std::ptr::null_mut() };
    into_list(guard(None, || crate::h264::patch_sps_low_latency(d).map(|o| vec![o])))
}

/// NAL senza start code: true se è uno slice B.
///
/// # Safety
/// `nal` deve puntare a `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_h264_slice_is_b(nal: *const u8, len: usize) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { input(nal, len) }.is_some_and(|n| guard(false, || crate::h264::slice_is_b(n)))
}

// ---------------------------------------------------------------------------- audio RTP

/// Intestazione RTP audio: seq e timestamp. false se non è un pacchetto audio AirPlay.
///
/// # Safety
/// `buf` deve puntare a `len` byte leggibili; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_rtp_header(buf: *const u8, len: usize, seq: *mut u16, rtp: *mut u32) -> bool {
    if seq.is_null() || rtp.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let Some(b) = (unsafe { input(buf, len) }) else { return false };
    let Some((s, r)) = guard(None, || crate::rtp::parse_rtp(b)) else { return false };
    // SAFETY: scrivibili per contratto.
    unsafe {
        *seq = s;
        *rtp = r;
    }
    true
}

/// Pacchetto della porta di controllo: 0 altro, 1 ritrasmissione (RTP dal byte 4),
/// 2 sincronizzazione (frame RTP in `rtp`).
///
/// # Safety
/// `buf` deve puntare a `len` byte leggibili; `rtp` scrivibile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_rtp_control(buf: *const u8, len: usize, rtp: *mut u32) -> i32 {
    if rtp.is_null() {
        return 0;
    }
    // SAFETY: contratto della funzione.
    let Some(b) = (unsafe { input(buf, len) }) else { return 0 };
    match guard(crate::rtp::Control::Other, || crate::rtp::classify_control(b)) {
        crate::rtp::Control::Resend => 1,
        crate::rtp::Control::Sync(r) => {
            // SAFETY: scrivibile per contratto.
            unsafe { *rtp = r };
            2
        }
        crate::rtp::Control::Other => 0,
    }
}

/// Nuovo buffer di riordino.
#[unsafe(no_mangle)]
pub extern "C" fn irplay_reorder_new() -> *mut crate::rtp::Reorder {
    guard(None, || Some(Box::new(crate::rtp::Reorder::new()))).map_or(std::ptr::null_mut(), Box::into_raw)
}

/// Libera il buffer.
///
/// # Safety
/// `h` da [`irplay_reorder_new`], non più usato dopo.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_free(h: *mut crate::rtp::Reorder) {
    if !h.is_null() {
        // SAFETY: creato con Box::into_raw, liberato una volta sola.
        drop(unsafe { Box::from_raw(h) });
    }
}

/// # Safety
/// `h` nullo oppure da [`irplay_reorder_new`], usato da un solo thread.
unsafe fn reorder<'a>(h: *mut crate::rtp::Reorder) -> Option<&'a mut crate::rtp::Reorder> {
    // SAFETY: contratto della funzione.
    (!h.is_null()).then(|| unsafe { &mut *h })
}

/// Musica: aggiunge un pacchetto (payload già decifrato).
///
/// # Safety
/// `h` valido; `payload` `len` byte leggibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_insert(h: *mut crate::rtp::Reorder, seq: u16, rtp: u32, payload: *const u8, len: usize) {
    // SAFETY: contratto della funzione.
    if let (Some(r), Some(p)) = (unsafe { reorder(h) }, unsafe { input(payload, len) }) {
        guard((), || r.insert(seq, rtp, p));
    }
}

/// Musica: prossima azione. 0 niente; 1 riprodurre (`data`/`len` validi fino alla chiamata
/// successiva, timestamp in `a`); 2 chiedere di nuovo `b` pacchetti da `a`.
///
/// # Safety
/// `h` valido; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_next(
    h: *mut crate::rtp::Reorder,
    now_ms: i64,
    data: *mut *const u8,
    len: *mut usize,
    a: *mut u32,
    b: *mut u32,
) -> i32 {
    if data.is_null() || len.is_null() || a.is_null() || b.is_null() {
        return 0;
    }
    // SAFETY: contratto della funzione.
    let Some(r) = (unsafe { reorder(h) }) else { return 0 };
    match guard(crate::rtp::Event::Idle, || r.next(now_ms)) {
        crate::rtp::Event::Idle => 0,
        crate::rtp::Event::Deliver { rtp } => {
            let d = r.delivered();
            // SAFETY: uscite scrivibili per contratto; i dati restano validi fino alla
            // prossima chiamata su questo buffer.
            unsafe {
                *data = d.as_ptr();
                *len = d.len();
                *a = rtp;
            }
            1
        }
        crate::rtp::Event::Resend { first, count } => {
            // SAFETY: uscite scrivibili per contratto.
            unsafe {
                *a = u32::from(first);
                *b = u32::from(count);
            }
            2
        }
    }
}

/// FLUSH: svuota; `next_seq` < 0 se il prossimo numero di sequenza non è noto.
///
/// # Safety
/// `h` valido.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_flush(h: *mut crate::rtp::Reorder, next_seq: i32) {
    // SAFETY: contratto della funzione.
    if let Some(r) = unsafe { reorder(h) } {
        let s = (next_seq >= 0).then_some((next_seq & 0xFFFF) as u16);
        guard((), || r.flush(s));
    }
}

/// Duplicazione schermo: true se il pacchetto va riprodotto.
///
/// # Safety
/// `h` valido.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_accept_live(h: *mut crate::rtp::Reorder, seq: u16) -> bool {
    // SAFETY: contratto della funzione.
    unsafe { reorder(h) }.is_some_and(|r| guard(false, || r.accept_live(seq)))
}

/// Pacchetti in attesa, persi e ritrasmissioni chieste.
///
/// # Safety
/// `h` valido; uscite scrivibili.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn irplay_reorder_stats(h: *mut crate::rtp::Reorder, buffered: *mut usize, lost: *mut u64, resend: *mut u64) -> bool {
    if buffered.is_null() || lost.is_null() || resend.is_null() {
        return false;
    }
    // SAFETY: contratto della funzione.
    let Some(r) = (unsafe { reorder(h) }) else { return false };
    // SAFETY: uscite scrivibili per contratto.
    unsafe {
        *buffered = r.buffered();
        *lost = r.lost;
        *resend = r.resend_asked;
    }
    true
}
