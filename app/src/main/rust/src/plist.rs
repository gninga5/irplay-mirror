//! Lettura dei plist binari ("bplist00") che iOS invia in SETUP, TEARDOWN, GET /info e
//! nell'AirPlay video. Formato CoreFoundation CFBinaryPList:
//!
//! - header "bplist00", oggetti, tabella degli offset, trailer di 32 byte (big-endian);
//! - marker di un byte (tipo nei 4 bit alti, info nei 4 bassi): 0x0 null/bool/fill,
//!   0x1 intero, 0x2 reale, 0x3 data, 0x4 dati, 0x5 ASCII, 0x6 UTF-16BE, 0x8 UID,
//!   0xA array, 0xD dizionario; info 0xF = lunghezza in un intero che segue.
//!
//! Ogni campo è ostile. Limiti: oggetti dichiarati, profondità (i cicli la superano),
//! nodi e byte prodotti in totale (un grafo che riferisce molte volte lo stesso oggetto
//! non può esplodere in memoria, "billion laughs"). Stesso comportamento della versione
//! C++ precedente, verificato con il confronto in tools/hosttest/parser_diff.cpp.

/// Valore di un plist. Le stringhe restano byte (le ASCII di iOS non sono controllate).
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    /// Booleano (anche null e fill, letti come false).
    Bool(bool),
    /// Intero (bit grezzi: gli 8 byte con segno di iOS restano tali) o UID.
    UInt(u64),
    /// Reale o data (secondi dal 2001).
    Real(f64),
    /// Stringa ASCII o UTF-16 convertita in UTF-8.
    Str(Vec<u8>),
    /// Dati binari.
    Data(Vec<u8>),
    /// Array.
    Array(Vec<Value>),
    /// Dizionario, nell'ordine del plist (chiavi ripetute comprese).
    Dict(Vec<(Vec<u8>, Value)>),
}

const MAX_OBJECTS: u64 = 4096;
const MAX_DEPTH: u32 = 16;
const MAX_NODES: usize = 8192;
const MAX_OUTPUT_BYTES: u64 = 256 * 1024;

fn be(p: &[u8]) -> u64 {
    p.iter().fold(0u64, |v, &b| (v << 8) | u64::from(b))
}

fn push_utf8(out: &mut Vec<u8>, cp: u32) {
    if cp < 0x80 {
        out.push(cp as u8);
    } else if cp < 0x800 {
        out.extend_from_slice(&[0xC0 | (cp >> 6) as u8, 0x80 | (cp & 0x3F) as u8]);
    } else if cp < 0x10000 {
        out.extend_from_slice(&[0xE0 | (cp >> 12) as u8, 0x80 | ((cp >> 6) & 0x3F) as u8, 0x80 | (cp & 0x3F) as u8]);
    } else {
        out.extend_from_slice(&[
            0xF0 | (cp >> 18) as u8,
            0x80 | ((cp >> 12) & 0x3F) as u8,
            0x80 | ((cp >> 6) & 0x3F) as u8,
            0x80 | (cp & 0x3F) as u8,
        ]);
    }
}

struct Parser<'a> {
    d: &'a [u8],
    off_size: usize,
    ref_size: usize,
    num_objects: u64,
    table_off: usize,
    nodes: usize,
    out_bytes: u64,
}

/// Interpreta un bplist00. None se il formato non è valido o supera i limiti.
pub fn parse_binary(d: &[u8]) -> Option<Value> {
    if d.len() < 8 + 32 || &d[..8] != b"bplist00" {
        return None;
    }
    let t = &d[d.len() - 32..];
    let (off_size, ref_size) = (usize::from(t[6]), usize::from(t[7]));
    let num_objects = be(&t[8..16]);
    let top = be(&t[16..24]);
    let table_off = be(&t[24..32]);
    if !(1..=8).contains(&off_size) || !(1..=8).contains(&ref_size) {
        return None;
    }
    if num_objects == 0 || num_objects > MAX_OBJECTS || top >= num_objects {
        return None;
    }
    let limit = (d.len() - 32) as u64;
    // num_objects <= 4096 e off_size <= 8: il prodotto non va in overflow.
    if table_off < 8 || table_off > limit || num_objects * off_size as u64 > limit - table_off {
        return None;
    }
    let mut p = Parser { d, off_size, ref_size, num_objects, table_off: table_off as usize, nodes: 0, out_bytes: 0 };
    p.parse(top, 0)
}

