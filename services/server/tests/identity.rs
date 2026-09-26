use asterion_server::identity::{Action, Registration, Session};
use asterion_server::{
    AUTHORIZATION, Authorization, Delivery, Identity, IdentityError, Mailer, RouteTable, Settings,
    Store, Verification, application,
};
use axum::body::{Body, Bytes};
use axum::http::{Method, StatusCode};
use base64::{Engine, engine::general_purpose::STANDARD};
use http_body_util::BodyExt;
use hyper_util::{client::legacy::Client, rt::TokioExecutor};
use serde_json::{Value, json};
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};

const SECRET: &str = "identity-test-secret-at-least-24-characters";
const PASSWORD: &str = "correct-password-123";

#[derive(Default)]
struct Capture {
    sent: Mutex<Vec<(String, String, String)>>,
    fail: AtomicBool,
}
struct Shared(Arc<Capture>);
impl Delivery for Shared {
    fn send(&self, email: &str, code: &str, purpose: &str) -> Result<(), IdentityError> {
        if self.0.fail.load(Ordering::SeqCst) {
            return Err(IdentityError {
                status: 502,
                code: "MAIL_FAILED",
                message: "邮件发送失败".into(),
            });
        }
        self.0
            .sent
            .lock()
            .unwrap()
            .push((email.into(), code.into(), purpose.into()));
        Ok(())
    }
}

struct Harness {
    identity: Identity,
    store: Arc<Store>,
    mail: Arc<Capture>,
}
impl Harness {
    fn new(verification: Verification) -> Self {
        let root = tempfile::tempdir().unwrap().keep();
        let store = Arc::new(
            Store::open(&format!("sqlite:///{}", root.join("identity.db").display())).unwrap(),
        );
        let mail = Arc::new(Capture::default());
        let identity = Identity::new(
            store.clone(),
            SECRET,
            verification,
            Box::new(Shared(mail.clone())),
        )
        .unwrap();
        Self {
            identity,
            store,
            mail,
        }
    }
    fn code(&self) -> String {
        self.mail.sent.lock().unwrap().last().unwrap().1.clone()
    }
    fn register(&self, pin: &str) -> Result<Value, IdentityError> {
        self.identity.register(&Registration {
            email: "person@example.com".into(),
            password: PASSWORD.into(),
            first_name: "Test".into(),
            last_name: "User".into(),
            country_code: String::new(),
            phone: String::new(),
            pin: pin.into(),
        })
    }
    fn sql(&self, statement: &str, params: Vec<Value>) {
        self.store
            .transaction(true, |tx| tx.execute(statement, params))
            .unwrap();
    }
    fn value(&self, query: &str) -> Value {
        self.store
            .transaction(false, |tx| tx.rows(query, vec![]))
            .unwrap()
            .first()
            .map(|row| row[0].clone())
            .unwrap_or(Value::Null)
    }
    /// Registered, verified and signed in (local mode accepts any code).
    fn signed_in(&self) -> String {
        self.register("246810").unwrap();
        self.identity
            .verify("person@example.com", "000000", None)
            .unwrap();
        self.login()
    }
    fn login(&self) -> String {
        self.identity.login("person@example.com", PASSWORD).unwrap()["session"]
            .as_str()
            .unwrap()
            .to_string()
    }
}

fn code(result: Result<impl std::fmt::Debug, IdentityError>) -> &'static str {
    result.expect_err("refused").code
}

