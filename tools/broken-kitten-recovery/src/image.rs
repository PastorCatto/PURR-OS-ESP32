//! The PURR image container: layout and verification. Mirrors purr_verify.c (PurrOS/components/coreos); the layout is
//! purr_image_header_t, 173 bytes, little-endian, and the signature (ECDSA P-256, raw r||s) covers the 109 bytes before it.

use p256::ecdsa::{signature::hazmat::PrehashVerifier, Signature, VerifyingKey};
use sha2::{Digest, Sha256};

use crate::keys::Key;

pub const HEADER_SIZE: usize = 173;
const SIGNED_LEN: usize = HEADER_SIZE - 64;
const MAGIC: u32 = 0x5055_5252;

pub const IMG_OS: u8 = 1;
pub const IMG_RECOVERY: u8 = 2;
pub const IMG_MODULE: u8 = 3;
pub const IMG_APP: u8 = 4;

pub const CHIP_ESP32S3: u16 = 9;
pub const CHIP_ANY: u16 = 0xFFFF;

#[derive(Debug, Clone, PartialEq)]
pub struct Header {
    pub chip_id: u16,
    pub image_type: u8,
    pub key_id: u8,
    pub flags: u16,
    pub name: String,
    pub version: String,
    pub payload_offset: u32,
    pub payload_size: u32,
    pub payload_sha256: [u8; 32],
}

#[derive(Debug, PartialEq)]
pub enum VerifyError {
    TooShort,
    BadMagic,
    BadHeaderVersion,
    BadLayout,
    BadType,
    BadChip,
    NoKey(u8),
    RoleMismatch,
    BadHash,
    BadSignature,
}

impl std::fmt::Display for VerifyError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            VerifyError::TooShort => write!(f, "too short to be a PURR image"),
            VerifyError::BadMagic => write!(f, "not a PURR image"),
            VerifyError::BadHeaderVersion => write!(f, "unknown header version"),
            VerifyError::BadLayout => write!(f, "sizes or offsets outside the image"),
            VerifyError::BadType => write!(f, "unknown image type"),
            VerifyError::BadChip => write!(f, "built for another chip"),
            VerifyError::NoKey(id) => write!(f, "signed with key {} which is not an official key", id),
            VerifyError::RoleMismatch => write!(f, "the signing key's role may not sign this kind of image"),
            VerifyError::BadHash => write!(f, "the contents do not match the hash in the header"),
            VerifyError::BadSignature => write!(f, "the signature is not valid"),
        }
    }
}

fn text(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).into_owned()
}

fn u16le(b: &[u8], at: usize) -> u16 { u16::from_le_bytes([b[at], b[at + 1]]) }
fn u32le(b: &[u8], at: usize) -> u32 { u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]]) }

/// Which key roles may sign which images (purr_role_may_sign in purr_keybag.c).
fn role_may_sign(role: u8, image_type: u8, subtype: u8) -> bool {
    const BOOT: u8 = 1;
    const SYSTEM: u8 = 2;
    const OWNER: u8 = 3;
    const DEVELOPER: u8 = 4;
    const VENDOR: u8 = 5;
    match image_type {
        IMG_OS | IMG_RECOVERY => role == BOOT,
        IMG_MODULE => match subtype {
            1 | 2 | 6 | 8 | 9 => role == BOOT,                   // kernel, coreos, bootpkg, loader, bootloader
            3 | 4 | 7 => role == SYSTEM,                         // app manager, runtime, dev bundle
            5 => matches!(role, SYSTEM | VENDOR | DEVELOPER | OWNER),   // driver
            _ => false,
        },
        IMG_APP => matches!(role, SYSTEM | OWNER | DEVELOPER | VENDOR),
        _ => false,
    }
}

/// The header and layout checks that need no key: magic, versions, type, chip, payload inside the file.
fn parse(data: &[u8], chip: u16) -> Result<Header, VerifyError> {
    if data.len() < HEADER_SIZE {
        return Err(VerifyError::TooShort);
    }
    if u32le(data, 0) != MAGIC {
        return Err(VerifyError::BadMagic);
    }
    if data[4] != 1 {
        return Err(VerifyError::BadHeaderVersion);
    }
    let header_size = u16le(data, 5) as usize;
    if header_size < HEADER_SIZE || header_size > data.len() {
        return Err(VerifyError::BadLayout);
    }
    let h = Header {
        chip_id: u16le(data, 7),
        image_type: data[9],
        key_id: data[10],
        flags: u16le(data, 11),
        name: text(&data[13..45]),
        version: text(&data[45..57]),
        payload_offset: u32le(data, 69),
        payload_size: u32le(data, 73),
        payload_sha256: data[77..109].try_into().unwrap(),
    };
    if !(IMG_OS..=IMG_APP).contains(&h.image_type) {
        return Err(VerifyError::BadType);
    }
    if h.chip_id != chip && !(h.chip_id == CHIP_ANY && h.image_type == IMG_APP) {
        return Err(VerifyError::BadChip);
    }
    let end = h.payload_offset as u64 + h.payload_size as u64;
    if (h.payload_offset as usize) < header_size || end > data.len() as u64 {
        return Err(VerifyError::BadLayout);
    }
    Ok(h)
}