impl Parser<'_> {
    /// Posizione dell'oggetto idx: tra l'header e la tabella degli offset.
    fn object_offset(&self, idx: u64) -> Option<usize> {
        if idx >= self.num_objects {
            return None;
        }
        let at = self.table_off + idx as usize * self.off_size;
        let o = be(&self.d[at..at + self.off_size]);
        if o < 8 || o >= self.table_off as u64 {
            return None;
        }
        Some(o as usize)
    }

    /// Byte disponibili da pos fino all'inizio della tabella degli offset.
    fn avail(&self, pos: usize) -> usize {
        self.table_off.saturating_sub(pos)
    }

    /// Lunghezza di un contenitore o di una stringa; pos avanza oltre marker ed eventuale intero.
    fn read_count(&self, pos: &mut usize, info: u8) -> Option<u64> {
        *pos += 1;
        if info != 0x0F {
            return Some(u64::from(info));
        }
        if self.avail(*pos) < 1 {
            return None;
        }
        let m = self.d[*pos];
        if m >> 4 != 0x1 || m & 0x0F > 3 {
            return None;
        }
        let n = 1usize << (m & 0x0F);
        if self.avail(*pos + 1) < n {
            return None;
        }
        let count = be(&self.d[*pos + 1..*pos + 1 + n]);
        *pos += 1 + n;
        Some(count)
    }

    fn charge(&mut self, bytes: u64) -> bool {
        self.nodes += 1;
        if self.nodes > MAX_NODES {
            return false;
        }
        self.out_bytes = self.out_bytes.saturating_add(bytes);
        self.out_bytes <= MAX_OUTPUT_BYTES
    }

    /// Riferimento i-esimo di un contenitore che inizia a pos.
    fn reference(&self, pos: usize, i: u64) -> u64 {
        let at = pos + i as usize * self.ref_size;
        be(&self.d[at..at + self.ref_size])
    }

    fn parse(&mut self, idx: u64, depth: u32) -> Option<Value> {
        if depth > MAX_DEPTH {
            return None;
        }
        let mut pos = self.object_offset(idx)?;
        let marker = self.d[pos];
        let (kind, info) = (marker >> 4, marker & 0x0F);
        match kind {
            0x0 => {
                if !self.charge(0) {
                    return None;
                }
                match marker {
                    0x08 | 0x00 | 0x0F => Some(Value::Bool(false)),
                    0x09 => Some(Value::Bool(true)),
                    _ => None,
                }
            }
            0x1 => {
                if info > 4 || !self.charge(0) {
                    return None;
                }
                let n = 1usize << info;
                if self.avail(pos + 1) < n {
                    return None;
                }
                // Interi da 16 byte: si tengono gli 8 byte meno significativi.
                let skip = n.saturating_sub(8);
                Some(Value::UInt(be(&self.d[pos + 1 + skip..pos + 1 + n])))
            }
            0x2 | 0x3 => {
                if (info != 2 && info != 3) || (kind == 0x3 && info != 3) || !self.charge(0) {
                    return None;
                }
                let n = 1usize << info;
                if self.avail(pos + 1) < n {
                    return None;
                }
                let bits = be(&self.d[pos + 1..pos + 1 + n]);
                Some(Value::Real(if n == 4 { f64::from(f32::from_bits(bits as u32)) } else { f64::from_bits(bits) }))
            }
            0x4 | 0x5 => {
                let count = self.read_count(&mut pos, info)?;
                if count > self.avail(pos) as u64 || !self.charge(count) {
                    return None;
                }
                let bytes = self.d[pos..pos + count as usize].to_vec();
                Some(if kind == 0x4 { Value::Data(bytes) } else { Value::Str(bytes) })
            }
            0x6 => {
                let count = self.read_count(&mut pos, info)?;
                if count > (self.avail(pos) / 2) as u64 || !self.charge(count * 3) {
                    return None;
                }
                let units: Vec<u32> =
                    self.d[pos..pos + 2 * count as usize].as_chunks::<2>().0.iter().map(|c| u32::from(c[0]) << 8 | u32::from(c[1])).collect();
                let mut s = Vec::with_capacity(units.len());
                let mut i = 0;
                while i < units.len() {
                    let mut cp = units[i];
                    if (0xD800..=0xDBFF).contains(&cp) && i + 1 < units.len() {
                        let lo = units[i + 1];
                        if (0xDC00..=0xDFFF).contains(&lo) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 1;
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (0xD800..=0xDFFF).contains(&cp) {
                        cp = 0xFFFD; // surrogato isolato: non è un carattere valido
                    }
                    push_utf8(&mut s, cp);
                    i += 1;
                }
                Some(Value::Str(s))
            }
            0x8 => {
                let n = usize::from(info) + 1;
                if n > 8 || self.avail(pos + 1) < n || !self.charge(0) {
                    return None;
                }
                Some(Value::UInt(be(&self.d[pos + 1..pos + 1 + n])))
            }
            0xA => {
                let count = self.read_count(&mut pos, info)?;
                if count > MAX_OBJECTS || count * self.ref_size as u64 > self.avail(pos) as u64 || !self.charge(0) {
                    return None;
                }
                let mut arr = Vec::with_capacity(count as usize);
                for i in 0..count {
                    arr.push(self.parse(self.reference(pos, i), depth + 1)?);
                }
                Some(Value::Array(arr))
            }
            0xD => {
                let count = self.read_count(&mut pos, info)?;
                if count > MAX_OBJECTS || 2 * count * self.ref_size as u64 > self.avail(pos) as u64 || !self.charge(0) {
                    return None;
                }
                let mut dict = Vec::with_capacity(count as usize);
                for i in 0..count {
                    let Value::Str(key) = self.parse(self.reference(pos, i), depth + 1)? else {
                        return None; // le chiavi devono essere stringhe
                    };
                    let val = self.parse(self.reference(pos, count + i), depth + 1)?;
                    dict.push((key, val));
                }
                Some(Value::Dict(dict))
            }
            _ => None, // set (0xC) e tipi riservati: iOS non li usa nei messaggi AirPlay
        }
    }
}