#[test]
fn verification_login_logout_and_reset() {
    let h = Harness::new(Verification::Email);
    h.register("246810").unwrap();
    let verify = h.code();
    assert_eq!(
        code(h.identity.login("person@example.com", PASSWORD)),
        "EMAIL_UNVERIFIED"
    );
    h.identity
        .verify("person@example.com", &verify, None)
        .unwrap();
    assert_eq!(
        code(h.identity.verify("person@example.com", &verify, None)),
        "INVALID_CODE"
    );
    let session = h.identity.login("PERSON@example.com", PASSWORD).unwrap()["session"]
        .as_str()
        .unwrap()
        .to_string();
    assert_eq!(
        h.identity.me(&session).unwrap()["email"],
        "person@example.com"
    );
    h.identity.resend("person@example.com", "reset").unwrap();
    let reset = h.code();
    assert_eq!(
        h.identity
            .verify(
                "person@example.com",
                &reset,
                Some("replacement-password-456")
            )
            .unwrap(),
        json!({"status": "password_reset"})
    );
    assert_eq!(code(h.identity.me(&session)), "SESSION_EXPIRED");
    assert_eq!(
        code(h.identity.login("person@example.com", PASSWORD)),
        "INVALID_CREDENTIALS"
    );
    let session = h
        .identity
        .login("person@example.com", "replacement-password-456")
        .unwrap()["session"]
        .as_str()
        .unwrap()
        .to_string();
    h.identity.logout(&session).unwrap();
    assert_eq!(code(h.identity.me(&session)), "SESSION_EXPIRED");
    assert_ne!(
        h.value("SELECT password FROM identity_accounts"),
        "replacement-password-456"
    );
    assert_eq!(h.value("SELECT digest FROM identity_sessions"), Value::Null);
}

#[test]
fn code_expiry_attempt_budget_and_resend() {
    let h = Harness::new(Verification::Email);
    h.register("246810").unwrap();
    let verify = h.code();
    let limited = h
        .identity
        .resend("person@example.com", "verify")
        .unwrap_err();
    assert_eq!(limited.status, 429);
    let wrong = if verify == "000000" {
        "999999"
    } else {
        "000000"
    };
    for _ in 0..5 {
        assert_eq!(
            code(h.identity.verify("person@example.com", wrong, None)),
            "INVALID_CODE"
        );
    }
    // The attempt budget is spent even for the correct code.
    assert_eq!(
        code(h.identity.verify("person@example.com", &verify, None)),
        "INVALID_CODE"
    );
    h.sql(
        "UPDATE identity_challenges SET sent_at = sent_at - 61",
        vec![],
    );
    h.identity.resend("person@example.com", "verify").unwrap();
    let fresh = h.code();
    h.sql("UPDATE identity_challenges SET expires = 0", vec![]);
    assert_eq!(
        code(h.identity.verify("person@example.com", &fresh, None)),
        "INVALID_CODE"
    );
}

#[test]
fn session_deadline_applies_to_profile_and_pin_without_rewriting() {
    let h = Harness::new(Verification::Local);
    let session = h.signed_in();
    h.sql("UPDATE identity_sessions SET expires = 1", vec![]);
    assert_eq!(code(h.identity.me(&session)), "SESSION_EXPIRED");
    assert_eq!(
        code(h.identity.security(&session, Action::Status)),
        "SESSION_EXPIRED"
    );
    assert_eq!(h.identity.session(&session), Session::Expired);
    assert_eq!(h.value("SELECT expires FROM identity_sessions"), json!(1.0));
}

#[test]
fn unsupported_credential_is_rejected_without_rewriting_account() {
    let h = Harness::new(Verification::Local);
    h.register("246810").unwrap();
    h.identity
        .verify("person@example.com", "000000", None)
        .unwrap();
    let unsupported = h
        .value("SELECT password FROM identity_accounts")
        .as_str()
        .unwrap()
        .to_uppercase();
    h.sql(
        "UPDATE identity_accounts SET password = ?",
        vec![json!(unsupported)],
    );
    assert_eq!(
        code(h.identity.login("person@example.com", PASSWORD)),
        "UNSUPPORTED_ACCOUNT_CREDENTIAL"
    );
    assert_eq!(
        h.value("SELECT password FROM identity_accounts"),
        json!(unsupported)
    );
    assert_eq!(h.value("SELECT failures FROM identity_accounts"), json!(0));
    assert_eq!(h.value("SELECT digest FROM identity_sessions"), Value::Null);
}