fn payload_matches(data: &[u8], h: &Header) -> bool {
    let end = h.payload_offset as usize + h.payload_size as usize;
    Sha256::digest(&data[h.payload_offset as usize..end])[..] == h.payload_sha256[..]
}

/// Checks a whole image against `keys`: layout, key and role, the payload hash, then the signature. `chip` is the chip the
/// image must be for. Returns the header on success.
pub fn verify(data: &[u8], chip: u16, keys: &[Key]) -> Result<Header, VerifyError> {
    let h = parse(data, chip)?;
    let key = keys.iter().find(|k| k.id == h.key_id).ok_or(VerifyError::NoKey(h.key_id))?;
    if !role_may_sign(key.role, h.image_type, (h.flags & 0xFF) as u8) {
        return Err(VerifyError::RoleMismatch);
    }
    if !payload_matches(data, &h) {
        return Err(VerifyError::BadHash);
    }
    let digest = Sha256::digest(&data[..SIGNED_LEN]);
    let mut sec1 = [0u8; 65];
    sec1[0] = 0x04;
    sec1[1..].copy_from_slice(&key.public);
    let vk = VerifyingKey::from_sec1_bytes(&sec1).map_err(|_| VerifyError::BadSignature)?;
    let sig = Signature::from_slice(&data[SIGNED_LEN..HEADER_SIZE]).map_err(|_| VerifyError::BadSignature)?;
    vk.verify_prehash(&digest, &sig).map_err(|_| VerifyError::BadSignature)?;
    Ok(h)
}

#[derive(Debug, PartialEq)]
pub enum Trust {
    Signed(u8),
    Unsigned,
}

pub const MOD_DRIVER: u8 = 5;

