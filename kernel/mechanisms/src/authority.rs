//! Request-scope credentials. Account authentication/session revocation remains a
//! separate admission check until its complete owning slice is migrated.

use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

type HmacSha256 = Hmac<Sha256>;
pub type Policies = BTreeMap<String, Vec<Grant>>;

const INVALID: &str = "请求授权无效、过期或超出当前功能范围";

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Grant {
    pub path: String,
    pub methods: Vec<String>,
    pub descendants: bool,
}

impl Grant {
    pub fn allows(&self, method: &str, path: &str) -> bool {
        if !self.methods.iter().any(|allowed| allowed == method)
            || !path.starts_with('/')
            || !path
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"_./-".contains(&c))
            || path
                .split('/')
                .skip(1)
                .any(|part| matches!(part, "" | "." | ".."))
        {
            return false;
        }
        let expected: Vec<_> = self.path.split('/').collect();
        let actual: Vec<_> = path.split('/').collect();
        actual.len() >= expected.len()
            && (self.descendants || actual.len() == expected.len())
            && expected
                .iter()
                .zip(actual)
                .all(|(a, b)| *a == ":id" || *a == b)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SecretRequest {
    pub secret: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ForwardedRequest {
    pub secret: String,
    pub credential: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SubjectRequest {
    pub session: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct GrantRequest {
    pub grant: Grant,
    pub method: String,
    pub path: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct IssueRequest {
    pub secret: String,
    pub policies: Policies,
    pub scope: String,
    pub session: String,
    pub now: f64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AuthorizeRequest {
    pub secret: String,
    pub policies: Policies,
    pub worker_grants: Vec<Grant>,
    pub credential: String,
    pub method: String,
    pub path: String,
    pub session: String,
    pub now: f64,
}

#[derive(Serialize)]
pub struct Issued {
    pub token: String,
    pub expires: i64,
}

// Field order is the canonical current token encoding (sorted JSON keys).
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Payload {
    expires: i64,
    scope: String,
    subject: String,
}

fn mac(key: &[u8], message: &[u8]) -> Vec<u8> {
    let mut mac = HmacSha256::new_from_slice(key).expect("HMAC accepts any key length");
    mac.update(message);
    mac.finalize().into_bytes().to_vec()
}

// Hash both inputs to equal-width digests and let HMAC's constant-time verifier
// compare them, rather than using early-return string equality for credentials.
fn secret_eq(a: &str, b: &str) -> bool {
    let mut check = HmacSha256::new_from_slice(b"asterion.constant-time.compare")
        .expect("HMAC accepts any key length");
    check.update(a.as_bytes());
    check
        .verify_slice(&mac(b"asterion.constant-time.compare", b.as_bytes()))
        .is_ok()
}

pub fn worker_token(secret: &str) -> String {
    hex::encode(mac(secret.as_bytes(), b"asterion.worker.transport.v1"))
}

/// Credential the Rust entry attaches when forwarding an already authorized
/// request to the internal Python process during migration. It is derived
/// from the runtime secret and is never accepted as a client credential.
pub fn forwarding_token(secret: &str) -> String {
    hex::encode(mac(secret.as_bytes(), b"asterion.entry.forwarding.v1"))
}

/// Constant-time check of the forwarding credential.
pub fn forwarded(credential: &str, secret: &str) -> bool {
    secret_eq(credential, &forwarding_token(secret))
}

pub fn subject(session: &str) -> String {
    hex::encode(Sha256::digest(session.as_bytes()))
}

pub fn issue(request: &IssueRequest) -> Result<Issued, String> {
    if !request.policies.contains_key(&request.scope) {
        return Err("未登记该功能的请求授权".into());
    }
    if !request.now.is_finite() || request.now < 0.0 || request.now >= (i64::MAX - 300) as f64 {
        return Err("Invalid authorization clock".into());
    }
    let expires = request.now.trunc() as i64 + 300;
    let payload = Payload {
        expires,
        scope: request.scope.clone(),
        subject: subject(&request.session),
    };
    let bytes = serde_json::to_vec(&payload).map_err(|error| error.to_string())?;
    let body = URL_SAFE_NO_PAD.encode(bytes);
    let key = mac(request.secret.as_bytes(), b"asterion.request.scopes.v1");
    let signature = hex::encode(mac(&key, body.as_bytes()));
    Ok(Issued {
        token: format!("scope.{body}.{signature}"),
        expires,
    })
}

/// The fixed authorization rules of one runtime: secret, scope policies and
/// the grants of the task worker credential.
pub struct Rules<'a> {
    pub secret: &'a str,
    pub policies: &'a Policies,
    pub worker_grants: &'a [Grant],
}

/// One request's credential, target and account session.
pub struct Check<'a> {
    pub credential: &'a str,
    pub method: &'a str,
    pub path: &'a str,
    pub session: &'a str,
    pub now: f64,
}

pub fn authorize(request: &AuthorizeRequest) -> Result<String, String> {
    check(
        &Rules {
            secret: &request.secret,
            policies: &request.policies,
            worker_grants: &request.worker_grants,
        },
        &Check {
            credential: &request.credential,
            method: &request.method,
            path: &request.path,
            session: &request.session,
            now: request.now,
        },
    )
}

/// Principal of an authorized request: `root`, `worker` or the scope name.
pub fn check(rules: &Rules, request: &Check) -> Result<String, String> {
    if secret_eq(request.credential, rules.secret) {
        return Ok("root".into());
    }
    if secret_eq(request.credential, &worker_token(rules.secret)) {
        return if rules
            .worker_grants
            .iter()
            .any(|g| g.allows(request.method, request.path))
        {
            Ok("worker".into())
        } else {
            Err("任务执行器无权访问此接口".into())
        };
    }
    verify_scope(rules, request).ok_or_else(|| INVALID.into())
}

fn verify_scope(rules: &Rules, request: &Check) -> Option<String> {
    if !request.now.is_finite() || request.now < 0.0 {
        return None;
    }
    let mut parts = request.credential.split('.');
    if parts.next()? != "scope" {
        return None;
    }
    let body = parts.next()?;
    let signature = parts.next()?;
    if parts.next().is_some()
        || signature.len() != 64
        || !signature
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return None;
    }
    let key = mac(rules.secret.as_bytes(), b"asterion.request.scopes.v1");
    let mut verifier = HmacSha256::new_from_slice(&key).ok()?;
    verifier.update(body.as_bytes());
    verifier.verify_slice(&hex::decode(signature).ok()?).ok()?;
    let value: Payload = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(body).ok()?).ok()?;
    if value.expires as f64 <= request.now || !secret_eq(&value.subject, &subject(request.session))
    {
        return None;
    }
    let grants = rules.policies.get(&value.scope)?;
    grants
        .iter()
        .any(|g| g.allows(request.method, request.path))
        .then_some(value.scope)
}