#[test]
fn delivery_failure_rolls_back_and_failed_login_is_limited() {
    let h = Harness::new(Verification::Email);
    h.mail.fail.store(true, Ordering::SeqCst);
    assert_eq!(code(h.register("246810")), "MAIL_FAILED");
    assert_eq!(h.value("SELECT id FROM identity_accounts"), Value::Null);
    assert_eq!(
        h.value("SELECT account_id FROM identity_pin_security"),
        Value::Null
    );
    h.mail.fail.store(false, Ordering::SeqCst);
    h.register("246810").unwrap();
    h.identity
        .verify("person@example.com", &h.code(), None)
        .unwrap();
    for _ in 0..5 {
        assert_eq!(
            code(h.identity.login("person@example.com", "wrong-password-000")),
            "INVALID_CREDENTIALS"
        );
    }
    let blocked = h
        .identity
        .login("person@example.com", PASSWORD)
        .unwrap_err();
    assert_eq!(blocked.status, 429);
    // Unknown addresses are refused the same way and create nothing.
    assert_eq!(
        code(h.identity.login("nobody@example.com", PASSWORD)),
        "INVALID_CREDENTIALS"
    );
    assert_eq!(code(h.register("246810")), "ACCOUNT_EXISTS");
}

#[test]
fn pin_is_hashed_and_not_the_local_email_code() {
    let h = Harness::new(Verification::Local);
    let session = h.signed_in();
    let encoded = h.value("SELECT pin_hash FROM identity_pin_security");
    assert!(!encoded.as_str().unwrap().contains("246810"));
    let state = h.identity.security(&session, Action::Lock).unwrap();
    let wrong = Action::Unlock {
        pin: "000000",
        expected: Some(state.revision),
    };
    assert_eq!(code(h.identity.security(&session, wrong)), "INVALID_PIN");
    assert!(
        h.identity
            .security(&session, Action::Status)
            .unwrap()
            .locked
    );
    assert_eq!(
        h.identity.session(&session),
        Session::Locked("person@example.com".into())
    );
    let right = Action::Unlock {
        pin: "246810",
        expected: Some(state.revision),
    };
    assert!(!h.identity.security(&session, right).unwrap().locked);
    assert_eq!(
        h.identity.session(&session),
        Session::Unlocked("person@example.com".into())
    );
}

#[test]
fn idle_and_activity_share_one_server_lock() {
    let h = Harness::new(Verification::Local);
    let first = h.signed_in();
    let second = h.login();
    h.sql(
        "UPDATE identity_pin_security SET last_activity = last_activity - 290",
        vec![],
    );
    assert!(
        !h.identity
            .security(&second, Action::Activity)
            .unwrap()
            .locked
    );
    h.sql(
        "UPDATE identity_pin_security SET last_activity = last_activity - 290",
        vec![],
    );
    assert!(!h.identity.security(&first, Action::Status).unwrap().locked);
    h.sql(
        "UPDATE identity_pin_security SET last_activity = last_activity - 11",
        vec![],
    );
    assert!(
        h.identity
            .security(&first, Action::Activity)
            .unwrap()
            .locked
    );
    assert!(h.identity.security(&second, Action::Status).unwrap().locked);
    assert_eq!(
        h.identity.session(&second),
        Session::Locked("person@example.com".into())
    );
}

#[test]
fn account_without_current_security_is_rejected_without_repair() {
    let h = Harness::new(Verification::Local);
    let session = h.signed_in();
    h.sql("DELETE FROM identity_pin_security", vec![]);
    assert_eq!(
        code(h.identity.security(&session, Action::Status)),
        "UNSUPPORTED_ACCOUNT_SECURITY"
    );
    assert_eq!(
        code(h.identity.login("person@example.com", PASSWORD)),
        "UNSUPPORTED_ACCOUNT_SECURITY"
    );
    assert_eq!(h.identity.session(&session), Session::Unsupported);
    assert_eq!(
        h.value("SELECT account_id FROM identity_pin_security"),
        Value::Null
    );
}

