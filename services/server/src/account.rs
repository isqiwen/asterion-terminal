//! HTTP operations of local accounts under `/api/v1/account`. Request bodies
//! follow the published contract (see `input`), and account refusals carry
//! their code.
use crate::identity::{Action, Identity, IdentityError, Registration};
use crate::input::{Invalid, body, length, pattern};
use crate::{Authorized, Entry, Failure, Operation, session_token};
use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use regex::Regex;
use serde::Deserialize;
use serde_json::json;
use std::sync::{Arc, LazyLock};

static PIN: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[0-9]{6}$").expect("pattern"));
static CODE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\d{6}$").expect("pattern"));
static COUNTRY: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\+?[0-9]*$").expect("pattern"));
static PHONE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[0-9 ()-]*$").expect("pattern"));

#[derive(Deserialize)]
struct Email {
    email: String,
}
#[derive(Deserialize)]
struct Login {
    email: String,
    password: String,
}
#[derive(Deserialize)]
struct Register {
    email: String,
    password: String,
    pin: String,
    #[serde(default)]
    country_code: String,
    #[serde(default)]
    phone: String,
    first_name: String,
    last_name: String,
}
#[derive(Deserialize)]
struct Code {
    email: String,
    code: String,
}
#[derive(Deserialize)]
struct Reset {
    email: String,
    code: String,
    password: String,
}
#[derive(Deserialize)]
struct PinInput {
    pin: String,
    #[serde(default)]
    expected: Option<i64>,
    #[serde(default)]
    password: Option<String>,
}
#[derive(Deserialize)]
struct LockTimeout {
    seconds: i64,
}

fn refused(error: IdentityError) -> Failure {
    let status = StatusCode::from_u16(error.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR);
    Failure::new(status, &error.message, error.code)
}

async fn run<T: serde::Serialize + Send + 'static>(
    entry: &Entry,
    work: impl FnOnce(&Identity) -> Result<T, IdentityError> + Send + 'static,
) -> Result<Response, Failure> {
    let identity = entry.identity.clone();
    match tokio::task::spawn_blocking(move || work(&identity)).await {
        Ok(Ok(value)) => Ok(axum::Json(value).into_response()),
        Ok(Err(error)) => Err(refused(error)),
        Err(_) => Err(Failure::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "账户服务暂不可用，请稍后重试",
            "INTERNAL_ERROR",
        )),
    }
}

type Shared = State<Arc<Entry>>;
type Outcome = Result<Response, Failure>;

async fn capabilities(State(entry): Shared, _: Authorized) -> Response {
    axum::Json(json!({"verification": entry.identity.verification().name(), "code_length": 6}))
        .into_response()
}

async fn register(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    let b = body::<Register>(&content, |b| {
        length(&b.email, 0, 254)?;
        length(&b.password, 1, 128)?;
        pattern(&b.pin, &PIN)?;
        length(&b.country_code, 0, 6)?;
        pattern(&b.country_code, &COUNTRY)?;
        length(&b.phone, 0, 30)?;
        pattern(&b.phone, &PHONE)?;
        length(&b.first_name, 1, 80)?;
        length(&b.last_name, 1, 80)
    })?;
    run(&entry, move |identity| {
        identity.register(&Registration {
            email: b.email,
            password: b.password,
            first_name: b.first_name,
            last_name: b.last_name,
            country_code: b.country_code,
            phone: b.phone,
            pin: b.pin,
        })
    })
    .await
}

async fn login(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    let b = body::<Login>(&content, |b| {
        length(&b.email, 0, 254)?;
        length(&b.password, 1, 128)
    })?;
    run(&entry, move |identity| {
        identity.login(&b.email, &b.password)
    })
    .await
}

async fn verify(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    let b = body::<Code>(&content, |b| {
        length(&b.email, 0, 254)?;
        pattern(&b.code, &CODE)
    })?;
    run(&entry, move |identity| {
        identity.verify(&b.email, &b.code, None)
    })
    .await
}

