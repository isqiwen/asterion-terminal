//! Source observations of sync tasks: retained by workers under their lease,
//! read back by the workbench.
use crate::access::Unlocked;
use crate::context::Trace;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_data::observations::{self, EvidenceError, Observation};
use asterion_kernel::artifacts::ArtifactStore;
use axum::{
    Extension,
    body::Bytes,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

fn failure(error: EvidenceError, missing: &str) -> Failure {
    match error {
        EvidenceError::Refused(message) => {
            Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
        }
        EvidenceError::Conflict(message) => {
            Failure::new(StatusCode::CONFLICT, &message, "CONFLICT")
        }
        EvidenceError::NotFound => Failure::new(StatusCode::NOT_FOUND, missing, "NOT_FOUND"),
        EvidenceError::Store(_) => Failure::internal(),
    }
}

fn artifacts(entry: &Entry, writable: bool) -> Result<ArtifactStore, Failure> {
    let open = if writable {
        ArtifactStore::new
    } else {
        ArtifactStore::reader
    };
    open(&entry.data_root, 1 << 30).map_err(|_| Failure::internal())
}

fn lease_token(headers: &HeaderMap) -> Result<String, Failure> {
    headers
        .get("x-lease-token")
        .and_then(|value| value.to_str().ok())
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .ok_or_else(Failure::invalid)
}

async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, Failure> + Send + 'static,
) -> Result<T, Failure> {
    tokio::task::spawn_blocking(work)
        .await
        .map_err(|_| Failure::internal())?
}

async fn resume(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    Path(job_id): Path<String>,
    headers: HeaderMap,
) -> Result<Response, Failure> {
    let token = lease_token(&headers)?;
    let reused = blocking(move || {
        let store = artifacts(&entry, true)?;
        entry.store.transaction(true, |tx| {
            observations::resume(tx, &store, &entry.tasks, &job_id, &token, now(), &trace)
                .map_err(|error| failure(error, "采集任务不存在"))
        })
    })
    .await?;
    Ok(axum::Json(reused).into_response())
}

async fn record(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    Path((job_id, index)): Path<(String, i64)>,
    headers: HeaderMap,
    content: Bytes,
) -> Result<Response, Failure> {
    let token = lease_token(&headers)?;
    if content.len() > observations::MAX_BYTES {
        return Err(Failure::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "分段证据超过 8 MB",
            "INVALID_INPUT",
        ));
    }
    let invalid = || {
        Failure::new(
            StatusCode::UNPROCESSABLE_ENTITY,
            "分段证据格式错误",
            "INVALID_INPUT",
        )
    };
    let value: Value = serde_json::from_slice(&content).map_err(|_| invalid())?;
    let value = Observation::parse(&value).map_err(|error| failure(error, ""))?;
    let manifest = blocking(move || {
        let store = artifacts(&entry, true)?;
        entry.store.transaction(true, |tx| {
            observations::record(
                tx,
                &store,
                &entry.tasks,
                &job_id,
                &token,
                index,
                &value,
                now(),
                &trace,
            )
            .map_err(|error| failure(error, "采集任务不存在"))
        })
    })
    .await?;
    Ok(axum::Json(manifest).into_response())
}

fn bounded(
    query: &HashMap<String, String>,
    name: &str,
    default: i64,
    range: std::ops::RangeInclusive<i64>,
) -> Result<i64, Failure> {
    let value = match query.get(name) {
        Some(text) => text.parse().map_err(|_| Failure::invalid())?,
        None => default,
    };
    range
        .contains(&value)
        .then_some(value)
        .ok_or_else(Failure::invalid)
}

async fn list(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(job_id): Path<String>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, Failure> {
    let offset = bounded(&query, "offset", 0, 0..=i64::MAX)?;
    let limit = bounded(&query, "limit", 50, 1..=100)?;
    let page = blocking(move || {
        entry.store.transaction(false, |tx| {
            observations::list(tx, &job_id, offset, limit)
                .map_err(|error| failure(error, "采集任务不存在"))
        })
    })
    .await?;
    Ok(axum::Json(page).into_response())
}

async fn preview(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path((job_id, attempt, index)): Path<(String, i64, i64)>,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, Failure> {
    let offset = bounded(&query, "offset", 0, 0..=i64::MAX)?;
    let limit = bounded(&query, "limit", 100, 1..=500)?;
    let value = blocking(move || {
        let store = artifacts(&entry, false)?;
        entry.store.transaction(false, |tx| {
            observations::preview(
                tx,
                &store,
                &job_id,
                attempt,
                index,
                usize::try_from(offset).unwrap_or(usize::MAX),
                limit as usize,
            )
            .map_err(|error| failure(error, "采集证据不存在"))
        })
    })
    .await?;
    Ok(axum::Json(value).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::post("/api/v1/jobs/{job_id}/resume", resume),
        Operation::post("/api/v1/jobs/{job_id}/observations/{index}", record)
            .limited(observations::MAX_BYTES + 1),
        Operation::get("/api/v1/data/jobs/{job_id}/observations", list),
        Operation::get(
            "/api/v1/data/jobs/{job_id}/observations/{attempt}/{index}",
            preview,
        ),
    ]
}