#[test]
fn change_pin_requires_account_password() {
    let h = Harness::new(Verification::Local);
    let session = h.signed_in();
    let refused = h.identity.security(
        &session,
        Action::Change {
            pin: "111111",
            password: "wrong-password",
        },
    );
    assert_eq!(code(refused), "INVALID_PIN");
    h.identity
        .security(
            &session,
            Action::Change {
                pin: "111111",
                password: PASSWORD,
            },
        )
        .unwrap();
    let state = h.identity.security(&session, Action::Lock).unwrap();
    let unlock = Action::Unlock {
        pin: "111111",
        expected: Some(state.revision),
    };
    assert!(!h.identity.security(&session, unlock).unwrap().locked);
}

#[test]
fn bruteforce_and_stale_unlock() {
    let h = Harness::new(Verification::Local);
    let session = h.signed_in();
    for seconds in [0, 30, 7200] {
        assert_eq!(
            code(h.identity.security(&session, Action::Timeout(seconds))),
            "INVALID_TIMEOUT"
        );
    }
    let state = h.identity.security(&session, Action::Lock).unwrap();
    assert_eq!(
        code(h.identity.security(&session, Action::Timeout(60))),
        "TERMINAL_LOCKED"
    );
    h.login();
    let newer = h.identity.security(&session, Action::Lock).unwrap();
    assert!(newer.revision > state.revision);
    let stale = Action::Unlock {
        pin: "246810",
        expected: Some(state.revision),
    };
    assert_eq!(code(h.identity.security(&session, stale)), "LOCK_CHANGED");
    let wrong = Action::Unlock {
        pin: "000000",
        expected: Some(newer.revision),
    };
    for _ in 0..5 {
        assert_eq!(code(h.identity.security(&session, wrong)), "INVALID_PIN");
    }
    let right = Action::Unlock {
        pin: "246810",
        expected: Some(newer.revision),
    };
    let blocked = h.identity.security(&session, right).unwrap_err();
    assert_eq!((blocked.status, blocked.code), (429, "PIN_RATE_LIMITED"));
    let state = h.identity.security(&session, Action::Status).unwrap();
    assert!(state.locked && state.retry_after > 0);
}

async fn serve(app: axum::Router) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    format!("http://{address}/api/v1/account")
}

async fn entry(verification: Verification, root: PathBuf) -> String {
    let store =
        Arc::new(Store::open(&format!("sqlite:///{}", root.join("api.db").display())).unwrap());
    let identity = Identity::new(
        store.clone(),
        SECRET,
        verification,
        Box::new(Mailer::new(root)),
    )
    .unwrap();
    let app = application(
        Settings {
            upstream: "http://127.0.0.1:9".into(),
            secret: SECRET.into(),
            require_account: false,
            lease_seconds: 60.0,
            data_root: std::env::temp_dir(),
        },
        RouteTable::parse("[]").unwrap(),
        Authorization::parse(AUTHORIZATION).unwrap(),
        store,
        Arc::new(identity),
    )
    .unwrap();
    serve(app).await
}

async fn call(
    method: Method,
    url: String,
    session: &str,
    body: Option<Value>,
) -> (StatusCode, Value) {
    let client = Client::builder(TokioExecutor::new()).build_http::<Body>();
    let request = axum::http::Request::builder()
        .method(method)
        .uri(url)
        .header("authorization", format!("Bearer {SECRET}"))
        .header("x-account-session", session)
        .header("content-type", "application/json")
        .body(body.map_or_else(Body::empty, |b| Body::from(b.to_string())))
        .unwrap();
    let response = client.request(request).await.unwrap();
    let status = response.status();
    let bytes: Bytes = response.into_body().collect().await.unwrap().to_bytes();
    (
        status,
        serde_json::from_slice(&bytes).unwrap_or(Value::Null),
    )
}