async fn reset(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    let b = body::<Reset>(&content, |b| {
        length(&b.email, 0, 254)?;
        pattern(&b.code, &CODE)?;
        length(&b.password, 12, 128)
    })?;
    run(&entry, move |identity| {
        identity.verify(&b.email, &b.code, Some(&b.password))
    })
    .await
}

async fn resend_for(entry: Arc<Entry>, content: Bytes, purpose: &'static str) -> Outcome {
    let b = body::<Email>(&content, |b| length(&b.email, 0, 254))?;
    run(&entry, move |identity| identity.resend(&b.email, purpose)).await
}
async fn resend(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    resend_for(entry, content, "verify").await
}
async fn forgot(State(entry): Shared, _: Authorized, content: Bytes) -> Outcome {
    resend_for(entry, content, "reset").await
}

async fn me(State(entry): Shared, _: Authorized, headers: HeaderMap) -> Outcome {
    let token = session_token(&headers);
    run(&entry, move |identity| identity.me(&token)).await
}

async fn logout(State(entry): Shared, _: Authorized, headers: HeaderMap) -> Outcome {
    let token = session_token(&headers);
    run(&entry, move |identity| identity.logout(&token)).await
}

async fn security_with(
    entry: Arc<Entry>,
    headers: HeaderMap,
    action: fn() -> Action<'static>,
) -> Outcome {
    let token = session_token(&headers);
    run(&entry, move |identity| identity.security(&token, action())).await
}
async fn security(State(entry): Shared, _: Authorized, headers: HeaderMap) -> Outcome {
    security_with(entry, headers, || Action::Status).await
}
async fn activity(State(entry): Shared, _: Authorized, headers: HeaderMap) -> Outcome {
    security_with(entry, headers, || Action::Activity).await
}
async fn lock(State(entry): Shared, _: Authorized, headers: HeaderMap) -> Outcome {
    security_with(entry, headers, || Action::Lock).await
}

fn pin_input(b: &PinInput) -> Result<(), Invalid> {
    pattern(&b.pin, &PIN)?;
    if let Some(password) = &b.password {
        length(password, 0, 128)?;
    }
    Ok(())
}

async fn unlock(
    State(entry): Shared,
    _: Authorized,
    headers: HeaderMap,
    content: Bytes,
) -> Outcome {
    let token = session_token(&headers);
    let b = body::<PinInput>(&content, pin_input)?;
    run(&entry, move |identity| {
        identity.security(
            &token,
            Action::Unlock {
                pin: &b.pin,
                expected: b.expected,
            },
        )
    })
    .await
}

async fn change(
    State(entry): Shared,
    _: Authorized,
    headers: HeaderMap,
    content: Bytes,
) -> Outcome {
    let token = session_token(&headers);
    let b = body::<PinInput>(&content, pin_input)?;
    run(&entry, move |identity| {
        identity.security(
            &token,
            Action::Change {
                pin: &b.pin,
                password: b.password.as_deref().unwrap_or(""),
            },
        )
    })
    .await
}

async fn timeout(
    State(entry): Shared,
    _: Authorized,
    headers: HeaderMap,
    content: Bytes,
) -> Outcome {
    let token = session_token(&headers);
    let b = body::<LockTimeout>(&content, |_| Ok(()))?;
    run(&entry, move |identity| {
        identity.security(&token, Action::Timeout(b.seconds))
    })
    .await
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::get("/api/v1/account/capabilities", capabilities),
        Operation::post("/api/v1/account/register", register),
        Operation::post("/api/v1/account/login", login),
        Operation::post("/api/v1/account/verify", verify),
        Operation::post("/api/v1/account/resend", resend),
        Operation::post("/api/v1/account/forgot", forgot),
        Operation::post("/api/v1/account/reset", reset),
        Operation::get("/api/v1/account/me", me),
        Operation::post("/api/v1/account/logout", logout),
        Operation::get("/api/v1/account/security", security),
        Operation::post("/api/v1/account/security/activity", activity),
        Operation::post("/api/v1/account/security/lock", lock),
        Operation::post("/api/v1/account/security/unlock", unlock),
        Operation::post("/api/v1/account/security/change", change),
        Operation::post("/api/v1/account/security/timeout", timeout),
    ]
}
