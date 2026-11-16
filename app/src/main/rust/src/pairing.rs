//! Crittografia del protocollo AirPlay "legacy" (AirPlay 1 con pairing Ed25519), l'unico che
//! iOS usa verso un ricevitore che annuncia il bit 27 nelle feature flags.
//!
//! Catena delle chiavi (da UxPlay/RPiPlay):
//! 1. pair-setup: scambio delle chiavi pubbliche Ed25519 (nessun segreto).
//! 2. pair-verify: X25519 effimero -> segreto condiviso S. Le firme Ed25519 su
//!    (pk_ecdh_a || pk_ecdh_b) viaggiano cifrate con AES-128-CTR, chiave e IV =
//!    SHA-512("Pair-Verify-AES-Key" | "Pair-Verify-AES-IV" || S)[0..16].
//! 3. fp-setup: FairPlay (resta in C, playfair).
//! 4. SETUP: K = SHA-512(FairPlay(ekey) || S)[0..16].
//! 5. Video: AES-128-CTR, chiave/IV = `SHA-512("AirPlayStreamKey<id>" | "AirPlayStreamIV<id>" || K)[0..16]`.
//! 6. Audio: AES-128-CBC con chiave K e IV "eiv", ripartito a ogni pacchetto.
//!
//! Pairing con PIN: SRP-6a ([`crate::srp`]), poi AES-GCM (IV di 16 byte, ultimo byte
//! incrementato prima di decifrare e di nuovo prima di cifrare: il "nonce" non documentato
//! che iOS si aspetta, vedi UxPlay pairing.c).
//!
//! Primitive: ed25519-dalek, x25519-dalek, sha2, aes-gcm. Segreti azzerati alla fine
//! dell'uso; firme verificate in modo stretto (niente chiavi di ordine basso, S canonico).

use crate::aes128::Ctr;
use crate::srp;
use aes_gcm::aead::{AeadInOut, KeyInit};
use aes_gcm::aead::consts::U16;
use aes_gcm::{AesGcm, Nonce, Tag};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use sha2::{Digest, Sha512};
use x25519_dalek::{PublicKey, StaticSecret};
use zeroize::{Zeroize, Zeroizing};

/// Casualità dal kernel (getrandom).
pub fn random(buf: &mut [u8]) -> bool {
    getrandom::fill(buf).is_ok()
}

/// Intero uniforme in [0, n) senza bias del modulo (n > 0).
pub fn random_below(n: u32) -> Option<u32> {
    let limit = u32::MAX - u32::MAX % n;
    loop {
        let mut b = [0u8; 4];
        if !random(&mut b) {
            return None;
        }
        let v = u32::from_le_bytes(b);
        if v < limit {
            return Some(v % n);
        }
    }
}

/// SHA-512(a || b), primi 16 byte.
pub fn sha512_prefix16(a: &[u8], b: &[u8]) -> [u8; 16] {
    let mut h = Sha512::new();
    h.update(a);
    h.update(b);
    let mut full: [u8; 64] = h.finalize().into();
    let mut out = [0u8; 16];
    out.copy_from_slice(&full[..16]);
    full.zeroize();
    out
}

/// K = SHA-512(fpKey || S)[0..16] (punto 4).
pub fn derive_session_key(fp_key: &[u8; 16], shared: &[u8; 32]) -> [u8; 16] {
    sha512_prefix16(fp_key, shared)
}

/// Chiave e IV del flusso video (punto 5).
pub fn derive_mirror_key(session_key: &[u8; 16], stream_connection_id: u64) -> ([u8; 16], [u8; 16]) {
    let k = format!("AirPlayStreamKey{stream_connection_id}");
    let i = format!("AirPlayStreamIV{stream_connection_id}");
    (sha512_prefix16(k.as_bytes(), session_key), sha512_prefix16(i.as_bytes(), session_key))
}

/// Identità Ed25519 della TV (dal seed salvato da Kotlin). Il segreto è azzerato al rilascio.
pub struct Identity {
    key: SigningKey,
}

impl Identity {
    /// Coppia di chiavi dal seed RFC 8032 (stessa di libsodium crypto_sign_seed_keypair).
    pub fn from_seed(seed: &[u8; 32]) -> Identity {
        Identity { key: SigningKey::from_bytes(seed) }
    }