#[tokio::test]
async fn local_account_contract_over_http() {
    let api = entry(Verification::Local, tempfile::tempdir().unwrap().keep()).await;
    let url = |path: &str| format!("{api}{path}");
    let (_, capabilities) = call(Method::GET, url("/capabilities"), "", None).await;
    assert_eq!(
        capabilities,
        json!({"verification": "local", "code_length": 6})
    );
    let mut user = json!({
        "email": "Local@Example.com", "password": "local-password-123",
        "first_name": "Local", "last_name": "User", "ignored": true
    });
    let (status, error) = call(Method::POST, url("/register"), "", Some(user.clone())).await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    assert_eq!(
        error,
        json!({"detail": "请求参数格式不正确", "code": "INVALID_INPUT"})
    );
    user["pin"] = json!("246810");
    let (status, registered) = call(Method::POST, url("/register"), "", Some(user.clone())).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(
        registered,
        json!({"status": "local_verification_ready", "email": "local@example.com", "retry_after": 0})
    );
    let verify = |code: &str| json!({"email": "local@example.com", "code": code});
    assert_eq!(
        call(Method::POST, url("/verify"), "", Some(verify("123")))
            .await
            .0,
        StatusCode::UNPROCESSABLE_ENTITY
    );
    assert_eq!(
        call(Method::POST, url("/verify"), "", Some(verify("000000"))).await,
        (StatusCode::OK, json!({"status": "verified"}))
    );
    let (status, refused) = call(Method::POST, url("/verify"), "", Some(verify("999999"))).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(refused["code"], "INVALID_CODE");
    let credentials = json!({"email": "local@example.com", "password": "local-password-123"});
    let (status, login) = call(Method::POST, url("/login"), "", Some(credentials)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(
        login["user"],
        json!({"email": "local@example.com", "first_name": "Local", "last_name": "User"})
    );
    let session = login["session"].as_str().unwrap().to_string();
    assert_eq!(
        call(Method::GET, url("/me"), &session, None).await.0,
        StatusCode::OK
    );
    let (status, expired) = call(Method::GET, url("/me"), "", None).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert_eq!(expired["code"], "SESSION_EXPIRED");

    let (status, state) = call(Method::POST, url("/security/lock"), &session, None).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(state["locked"], true);
    for key in [
        "timeout_seconds",
        "remaining_seconds",
        "revision",
        "retry_after",
    ] {
        assert!(state[key].is_number(), "{key}");
    }
    let revision = state["revision"].as_i64().unwrap();
    let unlock = |expected: i64| json!({"pin": "246810", "expected": expected});
    assert_eq!(
        call(
            Method::POST,
            url("/security/unlock"),
            &session,
            Some(unlock(revision - 1))
        )
        .await
        .0,
        StatusCode::CONFLICT
    );
    assert_eq!(
        call(
            Method::POST,
            url("/security/unlock"),
            &session,
            Some(unlock(revision))
        )
        .await
        .0,
        StatusCode::OK
    );
    let (_, state) = call(
        Method::POST,
        url("/security/timeout"),
        &session,
        Some(json!({"seconds": 60})),
    )
    .await;
    assert_eq!(state["timeout_seconds"], 60);
    assert_eq!(
        call(
            Method::POST,
            url("/security/timeout"),
            &session,
            Some(json!({"seconds": "60"}))
        )
        .await
        .0,
        StatusCode::UNPROCESSABLE_ENTITY
    );
    let email = json!({"email": "local@example.com"});
    assert_eq!(
        call(Method::POST, url("/forgot"), "", Some(email)).await,
        (
            StatusCode::OK,
            json!({"status": "local_verification_ready", "retry_after": 0})
        )
    );
    let reset = json!({"email": "local@example.com", "code": "654321", "password": "replacement-password-123"});
    assert_eq!(
        call(Method::POST, url("/reset"), "", Some(reset)).await,
        (StatusCode::OK, json!({"status": "password_reset"}))
    );
    assert_eq!(
        call(Method::POST, url("/logout"), &session, None).await,
        (StatusCode::OK, json!({"status": "signed_out"}))
    );
}

#[tokio::test]
async fn email_mode_requires_configured_delivery_and_valid_credentials() {
    let api = entry(Verification::Email, tempfile::tempdir().unwrap().keep()).await;
    let (_, capabilities) = call(Method::GET, format!("{api}/capabilities"), "", None).await;
    assert_eq!(capabilities["verification"], "email");
    let user = json!({
        "email": "person@example.com", "password": PASSWORD, "pin": "246810",
        "first_name": "Test", "last_name": "User"
    });
    let (status, error) = call(Method::POST, format!("{api}/register"), "", Some(user)).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(error["code"], "MAIL_NOT_CONFIGURED");
    let client = Client::builder(TokioExecutor::new()).build_http::<Body>();
    let request = axum::http::Request::get(format!("{api}/capabilities"))
        .header("authorization", "Bearer wrong")
        .body(Body::empty())
        .unwrap();
    assert_eq!(
        client.request(request).await.unwrap().status(),
        StatusCode::UNAUTHORIZED
    );
}

/// A minimal SMTP server that records one message; `refuse` rejects RCPT.
fn smtp(refuse: bool) -> (u16, std::thread::JoinHandle<String>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = std::thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        let mut writer = stream.try_clone().unwrap();
        let mut reader = BufReader::new(stream);
        let mut data = String::new();
        writer.write_all(b"220 test ESMTP\r\n").unwrap();
        let mut line = String::new();
        while reader.read_line(&mut line).unwrap_or(0) > 0 {
            let command = line.to_ascii_uppercase();
            let reply: &[u8] = if command.starts_with("EHLO") || command.starts_with("HELO") {
                b"250 test\r\n"
            } else if command.starts_with("RCPT") && refuse {
                b"550 no such user\r\n"
            } else if command.starts_with("DATA") {
                writer.write_all(b"354 go\r\n").unwrap();
                let mut body = String::new();
                while reader.read_line(&mut body).unwrap() > 0 && !body.ends_with("\r\n.\r\n") {}
                data = body;
                b"250 queued\r\n"
            } else if command.starts_with("QUIT") {
                writer.write_all(b"221 bye\r\n").unwrap();
                break;
            } else {
                b"250 ok\r\n"
            };
            writer.write_all(reply).unwrap();
            line.clear();
        }
        data
    });
    (port, server)
}

