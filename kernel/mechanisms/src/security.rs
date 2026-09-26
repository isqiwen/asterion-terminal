//! Fixed credential mechanisms. Account policy and persistent record ownership live above L1.
use std::sync::Mutex;

use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use scrypt::{Params, scrypt};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use crate::secrets::Signer;

// Fixed work factors and one active derivation bound native KDF memory per process.
// Callers cannot select cheaper or arbitrarily expensive parameters.
static KDF: Mutex<()> = Mutex::new(());
const SECRET_LIMIT: usize = 512;

#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    Input,
    Record,
    Unavailable,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Input => "Invalid credential input",
            Self::Record => "Unsupported credential record",
            Self::Unavailable => "Credential mechanism unavailable",
        })
    }
}

impl std::error::Error for Error {}

fn secret(value: &[u8]) -> Result<(), Error> {
    if value.is_empty() || value.len() > SECRET_LIMIT {
        return Err(Error::Input);
    }
    Ok(())
}

fn decode_hex<const N: usize>(value: &str) -> Result<[u8; N], Error> {
    if value.len() != N * 2
        || !value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err(Error::Record);
    }
    let mut bytes = [0; N];
    hex::decode_to_slice(value, &mut bytes).map_err(|_| Error::Record)?;
    Ok(bytes)
}

fn derive(value: &[u8], salt: &[u8; 16]) -> Result<Zeroizing<[u8; 64]>, Error> {
    secret(value)?;
    let _permit = KDF.lock().map_err(|_| Error::Unavailable)?;
    let params = Params::new(17, 8, 1, 64).expect("fixed scrypt parameters");
    let mut output = Zeroizing::new([0; 64]);
    scrypt(value, salt, &params, output.as_mut()).map_err(|_| Error::Unavailable)?;
    Ok(output)
}

/// The single current record format: 16-byte lowercase salt hex, ':', 64-byte hash hex.
pub fn hash(value: &[u8]) -> Result<String, Error> {
    secret(value)?;
    let mut salt = [0; 16];
    getrandom::fill(&mut salt).map_err(|_| Error::Unavailable)?;
    let digest = derive(value, &salt)?;
    Ok(format!(
        "{}:{}",
        hex::encode(salt),
        hex::encode(digest.as_ref())
    ))
}

/// Invalid persisted records fail explicitly; verification never rewrites a record.
pub fn verify(value: &[u8], record: &str) -> Result<bool, Error> {
    if record.len() != 161 || record.as_bytes().get(32) != Some(&b':') {
        return Err(Error::Record);
    }
    let salt = decode_hex::<16>(&record[..32])?;
    let expected = Zeroizing::new(decode_hex::<64>(&record[33..])?);
    let actual = derive(value, &salt)?;
    Ok(bool::from(actual.as_slice().ct_eq(expected.as_slice())))
}

/// Fixed-size SHA-256/HMAC digests are compared in constant time after shape validation.
pub fn digest_matches(actual: &str, expected: &str) -> bool {
    let (Ok(actual), Ok(expected)) = (decode_hex::<32>(actual), decode_hex::<32>(expected)) else {
        return false;
    };
    bool::from(actual.ct_eq(&expected))
}

pub struct IssuedSession {
    pub token: String,
    pub digest: String,
    pub expires: f64,
}

/// An opaque host-created key holder; storage and revocation use the returned digest.
pub struct Sessions {
    signer: Signer,
}

impl Sessions {
    pub fn new(key: &[u8]) -> Self {
        Self {
            signer: Signer::new(key),
        }
    }

    pub fn issue(&self, now: f64, lifetime: f64) -> Result<IssuedSession, Error> {
        let expires = now + lifetime;
        if !now.is_finite()
            || now < 0.0
            || !lifetime.is_finite()
            || lifetime <= 0.0
            || !expires.is_finite()
            || expires <= now
        {
            return Err(Error::Input);
        }
        let mut random = Zeroizing::new([0; 32]);
        getrandom::fill(random.as_mut()).map_err(|_| Error::Unavailable)?;
        let token = URL_SAFE_NO_PAD.encode(random.as_ref());
        let digest = self.signer.digest(token.as_bytes());
        Ok(IssuedSession {
            token,
            digest,
            expires,
        })
    }

    /// Reject noncanonical or oversized bearer input before any storage lookup.
    pub fn lookup(&self, token: &str) -> Result<String, Error> {
        if token.len() != 43 {
            return Err(Error::Input);
        }
        let decoded = URL_SAFE_NO_PAD.decode(token).map_err(|_| Error::Input)?;
        if decoded.len() != 32 || URL_SAFE_NO_PAD.encode(&decoded) != token {
            return Err(Error::Input);
        }
        Ok(self.signer.digest(token.as_bytes()))
    }