    /// Chiave pubblica.
    pub fn public(&self) -> [u8; 32] {
        self.key.verifying_key().to_bytes()
    }

    /// Firma deterministica.
    pub fn sign(&self, msg: &[u8]) -> [u8; 64] {
        self.key.sign(msg).to_bytes()
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    SetupDone,
    Step1Done,
    Verified,
}

/// Stato del pairing di una connessione RTSP.
pub struct Pairing {
    state: State,
    our_ecdh_pk: [u8; 32],
    their_ecdh_pk: [u8; 32],
    their_ed_pk: [u8; 32],
    shared: Zeroizing<[u8; 32]>,
    srp: Option<Box<srp::Server>>,
    pin_proven: bool,
    pin_done: bool,
    pin_client_pk: [u8; 32],
}

impl Default for Pairing {
    fn default() -> Self {
        Self::new()
    }
}

impl Pairing {
    /// Nessun pairing.
    pub fn new() -> Pairing {
        Pairing {
            state: State::Idle,
            our_ecdh_pk: [0; 32],
            their_ecdh_pk: [0; 32],
            their_ed_pk: [0; 32],
            shared: Zeroizing::new([0; 32]),
            srp: None,
            pin_proven: false,
            pin_done: false,
            pin_client_pk: [0; 32],
        }
    }

    fn wipe(&mut self) {
        self.shared.zeroize();
    }

    fn key_iv(&self) -> (Zeroizing<[u8; 16]>, Zeroizing<[u8; 16]>) {
        (
            Zeroizing::new(sha512_prefix16(b"Pair-Verify-AES-Key", &*self.shared)),
            Zeroizing::new(sha512_prefix16(b"Pair-Verify-AES-IV", &*self.shared)),
        )
    }

    /// POST /pair-setup: body = chiave pubblica Ed25519 del client (32 byte). Quella che
    /// conta arriva in pair-verify: qui basta la dimensione. Ricominciare invalida il
    /// pairing precedente sulla connessione.
    pub fn setup(&mut self, body: &[u8]) -> bool {
        if body.len() != 32 {
            return false;
        }
        self.wipe();
        self.state = State::SetupDone;
        true
    }

    /// POST /pair-verify, fase 1 (`body[0] == 1`): pk X25519 nostra (32) + firma cifrata (64).
    /// Accettata anche senza pair-setup: iOS che ha già associato la TV parte da qui
    /// (iOS 27.2); l'autenticazione vera è la firma della fase 2.
    pub fn verify_step1(&mut self, id: &Identity, body: &[u8]) -> Option<[u8; 96]> {
        if body.len() != 4 + 32 + 32 || body[0] != 1 {
            return None;
        }
        self.their_ecdh_pk.copy_from_slice(&body[4..36]);
        self.their_ed_pk.copy_from_slice(&body[36..68]);

        let mut seed = Zeroizing::new([0u8; 32]);
        if !random(&mut seed[..]) {
            return None;
        }
        let secret = StaticSecret::from(*seed);
        self.our_ecdh_pk = PublicKey::from(&secret).to_bytes();
        let shared = secret.diffie_hellman(&PublicKey::from(self.their_ecdh_pk));
        // Segreto tutto zero: chiave del client di ordine basso, S prevedibile.
        if !shared.was_contributory() {
            self.wipe();
            self.state = State::Idle;
            return None;
        }
        self.shared.copy_from_slice(shared.as_bytes());
        drop(secret); // serve solo per S

        let mut msg = [0u8; 64];
        msg[..32].copy_from_slice(&self.our_ecdh_pk);
        msg[32..].copy_from_slice(&self.their_ecdh_pk);
        let mut sig = id.sign(&msg);
        let (key, iv) = self.key_iv();
        Ctr::new(&key, &iv).apply(&mut sig);

        let mut out = [0u8; 96];
        out[..32].copy_from_slice(&self.our_ecdh_pk);
        out[32..].copy_from_slice(&sig);
        self.state = State::Step1Done;
        Some(out)
    }