/// Plist interpretato, in forma piatta per l'interfaccia C: il nodo 0 è la radice, i figli
/// di un contenitore sono indici in `nodes`.
pub struct Flat {
    /// Nodi in ordine di visita (radice per prima).
    pub nodes: Vec<FlatNode>,
}

/// Un nodo di [`Flat`].
pub struct FlatNode {
    /// Valore scalare (per contenitori: Bool(false) segnaposto, vedi `kind`).
    pub kind: u8,
    /// Bool (0/1), intero o UID.
    pub uint: u64,
    /// Reale.
    pub real: f64,
    /// Byte di stringa o dati.
    pub bytes: Vec<u8>,
    /// Figli (indici) e, per i dizionari, le chiavi.
    pub kids: Vec<u32>,
    /// Chiavi dei figli di un dizionario (vuoto per gli array).
    pub keys: Vec<Vec<u8>>,
}

/// Tipi di nodo per l'interfaccia C (PLIST_* in rust_core.h).
pub mod kind {
    /// Booleano.
    pub const BOOL: u8 = 0;
    /// Intero.
    pub const UINT: u8 = 1;
    /// Reale.
    pub const REAL: u8 = 2;
    /// Stringa UTF-8 (o ASCII di iOS).
    pub const STRING: u8 = 3;
    /// Dati.
    pub const DATA: u8 = 4;
    /// Array.
    pub const ARRAY: u8 = 5;
    /// Dizionario.
    pub const DICT: u8 = 6;
}

impl Flat {
    /// Appiattisce un valore (i limiti del parser valgono anche qui: al più 8192 nodi).
    pub fn new(v: Value) -> Flat {
        let mut f = Flat { nodes: Vec::new() };
        f.add(v);
        f
    }

