//! Purpose-scoped authenticated encryption. No domain names or plugin-selected ciphers.
use base64::{Engine as _, engine::general_purpose::URL_SAFE};
use fernet::Fernet;
use hmac::{Hmac, Mac};
use sha2::Sha256;
use zeroize::Zeroizing;

fn mac(key: &[u8], value: &[u8]) -> [u8; 32] {
    let mut digest =
        Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts arbitrary key lengths");
    digest.update(value);
    digest.finalize().into_bytes().into()
}

/// The host creates this handle with an approved purpose; consumers cannot extract keys.
pub struct Secrets {
    cipher: Fernet,
    fingerprint_key: Zeroizing<[u8; 32]>,
}

impl Secrets {
    pub fn new(master: &[u8], encryption_scope: &[u8], fingerprint_scope: &[u8]) -> Self {
        let key = Zeroizing::new(mac(master, encryption_scope));
        let encoded = Zeroizing::new(URL_SAFE.encode(*key));
        Self {
            cipher: Fernet::new(&encoded).expect("derived key is 32 bytes"),
            fingerprint_key: Zeroizing::new(mac(key.as_ref(), fingerprint_scope)),
        }
    }

    pub fn encrypt(&self, content: &[u8]) -> Vec<u8> {
        self.cipher.encrypt(content).into_bytes()
    }

    pub fn decrypt(&self, content: &[u8]) -> Result<Vec<u8>, SecretError> {
        let token = std::str::from_utf8(content).map_err(|_| SecretError)?;
        // Persistent credentials have no expiry. The Fernet timestamp is informational,
        // not an authorization claim. This library checks future skew even without a TTL;
        // use the token's reference time, then let it authenticate the entire message.
        // Clamp only to avoid overflow in the pinned library's reference + 60 calculation.
        let decoded = URL_SAFE.decode(content).map_err(|_| SecretError)?;
        let bytes: [u8; 8] = decoded
            .get(1..9)
            .ok_or(SecretError)?
            .try_into()
            .map_err(|_| SecretError)?;
        let reference = u64::from_be_bytes(bytes).min(u64::MAX - 60);
        self.cipher
            .decrypt_at_time(token, None, reference)
            .map_err(|_| SecretError)
    }

    pub fn fingerprint(&self, content: &[u8]) -> String {
        hex::encode(mac(self.fingerprint_key.as_ref(), content))
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct SecretError;

/// Restricted account/session signing operation. Does not expose the host key.
pub struct Signer {
    key: Zeroizing<Vec<u8>>,
}

impl Signer {
    pub fn new(key: &[u8]) -> Self {
        Self {
            key: Zeroizing::new(key.to_vec()),
        }
    }

    pub fn digest(&self, content: &[u8]) -> String {
        hex::encode(mac(&self.key, content))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scopes_keys_and_authentication_are_enforced() {
        let scope = Secrets::new(b"fixture-key", b"credentials", b"snapshot");
        let token = scope.encrypt(b"credential");
        assert_eq!(scope.decrypt(&token).unwrap(), b"credential");
        assert_ne!(token, scope.encrypt(b"credential"));
        for other in [
            Secrets::new(b"other-key", b"credentials", b"snapshot"),
            Secrets::new(b"fixture-key", b"other-credentials", b"snapshot"),
        ] {
            assert_eq!(other.decrypt(&token), Err(SecretError));
            assert_ne!(
                other.fingerprint(b"credential"),
                scope.fingerprint(b"credential")
            );
        }
        let different_fingerprint = Secrets::new(b"fixture-key", b"credentials", b"other-snapshot");
        assert_eq!(
            different_fingerprint.decrypt(&token).unwrap(),
            b"credential"
        );
        assert_ne!(
            different_fingerprint.fingerprint(b"credential"),
            scope.fingerprint(b"credential")
        );
        let mut changed = token.clone();
        let index = changed.len() / 2;
        changed[index] ^= 1;
        assert_eq!(scope.decrypt(&changed), Err(SecretError));
        for invalid in [&b""[..], &b"invalid"[..], &b"\xff"[..]] {
            assert_eq!(scope.decrypt(invalid), Err(SecretError));
        }
    }

    #[test]
    fn persistent_credentials_do_not_expire_or_depend_on_wall_clock() {
        let scope = Secrets::new(b"fixture-key", b"credentials", b"snapshot");
        for timestamp in [0, 4_000_000_000, u64::MAX] {
            let token = scope.cipher.encrypt_at_time(b"persisted", timestamp);
            assert_eq!(scope.decrypt(token.as_bytes()).unwrap(), b"persisted");
            let mut raw = URL_SAFE.decode(&token).unwrap();
            raw[1] ^= 1; // A timestamp is still authenticated, even though it cannot expire.
            assert_eq!(
                scope.decrypt(URL_SAFE.encode(raw).as_bytes()),
                Err(SecretError)
            );
        }
    }

    #[test]
    fn signing_uses_hmac_sha256() {
        // RFC 4231 test case 1, public non-secret vector.
        assert_eq!(
            Signer::new(&[0x0b; 20]).digest(b"Hi There"),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
    }
}