    /// POST /pair-verify, fase 2 (`body[0] == 0`): verifica la firma del client.
    pub fn verify_step2(&mut self, body: &[u8]) -> bool {
        if self.state != State::Step1Done || body.len() != 4 + 64 || body[0] != 0 {
            return false;
        }
        let (key, iv) = self.key_iv();
        // Il client prosegue lo stesso keystream usato per la nostra firma: si saltano 64 byte.
        let mut ctr = Ctr::new(&key, &iv);
        ctr.skip(64);
        let mut sig = [0u8; 64];
        sig.copy_from_slice(&body[4..]);
        ctr.apply(&mut sig);

        let mut msg = [0u8; 64];
        msg[..32].copy_from_slice(&self.their_ecdh_pk);
        msg[32..].copy_from_slice(&self.our_ecdh_pk);
        let ok = VerifyingKey::from_bytes(&self.their_ed_pk)
            .is_ok_and(|vk| vk.verify_strict(&msg, &Signature::from_bytes(&sig)).is_ok());
        if !ok {
            self.wipe();
            self.state = State::Idle;
            return false;
        }
        self.state = State::Verified;
        true
    }

    /// pair-verify completato.
    pub fn verified(&self) -> bool {
        self.state == State::Verified
    }

    /// Chiave Ed25519 del client ricevuta in pair-verify fase 1.
    pub fn client_ed_key(&self) -> [u8; 32] {
        self.their_ed_pk
    }

    /// K di sessione dalla chiave FairPlay (punto 4); None senza pair-verify.
    pub fn session_key(&self, fp_key: &[u8; 16]) -> Option<[u8; 16]> {
        self.verified().then(|| derive_session_key(fp_key, &self.shared))
    }

    /// pair-setup-pin fase 1: salt e B. Ogni nuova fase 1 annulla il tentativo precedente.
    pub fn pin_begin(&mut self, user: &[u8], pin: &[u8]) -> Option<([u8; 16], Vec<u8>)> {
        let mut s = Box::new(srp::Server::new());
        self.pin_proven = false;
        self.pin_done = false;
        let r = s.begin(user, pin, &mut |b: &mut [u8]| random(b));
        self.srp = Some(s);
        r
    }

    /// pair-setup-pin fase 2: prova del client -> prova della TV. PIN sbagliato: tentativo
    /// consumato.
    pub fn pin_proof(&mut self, a: &[u8], m1: &[u8]) -> Option<[u8; srp::PROOF_SIZE]> {
        let s = self.srp.as_mut()?;
        match s.verify(a, m1) {
            Some(m2) => {
                self.pin_proven = true;
                Some(m2)
            }
            None => {
                self.srp = None;
                None
            }
        }
    }

    /// pair-setup-pin fase 3: decifra (AES-GCM) la chiave Ed25519 del client e cifra la nostra.
    pub fn pin_exchange(&mut self, id: &Identity, epk: &[u8], tag: &[u8]) -> Option<([u8; 32], [u8; 16])> {
        let s = self.srp.take()?;
        let proven = std::mem::replace(&mut self.pin_proven, false);
        if !proven || epk.len() != 32 || tag.len() != 16 {
            return None;
        }
        let k = s.session_key()?;
        let key = Zeroizing::new(sha512_prefix16(b"Pair-Setup-AES-Key", k));
        let mut iv = Zeroizing::new(sha512_prefix16(b"Pair-Setup-AES-IV", k));
        drop(s); // K non serve più
        iv[15] = iv[15].wrapping_add(1);

        let gcm = AesGcm::<aes::Aes128, U16>::new(&(*key).into());
        let mut client_pk = [0u8; 32];
        client_pk.copy_from_slice(epk);
        // decrypt verifica il tag prima di restituire il testo in chiaro: una chiave
        // manomessa non arriva mai al resto del codice.
        gcm.decrypt_inout_detached(&Nonce::<U16>::from(*iv), &[], client_pk.as_mut_slice().into(), &Tag::<U16>::try_from(tag).ok()?)
            .ok()?;
        iv[15] = iv[15].wrapping_add(1);
        let mut out = id.public();
        let out_tag = gcm.encrypt_inout_detached(&Nonce::<U16>::from(*iv), &[], out.as_mut_slice().into()).ok()?;

        self.pin_client_pk = client_pk;
        self.pin_done = true;
        self.state = State::SetupDone; // ora può seguire il normale pair-verify
        Some((out, out_tag.into()))
    }