    fn add(&mut self, v: Value) -> u32 {
        let id = self.nodes.len() as u32;
        self.nodes.push(FlatNode { kind: kind::BOOL, uint: 0, real: 0.0, bytes: Vec::new(), kids: Vec::new(), keys: Vec::new() });
        let (k, uint, real, bytes, kids, keys) = match v {
            Value::Bool(b) => (kind::BOOL, u64::from(b), 0.0, Vec::new(), Vec::new(), Vec::new()),
            Value::UInt(n) => (kind::UINT, n, 0.0, Vec::new(), Vec::new(), Vec::new()),
            Value::Real(r) => (kind::REAL, 0, r, Vec::new(), Vec::new(), Vec::new()),
            Value::Str(s) => (kind::STRING, 0, 0.0, s, Vec::new(), Vec::new()),
            Value::Data(d) => (kind::DATA, 0, 0.0, d, Vec::new(), Vec::new()),
            Value::Array(a) => {
                let kids = a.into_iter().map(|c| self.add(c)).collect();
                (kind::ARRAY, 0, 0.0, Vec::new(), kids, Vec::new())
            }
            Value::Dict(d) => {
                let mut kids = Vec::with_capacity(d.len());
                let mut keys = Vec::with_capacity(d.len());
                for (key, c) in d {
                    keys.push(key);
                    kids.push(self.add(c));
                }
                (kind::DICT, 0, 0.0, Vec::new(), kids, keys)
            }
        };
        let n = &mut self.nodes[id as usize];
        (n.kind, n.uint, n.real, n.bytes, n.kids, n.keys) = (k, uint, real, bytes, kids, keys);
        id
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Plist minimale con gli oggetti dati (già codificati) e il top indicato.
    fn build(objects: &[Vec<u8>], top: u64, ref_size: u8) -> Vec<u8> {
        let mut out = b"bplist00".to_vec();
        let mut offs = Vec::new();
        for o in objects {
            offs.push(out.len());
            out.extend_from_slice(o);
        }
        let table = out.len();
        for o in &offs {
            out.extend_from_slice(&(*o as u16).to_be_bytes());
        }
        let mut t = [0u8; 32];
        t[6] = 2;
        t[7] = ref_size;
        t[8..16].copy_from_slice(&(objects.len() as u64).to_be_bytes());
        t[16..24].copy_from_slice(&top.to_be_bytes());
        t[24..32].copy_from_slice(&(table as u64).to_be_bytes());
        out.extend_from_slice(&t);
        out
    }

    #[test]
    fn dict_with_types() {
        // 0: dict {1:2, 3:4, 5:6}; 1 "a" 2 int 300; 3 "b" 4 utf16 "é😀"; 5 "c" 6 real 1.5
        let objs = vec![
            vec![0xD3, 1, 3, 5, 2, 4, 6],
            vec![0x51, b'a'],
            vec![0x11, 0x01, 0x2C],
            vec![0x51, b'b'],
            vec![0x63, 0x00, 0xE9, 0xD8, 0x3D, 0xDE, 0x00],
            vec![0x51, b'c'],
            vec![0x23, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0],
        ];
        let v = parse_binary(&build(&objs, 0, 1)).unwrap();
        assert_eq!(
            v,
            Value::Dict(vec![
                (b"a".to_vec(), Value::UInt(300)),
                (b"b".to_vec(), Value::Str("é😀".as_bytes().to_vec())),
                (b"c".to_vec(), Value::Real(1.5)),
            ])
        );
        let f = Flat::new(v);
        assert_eq!(f.nodes.len(), 4);
        assert_eq!(f.nodes[0].keys[1], b"b");
        assert_eq!(f.nodes[f.nodes[0].kids[0] as usize].uint, 300);
    }

    #[test]
    fn rejects_cycles_bad_keys_and_bombs() {
        // array che contiene se stesso: supera la profondità
        assert_eq!(parse_binary(&build(&[vec![0xA1, 0]], 0, 1)), None);
        // chiave non stringa
        assert_eq!(parse_binary(&build(&[vec![0xD1, 1, 1], vec![0x10, 5]], 0, 1)), None);
        // "billion laughs": 15 livelli di array da 15 riferimenti allo stesso figlio
        let mut objs: Vec<Vec<u8>> = (0..15u8).map(|i| { let mut o = vec![0xAF]; o.extend(std::iter::repeat_n(i + 1, 15)); o }).collect();
        objs.push(vec![0x09]);
        assert_eq!(parse_binary(&build(&objs, 0, 1)), None);
        // header, trailer e offset sbagliati
        let good = build(&[vec![0x09]], 0, 1);
        assert_eq!(parse_binary(&good), Some(Value::Bool(true)));
        let mut bad = good.clone();
        bad[0] = b'x';
        assert_eq!(parse_binary(&bad), None);
        assert_eq!(parse_binary(&good[..good.len() - 1]), None);
    }

    fn rng(s: &mut u64) -> u64 {
        *s ^= *s << 13;
        *s ^= *s >> 7;
        *s ^= *s << 17;
        *s
    }

    #[test]
    fn fuzz_no_panic() {
        let objs = vec![
            vec![0xD2, 1, 3, 2, 4],
            vec![0x51, b'k'],
            vec![0xA2, 5, 6],
            vec![0x5F, 0x10, 3, b'x', b'y', b'z'],
            vec![0x43, 1, 2, 3],
            vec![0x13, 0, 0, 0, 0, 0, 0, 0, 7],
            vec![0x22, 0x3F, 0x80, 0, 0],
        ];
        let base = build(&objs, 0, 1);
        assert!(parse_binary(&base).is_some());
        let mut st = 0x1234_5678_9ABC_DEF1u64;
        for _ in 0..300_000 {
            let mut m = base.clone();
            for _ in 0..1 + rng(&mut st) % 4 {
                let i = rng(&mut st) as usize % m.len();
                m[i] = rng(&mut st) as u8;
            }
            if let Some(v) = parse_binary(&m) {
                let _ = Flat::new(v);
            }
        }
    }
}
