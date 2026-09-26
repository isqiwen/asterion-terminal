//! Local accounts, verification codes, sessions and the account-wide terminal
//! lock (L3 application service). Credential hashing, session tokens and code
//! digests are the fixed kernel mechanisms keyed by the runtime secret, so
//! existing accounts, sessions and PINs remain valid.
use crate::store::{Kind, StoreError, Table, Transaction};
use asterion_kernel::{secrets::Signer, security};
use regex::Regex;
use serde::Serialize;
use serde_json::{Value, json};
use std::sync::{Arc, LazyLock};
use std::time::{SystemTime, UNIX_EPOCH};

pub const TABLES: [Table; 4] = [
    Table {
        name: "identity_accounts",
        columns: &[
            ("id", Kind::Text),
            ("email", Kind::Text),
            ("first_name", Kind::Text),
            ("last_name", Kind::Text),
            ("country_code", Kind::Text),
            ("phone", Kind::Text),
            ("password", Kind::Text),
            ("verified", Kind::Boolean),
            ("failures", Kind::Integer),
            ("blocked_until", Kind::Float),
        ],
        primary: &["id"],
        unique: &[&["email"]],
    },
    Table {
        name: "identity_challenges",
        columns: &[
            ("account_id", Kind::Text),
            ("purpose", Kind::Text),
            ("digest", Kind::Text),
            ("expires", Kind::Float),
            ("sent_at", Kind::Float),
            ("attempts", Kind::Integer),
        ],
        primary: &["account_id", "purpose"],
        unique: &[],
    },
    Table {
        name: "identity_sessions",
        columns: &[
            ("digest", Kind::Text),
            ("account_id", Kind::Text),
            ("expires", Kind::Float),
        ],
        primary: &["digest"],
        unique: &[],
    },
    Table {
        name: "identity_pin_security",
        columns: &[
            ("account_id", Kind::Text),
            ("pin_hash", Kind::Text),
            ("timeout", Kind::Integer),
            ("last_activity", Kind::Float),
            ("locked", Kind::Boolean),
            ("failures", Kind::Integer),
            ("blocked_until", Kind::Float),
            ("revision", Kind::Integer),
        ],
        primary: &["account_id"],
        unique: &[],
    },
];

const SESSION_SECONDS: f64 = 43_200.0;
const CODE_SECONDS: f64 = 600.0;
const TIMEOUTS: [i64; 5] = [60, 300, 600, 900, 1800];

/// A user-facing refusal with its HTTP status and stable code.
#[derive(Debug, PartialEq)]
pub struct IdentityError {
    pub status: u16,
    pub code: &'static str,
    pub message: String,
}
fn refuse(status: u16, code: &'static str, message: &str) -> IdentityError {
    IdentityError {
        status,
        code,
        message: message.into(),
    }
}
impl From<StoreError> for IdentityError {
    fn from(error: StoreError) -> Self {
        eprintln!("asterion-server: identity storage failure: {error}");
        refuse(500, "INTERNAL_ERROR", "账户服务暂不可用，请稍后重试")
    }
}
type Outcome<T> = Result<T, IdentityError>;

fn expired() -> IdentityError {
    refuse(401, "SESSION_EXPIRED", "登录已过期，请重新登录")
}
fn unsupported_security() -> IdentityError {
    refuse(
        409,
        "UNSUPPORTED_ACCOUNT_SECURITY",
        "账号安全状态不受支持，缺少注册时设置的 PIN",
    )
}

pub fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("clock after epoch")
        .as_secs_f64()
}

static EMAIL: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^[a-z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z]{2,63}$")
        .expect("email pattern")
});

pub fn email_address(value: &str) -> Outcome<String> {
    let value = value.trim().to_lowercase();
    if value.chars().count() > 254 || !EMAIL.is_match(&value) {
        return Err(refuse(400, "ACCOUNT_ERROR", "请输入有效的邮箱地址"));
    }
    Ok(value)
}

