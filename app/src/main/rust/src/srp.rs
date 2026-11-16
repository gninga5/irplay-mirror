//! Lato TV del pairing con PIN di AirPlay ("pair-setup-pin"): SRP-6a. L'iPhone dimostra di
//! conoscere il PIN mostrato sullo schermo senza trasmetterlo; le due parti ottengono una
//! chiave K con cui scambiarsi le chiavi Ed25519.
//!
//! Parametri di Apple (da UxPlay srp.c, APPLE_VARIANT, rfc5054_compat = 1):
//! - gruppo RFC 5054 a 2048 bit, g = 2, SHA-1;
//! - k = H(N | PAD(g)), u = H(PAD(A) | PAD(B));
//! - x = H(s | H(I ":" PIN)), I = identificativo inviato dall'iPhone;
//! - K = H(S | 00000000) | H(S | 00000001), 40 byte, usata intera in M1 e M2.
//!
//! Codifica dei numeri negli hash: "bytes(n)" big-endian senza zeri iniziali, "PAD(n)"
//! portato a 256 byte (solo in k e u). Un solo byte diverso renderebbe la prova
//! dell'iPhone non verificabile.
//!
//! Aritmetica di crypto-bigint in forma di Montgomery: le esponenziazioni sono a tempo
//! costante rispetto all'esponente (b e x sono segreti). Controlli: A mod N != 0, u != 0,
//! lunghezze; confronto della prova a tempo costante; segreti azzerati; un tentativo per
//! istanza.

use crypto_bigint::modular::{FixedMontyForm, FixedMontyParams};
use crypto_bigint::{Odd, U256, U2048};
use sha1::{Digest, Sha1};
use subtle::ConstantTimeEq;
use zeroize::{Zeroize, Zeroizing};

/// Dimensione della chiave di sessione K (2 × SHA-1).
pub const SESSION_KEY_SIZE: usize = 40;
/// Dimensione delle prove M1/M2 (SHA-1).
pub const PROOF_SIZE: usize = 20;
const N_BYTES: usize = 256;
const MAX_USER: usize = 64;

/// RFC 5054, appendice A, gruppo a 2048 bit.
const N_HEX: &str = concat!(
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4",
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60",
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF",
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907",
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861",
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB",
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73"
);

fn n() -> U2048 {
    U2048::from_be_hex(N_HEX)
}

fn padded(v: &U2048) -> Zeroizing<[u8; N_BYTES]> {
    let mut out = Zeroizing::new([0u8; N_BYTES]);
    out.copy_from_slice(v.to_be_bytes().as_ref());
    out
}

/// big-endian senza zeri iniziali (BN_bn2bin).
fn minimal(v: &U2048) -> Zeroizing<Vec<u8>> {
    let p = padded(v);
    let first = p.iter().position(|&b| b != 0).unwrap_or(N_BYTES);
    Zeroizing::new(p[first..].to_vec())
}

/// Hash SHA-1 come intero a 256 bit (esponenti x e u).
fn hash_u256(h: &[u8; 20]) -> U256 {
    let mut b = [0u8; 32];
    b[12..].copy_from_slice(h);
    let v = U256::from_be_slice(&b);
    b.zeroize();
    v
}