fn mailer(port: u16, security: &str) -> Mailer {
    let root = tempfile::tempdir().unwrap().keep();
    std::fs::write(
        root.join("identity-mail.json"),
        json!({"host": "127.0.0.1", "port": port, "security": security, "sender": "sender@example.com"})
            .to_string(),
    )
    .unwrap();
    Mailer::new(root)
}

#[test]
fn smtp_delivers_the_code_and_reports_refusals() {
    let (port, server) = smtp(false);
    mailer(port, "none")
        .send("person@example.com", "123456", "verify")
        .unwrap();
    let message = server.join().unwrap();
    let (headers, body) = message.split_once("\r\n\r\n").unwrap();
    assert!(headers.contains("To: person@example.com"));
    assert!(headers.contains("Content-Transfer-Encoding: base64"));
    let encoded: String = body.lines().take_while(|line| *line != ".").collect();
    let text = String::from_utf8(STANDARD.decode(encoded).unwrap()).unwrap();
    assert!(text.contains("验证码是：123456"));

    let (port, server) = smtp(true);
    let refused = mailer(port, "none")
        .send("person@example.com", "123456", "verify")
        .unwrap_err();
    assert_eq!(refused.code, "MAIL_FAILED");
    assert!(refused.message.contains("拒绝了收件地址"));
    drop(server);

    // TLS is required but the server only speaks plain SMTP.
    let (port, _server) = smtp(false);
    let failed = mailer(port, "tls")
        .send("person@example.com", "123456", "verify")
        .unwrap_err();
    assert_eq!(failed.code, "MAIL_FAILED");
}