fn password_hash(password: &str) -> Outcome<String> {
    if !(12..=128).contains(&password.chars().count()) {
        return Err(refuse(400, "ACCOUNT_ERROR", "密码长度必须为 12–128 个字符"));
    }
    security::hash(password.as_bytes())
        .map_err(|_| refuse(500, "INTERNAL_ERROR", "账户服务暂不可用，请稍后重试"))
}

fn password_matches(password: &str, encoded: &str) -> Outcome<bool> {
    if !(12..=128).contains(&password.chars().count()) {
        return Ok(false);
    }
    security::verify(password.as_bytes(), encoded).map_err(|_| {
        refuse(
            409,
            "UNSUPPORTED_ACCOUNT_CREDENTIAL",
            "账号凭据格式不受支持，无法验证",
        )
    })
}

fn pin_valid(pin: &str) -> bool {
    pin.len() == 6 && pin.bytes().all(|c| c.is_ascii_digit())
}

fn encode_pin(pin: &str) -> Outcome<String> {
    if !pin_valid(pin) {
        return Err(refuse(422, "INVALID_PIN", "PIN 必须为 6 位数字"));
    }
    password_hash(&format!("asterion-pin:{pin}"))
}

fn random_code() -> Outcome<String> {
    // Rejection sampling keeps all 10^6 codes equally likely.
    loop {
        let mut bytes = [0_u8; 4];
        getrandom::fill(&mut bytes)
            .map_err(|_| refuse(500, "INTERNAL_ERROR", "账户服务暂不可用，请稍后重试"))?;
        let value = u32::from_le_bytes(bytes);
        if value < 4_294_000_000 {
            return Ok(format!("{:06}", value % 1_000_000));
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verification {
    /// Codes are accepted locally without claiming ownership of the address.
    Local,
    /// Codes are delivered by the configured SMTP server and must match.
    Email,
}
impl Verification {
    pub fn parse(value: &str) -> Result<Self, String> {
        match value {
            "local" => Ok(Self::Local),
            "email" => Ok(Self::Email),
            _ => Err("Unknown account verification policy".into()),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Local => "local",
            Self::Email => "email",
        }
    }
    fn retry_after(self) -> f64 {
        match self {
            Self::Local => 0.0,
            Self::Email => 60.0,
        }
    }
}

fn text(value: &Value) -> String {
    value.as_str().unwrap_or_default().to_string()
}
fn number(value: &Value) -> f64 {
    value.as_f64().unwrap_or_default()
}
fn integer(value: &Value) -> i64 {
    value
        .as_i64()
        .or_else(|| value.as_f64().map(|v| v as i64))
        .unwrap_or_default()
}
fn boolean(value: &Value) -> bool {
    value
        .as_bool()
        .unwrap_or_else(|| value.as_i64().is_some_and(|v| v != 0))
}

const ACCOUNT: &str = "a.id, a.email, a.first_name, a.last_name, a.password, a.verified, \
                       a.failures, a.blocked_until";

struct Account {
    id: String,
    email: String,
    first_name: String,
    last_name: String,
    password: String,
    verified: bool,
    failures: i64,
    blocked_until: f64,
}
impl Account {
    fn read(row: &[Value]) -> Self {
        Self {
            id: text(&row[0]),
            email: text(&row[1]),
            first_name: text(&row[2]),
            last_name: text(&row[3]),
            password: text(&row[4]),
            verified: boolean(&row[5]),
            failures: integer(&row[6]),
            blocked_until: number(&row[7]),
        }
    }
    fn public(&self) -> Value {
        json!({"email": self.email, "first_name": self.first_name, "last_name": self.last_name})
    }
}

pub struct Registration {
    pub email: String,
    pub password: String,
    pub first_name: String,
    pub last_name: String,
    pub country_code: String,
    pub phone: String,
    pub pin: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action<'a> {
    Status,
    Activity,
    Lock,
    Unlock { pin: &'a str, expected: Option<i64> },
    Change { pin: &'a str, password: &'a str },
    Timeout(i64),
}

#[derive(Debug, Serialize, PartialEq)]
pub struct SecurityState {
    pub locked: bool,
    pub timeout_seconds: i64,
    pub remaining_seconds: f64,
    pub revision: i64,
    pub retry_after: i64,
}

/// How the entry reports an account session to the internal Python process.
#[derive(Debug, PartialEq)]
pub enum Session {
    Unlocked(String),
    Locked(String),
    Expired,
    Unsupported,
    /// The account store could not be read; nothing is decided.
    Unavailable,
}

/// Delivery of verification codes in email mode (SMTP in production).
pub trait Delivery: Send + Sync {
    fn send(&self, email: &str, code: &str, purpose: &str) -> Result<(), IdentityError>;
}

pub struct Identity {
    store: Arc<crate::store::Store>,
    signer: Signer,
    sessions: security::Sessions,
    verification: Verification,
    delivery: Box<dyn Delivery>,
    dummy_password: String,
}

impl Identity {
    pub fn new(
        store: Arc<crate::store::Store>,
        secret: &str,
        verification: Verification,
        delivery: Box<dyn Delivery>,
    ) -> Result<Self, String> {
        store.ensure(&TABLES).map_err(|e| e.to_string())?;
        let mut random = [0_u8; 18];
        getrandom::fill(&mut random).map_err(|e| e.to_string())?;
        Ok(Self {
            store,
            signer: Signer::new(secret.as_bytes()),
            sessions: security::Sessions::new(secret.as_bytes()),
            verification,
            delivery,
            dummy_password: security::hash(hex::encode(random).as_bytes())
                .map_err(|e| e.to_string())?,
        })
    }

    pub fn verification(&self) -> Verification {
        self.verification
    }

    fn user(&self, tx: &Transaction, email: &str) -> Outcome<Option<Account>> {
        let rows = tx.rows(
            &format!(
                "SELECT {ACCOUNT} FROM identity_accounts a WHERE a.email = ?{}",
                tx.locked()
            ),
            vec![json!(email)],
        )?;
        Ok(rows.first().map(|row| Account::read(row)))
    }

    fn issue(&self, tx: &Transaction, account: &Account, purpose: &str) -> Outcome<()> {
        let now = now();
        let key = vec![json!(account.id), json!(purpose)];
        let old = tx.rows(
            "SELECT sent_at FROM identity_challenges WHERE account_id = ? AND purpose = ?",
            key.clone(),
        )?;
        if old
            .first()
            .is_some_and(|row| now - number(&row[0]) < self.verification.retry_after())
        {
            return Err(refuse(429, "RATE_LIMITED", "请等待 60 秒后再申请验证码"));
        }
        let code = random_code()?;
        if self.verification == Verification::Email {
            self.delivery.send(&account.email, &code, purpose)?;
        }
        tx.execute(
            "DELETE FROM identity_challenges WHERE account_id = ? AND purpose = ?",
            key,
        )?;
        tx.execute(
            "INSERT INTO identity_challenges (account_id, purpose, digest, expires, sent_at, \
             attempts) VALUES (?, ?, ?, ?, ?, ?)",
            vec![
                json!(account.id),
                json!(purpose),
                json!(
                    self.signer
                        .digest(format!("{}{purpose}{code}", account.id).as_bytes())
                ),
                json!(now + CODE_SECONDS),
                json!(now),
                json!(0),
            ],
        )?;
        Ok(())
    }

    pub fn register(&self, body: &Registration) -> Outcome<Value> {
        let email = email_address(&body.email)?;
        let encoded = password_hash(&body.password)?;
        let pin = encode_pin(&body.pin)?;
        let result = self.store.transaction(true, |tx| -> Outcome<()> {
            if self.user(tx, &email)?.is_some() {
                return Err(refuse(
                    409,
                    "ACCOUNT_EXISTS",
                    "该邮箱已注册，请登录或重新发送验证码",
                ));
            }
            let account = Account {
                id: uuid::Uuid::new_v4().to_string(),
                email: email.clone(),
                first_name: body.first_name.clone(),
                last_name: body.last_name.clone(),
                password: encoded.clone(),
                verified: false,
                failures: 0,
                blocked_until: 0.0,
            };
            tx.execute(
                "INSERT INTO identity_accounts (id, email, first_name, last_name, country_code, \
                 phone, password, verified, failures, blocked_until) \
                 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                vec![
                    json!(account.id),
                    json!(account.email),
                    json!(account.first_name),
                    json!(account.last_name),
                    json!(body.country_code),
                    json!(body.phone),
                    json!(account.password),
                    json!(false),
                    json!(0),
                    json!(0.0),
                ],
            )?;
            tx.execute(
                "INSERT INTO identity_pin_security (account_id, pin_hash, timeout, \
                 last_activity, locked, failures, blocked_until, revision) \
                 VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                vec![
                    json!(account.id),
                    json!(pin),
                    json!(300),
                    json!(now()),
                    json!(false),
                    json!(0),
                    json!(0.0),
                    json!(0),
                ],
            )?;
            self.issue(tx, &account, "verify")
        });
        match result {
            Err(IdentityError {
                code: "INTERNAL_ERROR",
                ..
            }) if self.exists(&email) => Err(refuse(409, "ACCOUNT_EXISTS", "该邮箱已注册，请登录")),
            Err(error) => Err(error),
            Ok(()) => Ok(json!({
                "status": if self.verification == Verification::Local {
                    "local_verification_ready"
                } else {
                    "verification_sent"
                },
                "email": email,
                "retry_after": self.verification.retry_after() as i64,
            })),
        }
    }

    /// Distinguishes a concurrent registration of the same address from other
    /// storage failures after a failed insert.
    fn exists(&self, email: &str) -> bool {
        self.store
            .transaction(false, |tx| -> Outcome<bool> {
                Ok(self.user(tx, email)?.is_some())
            })
            .unwrap_or(false)
    }

    pub fn resend(&self, email: &str, purpose: &str) -> Outcome<Value> {
        let email = email_address(email)?;
        self.store.transaction(true, |tx| -> Outcome<()> {
            if let Some(account) = self.user(tx, &email)?
                && (purpose == "reset" || !account.verified)
            {
                self.issue(tx, &account, purpose)?;
            }
            Ok(())
        })?;
        Ok(json!({
            "status": if self.verification == Verification::Local {
                "local_verification_ready"
            } else {
                "if_eligible_sent"
            },
            "retry_after": self.verification.retry_after() as i64,
        }))
    }

    fn consume(
        &self,
        tx: &Transaction,
        account: &Account,
        code: &str,
        purpose: &str,
    ) -> Outcome<bool> {
        let key = vec![json!(account.id), json!(purpose)];
        let rows = tx.rows(
            &format!(
                "SELECT digest, expires, attempts FROM identity_challenges \
                 WHERE account_id = ? AND purpose = ?{}",
                tx.locked()
            ),
            key.clone(),
        )?;
        let Some(challenge) = rows.first() else {
            return Ok(false);
        };
        let attempts = integer(&challenge[2]);
        if number(&challenge[1]) < now() || attempts >= 5 {
            return Ok(false);
        }
        tx.execute(
            "UPDATE identity_challenges SET attempts = ? WHERE account_id = ? AND purpose = ?",
            vec![json!(attempts + 1), json!(account.id), json!(purpose)],
        )?;
        let accepted = match self.verification {
            Verification::Local => code.len() == 6 && code.bytes().all(|c| c.is_ascii_digit()),
            Verification::Email => security::digest_matches(
                &self
                    .signer
                    .digest(format!("{}{purpose}{code}", account.id).as_bytes()),
                &text(&challenge[0]),
            ),
        };
        if !accepted {
            return Ok(false);
        }
        tx.execute(
            "DELETE FROM identity_challenges WHERE account_id = ? AND purpose = ?",
            key,
        )?;
        Ok(true)
    }

    /// Verify an address, or reset the password when `password` is given.
    /// A wrong code still records the attempt before the refusal is returned.
    pub fn verify(&self, email: &str, code: &str, password: Option<&str>) -> Outcome<Value> {
        let email = email_address(email)?;
        let encoded = password.map(password_hash).transpose()?;
        let purpose = if encoded.is_some() { "reset" } else { "verify" };
        let valid = self.store.transaction(true, |tx| -> Outcome<bool> {
            let Some(account) = self.user(tx, &email)? else {
                return Ok(false);
            };
            if !self.consume(tx, &account, code, purpose)? {
                return Ok(false);
            }
            match &encoded {
                Some(encoded) => {
                    tx.execute(
                        "DELETE FROM identity_sessions WHERE account_id = ?",
                        vec![json!(account.id)],
                    )?;
                    tx.execute(
                        "UPDATE identity_accounts SET verified = ?, password = ?, failures = ?, \
                         blocked_until = ? WHERE id = ?",
                        vec![
                            json!(true),
                            json!(encoded),
                            json!(0),
                            json!(0.0),
                            json!(account.id),
                        ],
                    )?;
                }
                None => {
                    tx.execute(
                        "UPDATE identity_accounts SET verified = ? WHERE id = ?",
                        vec![json!(true), json!(account.id)],
                    )?;
                }
            }
            Ok(true)
        })?;
        if !valid {
            return Err(refuse(
                400,
                "INVALID_CODE",
                "验证码无效、已过期或尝试次数过多",
            ));
        }
        Ok(json!({"status": if password.is_some() { "password_reset" } else { "verified" }}))
    }

    /// Failed attempts are recorded before the refusal is returned.
    pub fn login(&self, email: &str, password: &str) -> Outcome<Value> {
        let email = email_address(email)?;
        let now = now();
        let (outcome, account) = self.store.transaction(true, |tx| {
            let account = self.user(tx, &email)?;
            let outcome: Outcome<String> = match &account {
                Some(account) if account.blocked_until > now => Err(refuse(
                    429,
                    "RATE_LIMITED",
                    "尝试次数过多，请 15 分钟后重试或找回密码",
                )),
                _ if !password_matches(
                    password,
                    account
                        .as_ref()
                        .map_or(&self.dummy_password, |a| &a.password),
                )? =>
                {
                    if let Some(account) = &account {
                        let failures = account.failures + 1;
                        tx.execute(
                            "UPDATE identity_accounts SET failures = ?, blocked_until = ? \
                             WHERE id = ?",
                            vec![
                                json!(if failures < 5 { failures } else { 0 }),
                                json!(if failures >= 5 { now + 900.0 } else { 0.0 }),
                                json!(account.id),
                            ],
                        )?;
                    }
                    Err(refuse(401, "INVALID_CREDENTIALS", "邮箱或密码不正确"))
                }
                Some(account) if !account.verified => {
                    Err(refuse(403, "EMAIL_UNVERIFIED", "请先验证邮箱"))
                }
                Some(account) => {
                    tx.execute(
                        "UPDATE identity_accounts SET failures = ?, blocked_until = ? WHERE id = ?",
                        vec![json!(0), json!(0.0), json!(account.id)],
                    )?;
                    self.pin_login(tx, &account.id, now)?;
                    let session = self.sessions.issue(now, SESSION_SECONDS).map_err(|_| {
                        refuse(500, "INTERNAL_ERROR", "账户服务暂不可用，请稍后重试")
                    })?;
                    tx.execute(
                        "DELETE FROM identity_sessions WHERE expires <= ?",
                        vec![json!(now)],
                    )?;
                    tx.execute(
                        "INSERT INTO identity_sessions (digest, account_id, expires) \
                         VALUES (?, ?, ?)",
                        vec![
                            json!(session.digest),
                            json!(account.id),
                            json!(session.expires),
                        ],
                    )?;
                    Ok(session.token.to_string())
                }
                None => Err(refuse(401, "INVALID_CREDENTIALS", "邮箱或密码不正确")),
            };
            Ok::<_, IdentityError>((outcome, account))
        })?;
        let token = outcome?;
        let account = account.expect("a session belongs to an account");
        Ok(json!({"session": token, "user": account.public()}))
    }

    fn pin_login(&self, tx: &Transaction, account_id: &str, now: f64) -> Outcome<()> {
        let rows = tx.rows(
            &format!(
                "SELECT pin_hash, revision FROM identity_pin_security WHERE account_id = ?{}",
                tx.locked()
            ),
            vec![json!(account_id)],
        )?;
        let Some(row) = rows.first().filter(|row| !text(&row[0]).is_empty()) else {
            return Err(unsupported_security());
        };
        tx.execute(
            "UPDATE identity_pin_security SET locked = ?, last_activity = ?, revision = ? \
             WHERE account_id = ?",
            vec![
                json!(false),
                json!(now),
                json!(integer(&row[1]) + 1),
                json!(account_id),
            ],
        )?;
        Ok(())
    }

    fn session_user(&self, tx: &Transaction, token: &str, lock: bool) -> Outcome<Account> {
        let digest = self.sessions.lookup(token).map_err(|_| expired())?;
        let rows = tx.rows(
            &format!(
                "SELECT {ACCOUNT}, s.digest, s.expires FROM identity_accounts a \
                 JOIN identity_sessions s ON s.account_id = a.id WHERE s.digest = ?{}",
                if lock { tx.locked() } else { "" }
            ),
            vec![json!(digest)],
        )?;
        let row = rows.first().ok_or_else(expired)?;
        if !self
            .sessions
            .validate(token, &text(&row[8]), number(&row[9]), now())
        {
            return Err(expired());
        }
        Ok(Account::read(row))
    }

    pub fn me(&self, token: &str) -> Outcome<Value> {
        self.store.transaction(false, |tx| {
            Ok(self.session_user(tx, token, false)?.public())
        })
    }

    pub fn logout(&self, token: &str) -> Outcome<Value> {
        if let Ok(digest) = self.sessions.lookup(token) {
            self.store.transaction(true, |tx| -> Outcome<()> {
                tx.execute(
                    "DELETE FROM identity_sessions WHERE digest = ?",
                    vec![json!(digest)],
                )?;
                Ok(())
            })?;
        }
        Ok(json!({"status": "signed_out"}))
    }

    /// Apply `action` to the account-wide lock. Counters and automatic locking
    /// are committed even when the action itself is refused.
    pub fn security(&self, token: &str, action: Action) -> Outcome<SecurityState> {
        let now = now();
        let (state, error) = self.store.transaction(true, |tx| {
            let account = self.session_user(tx, token, true)?;
            let rows = tx.rows(
                &format!(
                    "SELECT pin_hash, timeout, last_activity, locked, failures, blocked_until, \
                     revision FROM identity_pin_security WHERE account_id = ?{}",
                    tx.locked()
                ),
                vec![json!(account.id)],
            )?;
            let Some(row) = rows.first().filter(|row| !text(&row[0]).is_empty()) else {
                return Err(unsupported_security());
            };
            let mut pin_hash = text(&row[0]);
            let mut timeout = integer(&row[1]);
            let mut last_activity = number(&row[2]);
            let mut locked = boolean(&row[3]);
            let mut failures = integer(&row[4]);
            let mut blocked_until = number(&row[5]);
            let mut revision = integer(&row[6]);
            let mut error = None;
            if blocked_until != 0.0 && blocked_until <= now {
                failures = 0;
                blocked_until = 0.0;
            }
            if now - last_activity >= timeout as f64 && !locked {
                locked = true;
                revision += 1;
            }
            match action {
                Action::Status => {}
                Action::Lock => {
                    if !locked {
                        locked = true;
                        revision += 1;
                    }
                }
                Action::Activity => {
                    if !locked {
                        last_activity = now;
                    }
                }
                Action::Unlock { .. } | Action::Change { .. } => {
                    if blocked_until > now {
                        error = Some(refuse(
                            429,
                            "PIN_RATE_LIMITED",
                            "PIN 尝试次数过多，请稍后重试",
                        ));
                    } else if let Action::Unlock { expected, .. } = action
                        && expected != Some(revision)
                    {
                        error = Some(refuse(
                            409,
                            "LOCK_CHANGED",
                            "锁屏状态已变化，请重新输入 PIN",
                        ));
                    } else {
                        let valid = match action {
                            Action::Change { password, .. } => {
                                password_matches(password, &account.password)?
                            }
                            Action::Unlock { pin, .. } => {
                                pin_valid(pin)
                                    && password_matches(&format!("asterion-pin:{pin}"), &pin_hash)?
                            }
                            _ => false,
                        };
                        if valid {
                            if let Action::Change { pin, .. } = action {
                                pin_hash = encode_pin(pin)?;
                            }
                            locked = false;
                            last_activity = now;
                            failures = 0;
                            blocked_until = 0.0;
                            revision += 1;
                        } else {
                            failures += 1;
                            blocked_until = if failures >= 5 { now + 300.0 } else { 0.0 };
                            error = Some(refuse(401, "INVALID_PIN", "PIN 或账号密码不正确"));
                        }
                    }
                }
                Action::Timeout(seconds) => {
                    if locked {
                        error = Some(refuse(423, "TERMINAL_LOCKED", "请先解锁终端"));
                    } else if !TIMEOUTS.contains(&seconds) {
                        error = Some(refuse(422, "INVALID_TIMEOUT", "无效的自动锁定时间"));
                    } else {
                        timeout = seconds;
                        last_activity = now;
                    }
                }
            }
            tx.execute(
                "UPDATE identity_pin_security SET pin_hash = ?, timeout = ?, last_activity = ?, \
                 locked = ?, failures = ?, blocked_until = ?, revision = ? WHERE account_id = ?",
                vec![
                    json!(pin_hash),
                    json!(timeout),
                    json!(last_activity),
                    json!(locked),
                    json!(failures),
                    json!(blocked_until),
                    json!(revision),
                    json!(account.id),
                ],
            )?;
            let state = SecurityState {
                locked,
                timeout_seconds: timeout,
                remaining_seconds: (timeout as f64 - (now - last_activity)).max(0.0),
                revision,
                retry_after: (blocked_until - now).max(0.0) as i64,
            };
            Ok::<_, IdentityError>((state, error))
        })?;
        match error {
            Some(error) => Err(error),
            None => Ok(state),
        }
    }

    /// The account behind a session for requests forwarded to Python: the
    /// lock is evaluated (and automatic locking committed) as for a status read.
    pub fn session(&self, token: &str) -> Session {
        if token.is_empty() {
            return Session::Expired;
        }
        let refused = |error: IdentityError| match error.code {
            "SESSION_EXPIRED" => Session::Expired,
            "UNSUPPORTED_ACCOUNT_SECURITY" => Session::Unsupported,
            _ => Session::Unavailable,
        };
        let email = match self.me(token) {
            Ok(user) => text(&user["email"]),
            Err(error) => return refused(error),
        };
        match self.security(token, Action::Status) {
            Ok(state) if state.locked => Session::Locked(email),
            Ok(_) => Session::Unlocked(email),
            Err(error) => refused(error),
        }
    }
}