    /// true dopo un pair-setup-pin completo su questa connessione.
    pub fn pin_authenticated(&self) -> bool {
        self.pin_done
    }

    /// Chiave Ed25519 del client autenticata con il PIN.
    pub fn pin_client_key(&self) -> [u8; 32] {
        self.pin_client_pk
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ed25519_rfc8032_vector() {
        // RFC 8032 §7.1, TEST 1
        let seed: [u8; 32] = core::array::from_fn(|i| {
            u8::from_str_radix(&"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"[2 * i..2 * i + 2], 16).unwrap()
        });
        let id = Identity::from_seed(&seed);
        assert_eq!(id.public()[..4], [0xd7, 0x5a, 0x98, 0x01]);
        assert_eq!(id.sign(b"")[..4], [0xe5, 0x56, 0x43, 0x00]);
    }

    #[test]
    fn mirror_key_format() {
        // Stessa stringa del C++: "AirPlayStreamKey" + decimale senza segno.
        let (k, i) = derive_mirror_key(&[1; 16], u64::MAX);
        assert_eq!(k, sha512_prefix16(b"AirPlayStreamKey18446744073709551615", &[1; 16]));
        assert_eq!(i, sha512_prefix16(b"AirPlayStreamIV18446744073709551615", &[1; 16]));
    }

    /// pair-verify completo con un client simulato (stesse primitive, ruoli invertiti).
    #[test]
    fn pair_verify_roundtrip_and_tamper() {
        let tv = Identity::from_seed(&[7; 32]);
        let phone = Identity::from_seed(&[9; 32]);
        for tamper in [false, true] {
            let mut p = Pairing::new();
            assert!(p.setup(&phone.public()));
            let xs = StaticSecret::from([5u8; 32]);
            let xpk = PublicKey::from(&xs).to_bytes();
            let mut body = vec![1, 0, 0, 0];
            body.extend_from_slice(&xpk);
            body.extend_from_slice(&phone.public());
            let out = p.verify_step1(&tv, &body).unwrap();
            let spk: [u8; 32] = out[..32].try_into().unwrap();
            let shared = xs.diffie_hellman(&PublicKey::from(spk));
            let key = sha512_prefix16(b"Pair-Verify-AES-Key", shared.as_bytes());
            let iv = sha512_prefix16(b"Pair-Verify-AES-IV", shared.as_bytes());
            let mut ctr = Ctr::new(&key, &iv);
            let mut ssig: [u8; 64] = out[32..].try_into().unwrap();
            ctr.apply(&mut ssig);
            let mut m = [0u8; 64];
            m[..32].copy_from_slice(&spk);
            m[32..].copy_from_slice(&xpk);
            VerifyingKey::from_bytes(&tv.public()).unwrap().verify_strict(&m, &Signature::from_bytes(&ssig)).unwrap();
            m[..32].copy_from_slice(&xpk);
            m[32..].copy_from_slice(&spk);
            let mut my = phone.sign(&m);
            if tamper {
                my[3] ^= 1;
            }
            ctr.apply(&mut my);
            let mut b2 = vec![0, 0, 0, 0];
            b2.extend_from_slice(&my);
            assert_eq!(p.verify_step2(&b2), !tamper);
            assert_eq!(p.verified(), !tamper);
            assert_eq!(p.session_key(&[1; 16]).is_some(), !tamper);
        }
        // chiave X25519 di ordine basso (tutto zero): rifiutata
        let mut p = Pairing::new();
        let mut body = vec![1, 0, 0, 0];
        body.extend_from_slice(&[0; 64]);
        assert!(p.verify_step1(&tv, &body).is_none());
        assert!(!p.verify_step2(&[0; 68]));
    }

    #[test]
    fn random_below_is_in_range() {
        for _ in 0..1000 {
            assert!(random_below(10_000).unwrap() < 10_000);
        }
    }
}
