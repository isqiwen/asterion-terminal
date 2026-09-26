//! Immutable trading-time versions and timestamp diagnostics. Specifications,
//! fingerprints and session resolution are the fixed futures-domain calendar's;
//! this module stores versions and binds HTTP to it.
use crate::access::Unlocked;
use crate::input::{Invalid, body};
use crate::store::{Kind, Table};
use crate::{Authorized, Entry, Failure, Operation};
use asterion_trading_calendar::{Boundary, Calendar, TimeSpec, TimeVersion, Validate};
use axum::{
    body::Bytes,
    extract::State,
    http::StatusCode,
    response::{IntoResponse, Response},
};
use chrono::{DateTime, NaiveDate};
use serde::Deserialize;
use serde_json::json;
use std::sync::Arc;

pub(crate) const TABLES: [Table; 1] = [Table {
    name: "trading_time_versions",
    columns: &[("id", Kind::Text), ("spec", Kind::Json)],
    primary: &["id"],
    unique: &[],
}];

fn valid<T: Validate>(value: &T) -> Result<(), Invalid> {
    value.validate().map_err(|_| Invalid)
}

/// Domain refusals keep their message, as input the domain cannot accept.
fn refused(message: String) -> Failure {
    Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
}

async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, Failure> + Send + 'static,
) -> Result<T, Failure> {
    tokio::task::spawn_blocking(work)
        .await
        .map_err(|_| Failure::internal())?
}

async fn listing(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
) -> Result<Response, Failure> {
    let rows = blocking(move || {
        entry.store.transaction(false, |tx| {
            tx.rows(
                "SELECT id, spec FROM trading_time_versions ORDER BY id",
                vec![],
            )
            .map_err(Failure::from)
        })
    })
    .await?;
    let versions = rows
        .into_iter()
        .map(|row| {
            // A stored version is served only if it is a current, intact one.
            let spec = row[1].as_str().ok_or_else(Failure::internal)?;
            let version = TimeVersion {
                id: row[0].as_str().ok_or_else(Failure::internal)?.into(),
                spec: serde_json::from_str(spec).map_err(|_| unsupported())?,
            };
            version.validate().map_err(|_| unsupported())?;
            Ok(version)
        })
        .collect::<Result<Vec<_>, Failure>>()?;
    Ok(axum::Json(versions).into_response())
}

fn unsupported() -> Failure {
    Failure::new(
        StatusCode::CONFLICT,
        "已保存的交易时间版本不符合当前契约",
        "UNSUPPORTED_TIME_VERSION",
    )
}

async fn save(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    content: Bytes,
) -> Result<Response, Failure> {
    let spec = body::<TimeSpec>(&content, valid)?;
    let calendar = Calendar::new(spec).map_err(refused)?;
    let version = TimeVersion {
        id: calendar.id().into(),
        spec: calendar.spec().clone(),
    };
    let row = vec![
        json!(version.id),
        json!(serde_json::to_string(&version.spec).map_err(|_| Failure::internal())?),
    ];
    blocking(move || {
        entry.store.transaction(true, |tx| {
            tx.execute(
                "INSERT INTO trading_time_versions (id, spec) VALUES (?, ?) \
                 ON CONFLICT (id) DO NOTHING",
                row,
            )
            .map_err(Failure::from)
        })
    })
    .await?;
    Ok(axum::Json(version).into_response())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ResolveRequest {
    version: TimeVersion,
    contract: String,
    timestamp: String,
    boundary: Boundary,
}

async fn resolve(_: Authorized, _: Unlocked, content: Bytes) -> Result<Response, Failure> {
    let request = body::<ResolveRequest>(&content, |r| valid(&r.version))?;
    let stamp = DateTime::parse_from_rfc3339(&request.timestamp).map_err(|_| Failure::invalid())?;
    let calendar = Calendar::new(request.version.spec).map_err(refused)?;
    let span = calendar
        .resolve(&request.contract, stamp, request.boundary)
        .map_err(refused)?;
    Ok(axum::Json(span).into_response())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct DayRequest {
    version: TimeVersion,
    contract: String,
    trading_day: NaiveDate,
}

async fn daily(_: Authorized, _: Unlocked, content: Bytes) -> Result<Response, Failure> {
    let request = body::<DayRequest>(&content, |r| valid(&r.version))?;
    let calendar = Calendar::new(request.version.spec).map_err(refused)?;
    let spans = calendar
        .daily(&request.contract, request.trading_day)
        .map_err(refused)?;
    Ok(axum::Json(spans).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::get("/api/v1/trading-time", listing),
        Operation::post("/api/v1/trading-time", save),
        Operation::post("/api/v1/trading-time/resolve", resolve),
        Operation::post("/api/v1/trading-time/day", daily),
    ]
}
