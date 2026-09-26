//! Sync task admission for the workbench and partition progress for workers,
//! from the data service library.
use crate::access::Unlocked;
use crate::context::Trace;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_data::configuration::SourceError;
use asterion_data::credentials::Credentials;
use asterion_data::sync::{self, Submission};
use asterion_kernel::artifacts::ArtifactStore;
use axum::{
    Extension,
    body::Bytes,
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use serde::Deserialize;
use serde_json::json;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

fn failure(error: SourceError) -> Failure {
    match error {
        SourceError::Refused(message) => {
            Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
        }
        SourceError::Conflict(message) => Failure::new(StatusCode::CONFLICT, &message, "CONFLICT"),
        SourceError::Store(_) => Failure::internal(),
    }
}

async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, Failure> + Send + 'static,
) -> Result<T, Failure> {
    tokio::task::spawn_blocking(work)
        .await
        .map_err(|_| Failure::internal())?
}

/// Admit and queue a sync task; repeating a command returns its task.
async fn submit(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Extension(Trace(trace)): Extension<Trace>,
    content: Bytes,
) -> Result<Response, Failure> {
    let today = chrono::Utc::now()
        .with_timezone(&chrono_tz::Asia::Shanghai)
        .date_naive();
    let submission = crate::input::body(&content, |body: &Submission| {
        body.valid(today).then_some(()).ok_or(crate::input::Invalid)
    })?;
    let waker = entry.clone();
    let job = blocking(move || {
        let store =
            ArtifactStore::reader(&entry.data_root, 1 << 30).map_err(|_| Failure::internal())?;
        let credentials = Credentials::new(&entry.secret, &entry.data_root);
        entry.store.transaction(true, |tx| {
            sync::admit(
                tx,
                &store,
                &entry.tasks,
                &credentials,
                &submission,
                now(),
                &trace,
            )
            .map_err(failure)
        })
    })
    .await?;
    waker.syncs.notify_one();
    Ok((StatusCode::ACCEPTED, axum::Json(sync::public(&job))).into_response())
}

#[derive(Deserialize)]
struct Progress {
    token: String,
    completed: i64,
    total: i64,
}

async fn progress(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    Path(job_id): Path<String>,
    content: Bytes,
) -> Result<Response, Failure> {
    let body = crate::input::body(&content, |_: &Progress| Ok(()))?;
    blocking(move || {
        entry.store.transaction(true, |tx| {
            sync::progress(
                tx,
                &entry.tasks,
                &job_id,
                &body.token,
                body.completed,
                body.total,
                now(),
                &trace,
            )
            .map_err(failure)
        })
    })
    .await?;
    Ok(axum::Json(json!({"status": "updated"})).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::post("/api/v1/data/sync", submit),
        Operation::post("/api/v1/jobs/{job_id}/progress", progress),
    ]
}