fn sha1(parts: &[&[u8]]) -> [u8; 20] {
    let mut h = Sha1::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

/// Server SRP per un tentativo.
pub struct Server {
    params: FixedMontyParams<{ U2048::LIMBS }>,
    k: U2048,
    v: Option<FixedMontyForm<{ U2048::LIMBS }>>,
    b: Zeroizing<U256>,
    public_b: U2048,
    user: Vec<u8>,
    salt: [u8; 16],
    key: Zeroizing<[u8; SESSION_KEY_SIZE]>,
    begun: bool,
    verified: bool,
}

impl Default for Server {
    fn default() -> Self {
        Self::new()
    }
}

impl Server {
    /// Nuovo tentativo.
    pub fn new() -> Server {
        let n = n();
        let params = FixedMontyParams::new_vartime(Odd::new(n).expect("N è dispari"));
        let g = U2048::from_u8(2);
        let k = hash_u256(&sha1(&[&padded(&n)[..], &padded(&g)[..]])).resize::<{ U2048::LIMBS }>();
        Server {
            params,
            k,
            v: None,
            b: Zeroizing::new(U256::ZERO),
            public_b: U2048::ZERO,
            user: Vec::new(),
            salt: [0; 16],
            key: Zeroizing::new([0; SESSION_KEY_SIZE]),
            begun: false,
            verified: false,
        }
    }

    fn g(&self) -> FixedMontyForm<{ U2048::LIMBS }> {
        FixedMontyForm::new(&U2048::from_u8(2), &self.params)
    }

    /// Fase 1: salt (16 byte) e B. `random` riempie i buffer di casualità (getrandom
    /// nell'app, deterministica nei test).
    pub fn begin(&mut self, user: &[u8], pin: &[u8], random: &mut dyn FnMut(&mut [u8]) -> bool) -> Option<([u8; 16], Vec<u8>)> {
        if self.begun || user.is_empty() || user.len() > MAX_USER || pin.len() != 4 {
            return None;
        }
        self.user = user.to_vec();
        // Salt con il primo byte non nullo: bytes(s) ha sempre 16 byte, come vuole UxPlay
        // (e quindi iOS).
        loop {
            if !random(&mut self.salt) {
                return None;
            }
            if self.salt[0] != 0 {
                break;
            }
        }
        let inner = Zeroizing::new(sha1(&[user, b":", pin]));
        let x = Zeroizing::new(hash_u256(&sha1(&[&self.salt, &inner[..]])));
        let g = self.g();
        let v = g.pow(&*x);
        let mut b = Zeroizing::new([0u8; 32]);
        if !random(&mut b[..]) {
            return None;
        }
        *self.b = U256::from_be_slice(&b[..]);
        // B = (k*v + g^b) mod N
        let kv = FixedMontyForm::new(&self.k, &self.params).mul(&v);
        let big_b = kv.add(&g.pow(&*self.b)).retrieve();
        if bool::from(big_b.is_zero()) {
            return None;
        }
        self.v = Some(v);
        self.public_b = big_b;
        self.begun = true;
        Some((self.salt, minimal(&big_b).to_vec()))
    }

    /// Fase 2: verifica la prova M1 del client; se giusta restituisce M2 e K diventa valida.
    pub fn verify(&mut self, a_bytes: &[u8], m1: &[u8]) -> Option<[u8; PROOF_SIZE]> {
        // Una sola verifica per istanza: lo stesso b non serve a più tentativi.
        if !self.begun || self.verified {
            return None;
        }
        self.begun = false;
        if a_bytes.is_empty() || a_bytes.len() > N_BYTES || m1.len() != PROOF_SIZE {
            return None;
        }
        let v = self.v.take()?;
        let mut buf = [0u8; N_BYTES];
        buf[N_BYTES - a_bytes.len()..].copy_from_slice(a_bytes);
        let a = U2048::from_be_slice(&buf);
        // SRP-6a: con A ≡ 0 (mod N) il segreto S sarebbe 0 e un client potrebbe autenticarsi
        // senza conoscere il PIN.
        let a_mod = FixedMontyForm::new(&a, &self.params);
        if bool::from(a_mod.retrieve().is_zero()) {
            return None;
        }
        let u = hash_u256(&sha1(&[&padded(&a)[..], &padded(&self.public_b)[..]]));
        if bool::from(u.is_zero()) {
            return None;
        }
        // S = (A * v^u)^b mod N
        let s = Zeroizing::new(a_mod.mul(&v.pow(&u)).pow(&*self.b).retrieve());
        let s_bytes = minimal(&s);
        for i in 0..2u8 {
            let h = sha1(&[&s_bytes[..], &[0, 0, 0, i]]);
            self.key[20 * usize::from(i)..20 * usize::from(i) + 20].copy_from_slice(&h);
        }
        // M = H( (H(N) xor H(g)) | H(I) | s | A | B | K )
        let mut hn = sha1(&[&minimal(&n())[..]]);
        let hg = sha1(&[&[2u8]]);
        for (x, y) in hn.iter_mut().zip(hg) {
            *x ^= y;
        }
        let a_min = minimal(&a);
        let expected = sha1(&[&hn, &sha1(&[&self.user]), &self.salt, &a_min[..], &minimal(&self.public_b)[..], &self.key[..]]);
        // Confronto a tempo costante: uno che si ferma al primo byte diverso rivelerebbe,
        // misurando i tempi, quanti byte della prova sono giusti.
        if !bool::from(expected.ct_eq(m1)) {
            self.key.zeroize();
            return None;
        }
        self.verified = true;
        Some(sha1(&[&a_min[..], &expected, &self.key[..]]))
    }

    /// Chiave K; valida solo dopo verify() riuscita.
    pub fn session_key(&self) -> Option<&[u8; SESSION_KEY_SIZE]> {
        self.verified.then_some(&*self.key)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Client SRP minimale (lato iPhone) con la stessa variante, per provare il server.
    fn client(user: &[u8], pin: &[u8], salt: &[u8; 16], b: &[u8], a_secret: u8) -> (Vec<u8>, [u8; 20], [u8; 40]) {
        let n = n();
        let params = FixedMontyParams::new_vartime(Odd::new(n).unwrap());
        let g = FixedMontyForm::new(&U2048::from_u8(2), &params);
        let k = hash_u256(&sha1(&[&padded(&n)[..], &padded(&U2048::from_u8(2))[..]])).resize::<{ U2048::LIMBS }>();
        let a = U256::from_u8(a_secret);
        let big_a = g.pow(&a).retrieve();
        let mut bb = [0u8; N_BYTES];
        bb[N_BYTES - b.len()..].copy_from_slice(b);
        let big_b = U2048::from_be_slice(&bb);
        let u = hash_u256(&sha1(&[&padded(&big_a)[..], &padded(&big_b)[..]]));
        let x = hash_u256(&sha1(&[salt, &sha1(&[user, b":", pin])]));
        // S = (B - k*g^x)^(a + u*x)
        let kgx = FixedMontyForm::new(&k, &params).mul(&g.pow(&x));
        let base = FixedMontyForm::new(&big_b, &params).sub(&kgx);
        let s = base.pow(&a).mul(&base.pow(&u).pow(&x)).retrieve();
        let sm = minimal(&s);
        let mut key = [0u8; 40];
        key[..20].copy_from_slice(&sha1(&[&sm[..], &[0, 0, 0, 0]]));
        key[20..].copy_from_slice(&sha1(&[&sm[..], &[0, 0, 0, 1]]));
        let mut hn = sha1(&[&minimal(&n)[..]]);
        for (x, y) in hn.iter_mut().zip(sha1(&[&[2u8]])) {
            *x ^= y;
        }
        let a_min = minimal(&big_a).to_vec();
        let m1 = sha1(&[&hn, &sha1(&[user]), salt, &a_min, b, &key]);
        (a_min, m1, key)
    }

    fn counter_rng() -> impl FnMut(&mut [u8]) -> bool {
        let mut c = 0u8;
        move |buf: &mut [u8]| {
            for x in buf.iter_mut() {
                c = c.wrapping_mul(31).wrapping_add(17);
                *x = c;
            }
            true
        }
    }

    #[test]
    fn handshake_with_right_and_wrong_pin() {
        let mut s = Server::new();
        let (salt, b) = s.begin(b"iPhone", b"1234", &mut counter_rng()).unwrap();
        assert_ne!(salt[0], 0);
        let (a, m1, key) = client(b"iPhone", b"1234", &salt, &b, 77);
        let m2 = s.verify(&a, &m1).unwrap();
        assert_eq!(s.session_key().unwrap(), &key);
        assert_eq!(m2, sha1(&[&a, &m1, &key]));
        // un secondo verify non è ammesso
        assert!(s.verify(&a, &m1).is_none());

        let mut s = Server::new();
        let (salt, b) = s.begin(b"iPhone", b"1234", &mut counter_rng()).unwrap();
        let (a, m1, _) = client(b"iPhone", b"9999", &salt, &b, 5);
        assert!(s.verify(&a, &m1).is_none());
        assert!(s.session_key().is_none());
    }

    /// Tempi su un dispositivo (cargo test -- --ignored --nocapture srp_timing).
    #[test]
    #[ignore]
    fn srp_timing() {
        let t = std::time::Instant::now();
        let mut s = Server::new();
        let (salt, b) = s.begin(b"iPhone", b"1234", &mut counter_rng()).unwrap();
        let t1 = t.elapsed();
        let (a, m1, _) = client(b"iPhone", b"1234", &salt, &b, 77);
        let t = std::time::Instant::now();
        assert!(s.verify(&a, &m1).is_some());
        eprintln!("SRP: fase 1 {:?}, fase 2 {:?}", t1, t.elapsed());
    }

    #[test]
    fn rejects_bad_inputs() {
        let mut s = Server::new();
        assert!(s.begin(b"", b"1234", &mut counter_rng()).is_none());
        assert!(s.begin(&[b'a'; 65], b"1234", &mut counter_rng()).is_none());
        assert!(s.begin(b"x", b"123", &mut counter_rng()).is_none());
        assert!(s.begin(b"x", b"1234", &mut |_: &mut [u8]| false).is_none());
        // A = 0 e A = N: rifiutati (S sarebbe 0)
        for bad in [vec![0u8], padded(&n()).to_vec()] {
            let mut s = Server::new();
            s.begin(b"x", b"1234", &mut counter_rng()).unwrap();
            assert!(s.verify(&bad, &[0; 20]).is_none());
        }
        let mut s = Server::new();
        s.begin(b"x", b"1234", &mut counter_rng()).unwrap();
        assert!(s.verify(&[1; 257], &[0; 20]).is_none());
    }
}