    /// A missing/revoked row must never be supplied by the caller as a fabricated record.
    pub fn validate(&self, token: &str, digest: &str, expires: f64, now: f64) -> bool {
        if !now.is_finite() || now < 0.0 || !expires.is_finite() || expires <= now {
            return false;
        }
        self.lookup(token)
            .is_ok_and(|actual| digest_matches(&actual, digest))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fixed_kdf_matches_current_ascii_and_unicode_vectors() {
        // Independently generated with the current scrypt parameters, not an obsolete format.
        let salt = "000102030405060708090a0b0c0d0e0f";
        for (value, expected) in [
            (
                "correct-password-123",
                "a66b5854a3aa611b6d4ee5055745d008e675efc4cf6f3c4c6d4695b52b03188fb80e8226bedf9bc719f2917bdbfdc1181ae344c9edaa7adb8d4ec6baffcaa866",
            ),
            (
                "密码🔒密码🔒密码🔒密码🔒",
                "99ae239edd5a960f6153bf14a504bc1e6ade370ddcd41c3d9323618344e44febb032c1561f4ecc88c58f9e98bd758542139c3589e4411225b93a871c60db3c2a",
            ),
        ] {
            assert!(verify(value.as_bytes(), &format!("{salt}:{expected}")).unwrap());
        }
    }

    #[test]
    fn salts_are_random_and_wrong_secrets_fail() {
        let first = hash(b"fixture-password").unwrap();
        let second = hash(b"fixture-password").unwrap();
        assert_ne!(first, second);
        assert!(!verify(b"incorrect-password", &first).unwrap());
    }

    #[test]
    fn malformed_records_and_unbounded_inputs_are_rejected_before_derivation() {
        let good = format!("{}:{}", "a".repeat(32), "b".repeat(128));
        for record in [
            String::new(),
            good.replace(':', "/"),
            good.to_uppercase(),
            good[..160].into(),
            format!("{}:{}", "界".repeat(10) + "xx", "b".repeat(128)),
        ] {
            assert_eq!(verify(b"fixture", &record), Err(Error::Record));
        }
        assert_eq!(hash(&[]), Err(Error::Input));
        assert_eq!(hash(&[b'a'; 513]), Err(Error::Input));
        assert_eq!(verify(&[b'a'; 513], &good), Err(Error::Input));
    }

    #[test]
    fn comparisons_reject_every_changed_digest_byte() {
        let expected = "a".repeat(64);
        assert!(digest_matches(&expected, &expected));
        for index in 0..64 {
            let mut actual = expected.clone();
            actual.replace_range(index..index + 1, "b");
            assert!(!digest_matches(&actual, &expected));
        }
        for malformed in ["", "not-a-digest", &"A".repeat(64), &"a".repeat(65)] {
            assert!(!digest_matches(malformed, &expected));
        }
    }

    #[test]
    fn session_issuance_is_random_and_validation_is_bound_to_key_token_and_time() {
        let sessions = Sessions::new(b"public-fixture-key");
        let issued = sessions.issue(100.0, 43200.0).unwrap();
        assert_eq!(issued.expires, 43300.0);
        assert_ne!(issued.token, sessions.issue(100.0, 43200.0).unwrap().token);
        assert_eq!(sessions.lookup(&issued.token).unwrap(), issued.digest);
        assert!(sessions.validate(&issued.token, &issued.digest, issued.expires, 43299.9));
        assert!(!sessions.validate(&issued.token, &issued.digest, issued.expires, 43300.0));
        assert!(!Sessions::new(b"wrong-key").validate(
            &issued.token,
            &issued.digest,
            issued.expires,
            100.0
        ));
        for value in [f64::NAN, f64::INFINITY, -1.0] {
            assert!(sessions.issue(value, 1.0).is_err());
            assert!(!sessions.validate(&issued.token, &issued.digest, issued.expires, value));
            assert!(!sessions.validate(&issued.token, &issued.digest, value, 100.0));
        }
        for lifetime in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            assert!(sessions.issue(100.0, lifetime).is_err());
        }
        for invalid in [
            String::new(),
            "a".repeat(42),
            "a".repeat(44),
            "界".repeat(43),
            format!("{}=", issued.token),
        ] {
            assert_eq!(sessions.lookup(&invalid), Err(Error::Input));
            assert!(!sessions.validate(&invalid, &issued.digest, issued.expires, 100.0));
        }
    }
}