/// A driver file for developer mode: signed by an official key if it is, otherwise unsigned, which a device only loads in
/// developer mode. Either way the file has to be a well-formed PURR module of the driver kind for this chip with an intact payload,
/// so a damaged or mislabelled file is refused here, not left for the device to find out.
pub fn inspect_driver(data: &[u8], chip: u16, keys: &[Key]) -> Result<(Header, Trust), VerifyError> {
    match verify(data, chip, keys) {
        Ok(h) => {
            if h.image_type != IMG_MODULE || (h.flags & 0xFF) as u8 != MOD_DRIVER {
                return Err(VerifyError::BadType);
            }
            let id = h.key_id;
            Ok((h, Trust::Signed(id)))
        }
        Err(VerifyError::NoKey(_)) | Err(VerifyError::RoleMismatch) | Err(VerifyError::BadSignature) => {
            let h = parse(data, chip)?;
            if h.image_type != IMG_MODULE || (h.flags & 0xFF) as u8 != MOD_DRIVER {
                return Err(VerifyError::BadType);
            }
            if !payload_matches(data, &h) {
                return Err(VerifyError::BadHash);
            }
            Ok((h, Trust::Unsigned))
        }
        Err(e) => Err(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use p256::ecdsa::{signature::hazmat::PrehashSigner, SigningKey};

    fn test_key() -> (SigningKey, Key) {
        let sk = SigningKey::from_slice(&[7u8; 32]).unwrap();
        let point = sk.verifying_key().to_encoded_point(false);
        let mut public = [0u8; 64];
        public.copy_from_slice(&point.as_bytes()[1..]);
        (sk, Key { id: 9, role: 1, public })
    }

    /// An image of `payload`, signed by `sk` as key id 9.
    fn make(payload: &[u8], sk: &SigningKey, image_type: u8, flags: u16) -> Vec<u8> {
        let mut h = vec![0u8; HEADER_SIZE];
        h[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        h[4] = 1;
        h[5..7].copy_from_slice(&(HEADER_SIZE as u16).to_le_bytes());
        h[7..9].copy_from_slice(&CHIP_ESP32S3.to_le_bytes());
        h[9] = image_type;
        h[10] = 9;
        h[11..13].copy_from_slice(&flags.to_le_bytes());
        h[13..17].copy_from_slice(b"test");
        h[45..50].copy_from_slice(b"1.0.0");
        h[69..73].copy_from_slice(&(HEADER_SIZE as u32).to_le_bytes());
        h[73..77].copy_from_slice(&(payload.len() as u32).to_le_bytes());
        h[77..109].copy_from_slice(&Sha256::digest(payload));
        let sig: Signature = sk.sign_prehash(&Sha256::digest(&h[..SIGNED_LEN])).unwrap();
        h[SIGNED_LEN..].copy_from_slice(&sig.to_bytes());
        h.extend_from_slice(payload);
        h
    }

    #[test]
    fn good_image_verifies() {
        let (sk, key) = test_key();
        let img = make(b"hello kernel", &sk, IMG_MODULE, 1);
        let h = verify(&img, CHIP_ESP32S3, &[key]).unwrap();
        assert_eq!(h.name, "test");
        assert_eq!(h.version, "1.0.0");
    }

    #[test]
    fn flipped_payload_byte_is_refused() {
        let (sk, key) = test_key();
        let mut img = make(b"hello kernel", &sk, IMG_MODULE, 1);
        let last = img.len() - 1;
        img[last] ^= 1;
        assert_eq!(verify(&img, CHIP_ESP32S3, &[key]), Err(VerifyError::BadHash));
    }

    #[test]
    fn altered_header_breaks_the_signature() {
        let (sk, key) = test_key();
        let mut img = make(b"hello kernel", &sk, IMG_MODULE, 1);
        img[45] = b'2';                                   // version 2.0.0, hash still fine
        assert_eq!(verify(&img, CHIP_ESP32S3, &[key]), Err(VerifyError::BadSignature));
    }

    #[test]
    fn signature_from_another_key_is_refused() {
        let (_, key) = test_key();
        let other = SigningKey::from_slice(&[8u8; 32]).unwrap();
        let img = make(b"hello kernel", &other, IMG_MODULE, 1);
        assert_eq!(verify(&img, CHIP_ESP32S3, &[key]), Err(VerifyError::BadSignature));
    }

    #[test]
    fn unknown_key_wrong_role_and_wrong_chip() {
        let (sk, key) = test_key();
        let img = make(b"x", &sk, IMG_MODULE, 1);
        assert_eq!(verify(&img, CHIP_ESP32S3, &[]), Err(VerifyError::NoKey(9)));
        assert_eq!(verify(&img, 0, &[key]), Err(VerifyError::BadChip));
        let (sk, mut key) = test_key();
        key.role = 4;                                    // a developer key may not sign a kernel
        let img = make(b"x", &sk, IMG_MODULE, 1);
        assert_eq!(verify(&img, CHIP_ESP32S3, &[key]), Err(VerifyError::RoleMismatch));
    }

    #[test]
    fn unsigned_driver_is_accepted_only_as_unsigned_and_only_if_intact() {
        let (sk, key) = test_key();
        let good = make(b"driver payload", &sk, IMG_MODULE, MOD_DRIVER as u16);
        let dev = Key { role: 4, ..key };                 // a developer key may sign drivers
        let (_, t) = inspect_driver(&good, CHIP_ESP32S3, &[dev]).unwrap();
        assert_eq!(t, Trust::Signed(9));
        // no key known to this client: the same file is "unsigned", not refused
        let (_, t) = inspect_driver(&good, CHIP_ESP32S3, &[]).unwrap();
        assert_eq!(t, Trust::Unsigned);
        // a flipped payload byte is refused either way
        let mut bad = good.clone();
        let last = bad.len() - 1;
        bad[last] ^= 1;
        assert_eq!(inspect_driver(&bad, CHIP_ESP32S3, &[]).unwrap_err(), VerifyError::BadHash);
        // a kernel is not a driver
        let kernel = make(b"k", &sk, IMG_MODULE, 1);
        assert_eq!(inspect_driver(&kernel, CHIP_ESP32S3, &[]).unwrap_err(), VerifyError::BadType);
    }

    #[test]
    fn truncated_and_foreign_files_are_refused() {
        let (sk, key) = test_key();
        let img = make(b"hello kernel", &sk, IMG_MODULE, 1);
        assert_eq!(verify(&img[..img.len() - 3], CHIP_ESP32S3, &[Key { ..key }]), Err(VerifyError::BadLayout));
        assert_eq!(verify(b"PK\x03\x04", CHIP_ESP32S3, &[]), Err(VerifyError::TooShort));
        assert_eq!(verify(&vec![0u8; 400], CHIP_ESP32S3, &[]), Err(VerifyError::BadMagic));
    }
}
