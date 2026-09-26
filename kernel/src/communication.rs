//! Fixed runtime communication rules. Transports carry values; this module owns
//! deadlines, causal context, response binding and sanitized protocol failures.

use asterion_foundation::{
    communication::{MAX_SAFE_INTEGER, parse_json, validate},
    wire_generated::Context,
};
use serde_json::{Value, json};
use std::{
    fmt,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use uuid::Uuid;

pub const MESSAGE_LIMIT: usize = 8_000_000;
pub const CONTEXT_LIMIT: usize = 2048;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Invalid,
    Deadline,
    Mismatch,
    Remote,
}

impl fmt::Display for Error {
    fn fmt(&self, output: &mut fmt::Formatter<'_>) -> fmt::Result {
        output.write_str(match self {
            Self::Invalid => "Invalid communication value or budget",
            Self::Deadline => "Communication deadline exceeded",
            Self::Mismatch => "Communication response context mismatch",
            Self::Remote => "Remote operation failed",
        })
    }
}
impl std::error::Error for Error {}

pub fn now_ms() -> Result<u64, Error> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| Error::Invalid)?
        .as_millis();
    u64::try_from(milliseconds).map_err(|_| Error::Invalid)
}

pub fn parse_context(value: Value) -> Result<Context, Error> {
    validate("Context", &value).map_err(|_| Error::Invalid)?;
    serde_json::from_value(value).map_err(|_| Error::Invalid)
}

fn check_context(trace: &Context) -> Result<(), Error> {
    validate("Context", &json!(trace)).map_err(|_| Error::Invalid)
}

pub fn ensure_active_at(trace: &Context, now: u64) -> Result<(), Error> {
    check_context(trace)?;
    if now >= trace.deadline_ms {
        return Err(Error::Deadline);
    }
    Ok(())
}

pub fn ensure_active(trace: &Context) -> Result<(), Error> {
    ensure_active_at(trace, now_ms()?)
}

/// A transport captures this wall-clock budget as an Instant-based deadline too,
/// so adjusting the system clock backwards cannot extend an in-flight wait.
pub fn remaining(trace: &Context) -> Result<Duration, Error> {
    let now = now_ms()?;
    ensure_active_at(trace, now)?;
    Ok(Duration::from_millis(trace.deadline_ms - now))
}

pub fn context_at(
    parent: Option<&Context>,
    timeout_seconds: f64,
    now: u64,
) -> Result<Context, Error> {
    if timeout_seconds == 0.0 {
        return Err(Error::Deadline);
    }
    let milliseconds = timeout_seconds * 1000.0;
    if !milliseconds.is_finite() || milliseconds < 1.0 || milliseconds > MAX_SAFE_INTEGER as f64 {
        return Err(Error::Invalid);
    }
    let deadline = now
        .checked_add(milliseconds.floor() as u64)
        .filter(|n| *n <= MAX_SAFE_INTEGER)
        .ok_or(Error::Invalid)?;
    if let Some(parent) = parent {
        ensure_active_at(parent, now)?;
    }
    let id = Uuid::new_v4().simple().to_string();
    let trace = Context {
        version: 1,
        request_id: id.clone(),
        correlation_id: parent.map_or(id, |parent| parent.correlation_id.clone()),
        causation_id: parent.map_or(Value::Null, |parent| json!(parent.request_id)),
        deadline_ms: parent.map_or(deadline, |parent| deadline.min(parent.deadline_ms)),
    };
    check_context(&trace)?;
    Ok(trace)
}

pub fn context(parent: Option<&Context>, timeout_seconds: f64) -> Result<Context, Error> {
    context_at(parent, timeout_seconds, now_ms()?)
}

/// A committed asynchronous fact starts a new execution budget. It preserves
/// causal identity, without inheriting the already-finished caller's deadline.
pub fn continuation(
    correlation_id: &str,
    causation_id: &str,
    timeout_seconds: f64,
) -> Result<Context, Error> {
    let mut trace = context(None, timeout_seconds)?;
    trace.correlation_id = correlation_id.into();
    trace.causation_id = json!(causation_id);
    check_context(&trace)?;
    Ok(trace)
}

pub fn ingress(raw: Option<&[u8]>, timeout_seconds: f64) -> Result<Context, Error> {
    match raw {
        None => context(None, timeout_seconds),
        Some(raw) => {
            if raw.len() > CONTEXT_LIMIT {
                return Err(Error::Invalid);
            }
            let trace = parse_context(parse_json(raw).map_err(|_| Error::Invalid)?)?;
            ensure_active(&trace)?;
            Ok(trace)
        }
    }
}

pub fn prepare_call(
    trace: &Context,
    kind: &str,
    contract: &str,
    payload: Value,
) -> Result<Value, Error> {
    ensure_active(trace)?;
    let value = json!({"context":trace,"kind":kind,"contract":contract,"payload":payload});
    validate("Call", &value).map_err(|_| Error::Invalid)?;
    Ok(value)
}

pub fn prepare_reply(trace: &Context, result: Value, error: Value) -> Result<Value, Error> {
    // An expired operation may still return a sanitized failure. Consumers must
    // reject late results; producing the failure does not renew the deadline.
    let value = json!({"context":trace,"result":result,"error":error});
    validate("Reply", &value).map_err(|_| Error::Invalid)?;
    Ok(value)
}

pub fn reply_result_at(trace: &Context, response: Value, now: u64) -> Result<Value, Error> {
    validate("Reply", &response).map_err(|_| Error::Invalid)?;
    let received = parse_context(response["context"].clone())?;
    if json!(received) != json!(trace) {
        return Err(Error::Mismatch);
    }
    ensure_active_at(trace, now)?;
    if !response["error"].is_null() {
        return Err(Error::Remote);
    }
    Ok(response["result"].clone())
}

pub fn reply_result(trace: &Context, response: Value) -> Result<Value, Error> {
    reply_result_at(trace, response, now_ms()?)
}

pub fn decode_reply(trace: &Context, raw: &[u8]) -> Result<Value, Error> {
    if raw.len() > MESSAGE_LIMIT {
        return Err(Error::Invalid);
    }
    let response = parse_json(raw).map_err(|_| Error::Invalid)?;
    reply_result(trace, response)
}
