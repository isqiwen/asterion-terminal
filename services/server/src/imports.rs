//! CSV imports and chart snapshots. Import tasks are executed here, by the
//! entry itself: it claims only import tasks, and the internal process's
//! worker never claims them.
use crate::access::Unlocked;
use crate::context::Trace;
use crate::tasks::job;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_data::imports::{self, ExecuteError, ImportOptions, KIND, RawOptions};
use asterion_data::snapshots::{self, SnapshotError};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::communication;
use asterion_kernel::database::OperationError;
use asterion_kernel::tasks::{Action, ClaimKinds, repository::Request};
use axum::{
    Extension,
    body::Bytes,
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use serde::Deserialize;
use serde_json::{Value, json};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

fn refused(message: String) -> Failure {
    Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
}

fn artifacts(entry: &Entry, writable: bool) -> Result<ArtifactStore, Failure> {
    let open = if writable {
        ArtifactStore::new
    } else {
        ArtifactStore::reader
    };
    open(&entry.data_root, 1 << 30).map_err(|_| Failure::internal())
}

#[derive(Deserialize)]
struct ImportRequest {
    command_id: String,
    source: String,
    csv: String,
    options: RawOptions,
}

fn request(content: &Bytes) -> Result<(ImportRequest, ImportOptions), Failure> {
    let request: ImportRequest = serde_json::from_slice(content).map_err(|_| Failure::invalid())?;
    let within = |text: &str, max: usize| (1..=max).contains(&text.chars().count());
    if !within(&request.command_id, 100)
        || !within(&request.source, 200)
        || !within(&request.csv, 2_000_000)
    {
        return Err(Failure::invalid());
    }
    let options = ImportOptions::new(request.options.clone()).map_err(|_| Failure::invalid())?;
    Ok((request, options))
}

async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, Failure> + Send + 'static,
) -> Result<T, Failure> {
    tokio::task::spawn_blocking(work)
        .await
        .map_err(|_| Failure::internal())?
}

async fn submit(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Extension(Trace(trace)): Extension<Trace>,
    content: Bytes,
) -> Result<Response, Failure> {
    let (request, options) = request(&content)?;
    let state = entry.clone();
    let row = blocking(move || {
        let store = artifacts(&state, false)?;
        state.store.transaction(false, |tx| {
            imports::validate_inputs(tx, &store, &options).map_err(refused)
        })?;
        imports::encode(&request.csv, &options).map_err(refused)?;
        let record = json!({
            "id": uuid::Uuid::new_v4().to_string(), "command_id": request.command_id,
            "kind": KIND, "now": now(),
            "payload": {"source": request.source, "csv": request.csv, "options": options.dump()},
        });
        let submit: Request = serde_json::from_value(json!({"op": "submit", "record": record}))
            .map_err(|_| Failure::internal())?;
        state.store.transaction(true, |tx| {
            state
                .tasks
                .run(tx.connection(), submit, trace)
                .map_err(|error| match error {
                    OperationError::Contract(message) => {
                        Failure::new(StatusCode::CONFLICT, &message, "CONFLICT")
                    }
                    OperationError::Database(_) => Failure::internal(),
                })
        })
    })
    .await?;
    entry.imports.notify_one();
    Ok((StatusCode::ACCEPTED, axum::Json(job(&row))).into_response())
}

async fn preview(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    content: Bytes,
) -> Result<Response, Failure> {
    let (request, options) = request(&content)?;
    let value = blocking(move || {
        let store = artifacts(&entry, false)?;
        entry.store.transaction(false, |tx| {
            imports::validate_inputs(tx, &store, &options).map_err(refused)
        })?;
        imports::preview_import(&request.csv, &options).map_err(refused)
    })
    .await?;
    Ok(axum::Json(value).into_response())
}

fn snapshot_failure(error: SnapshotError) -> Failure {
    match error {
        SnapshotError::NotFound => Failure::new(
            StatusCode::NOT_FOUND,
            "Published snapshot not found",
            "NOT_FOUND",
        ),
        SnapshotError::Refused(message) => refused(message),
        SnapshotError::Store(_) => Failure::internal(),
    }
}

async fn list(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
) -> Result<Response, Failure> {
    let value = blocking(move || {
        entry
            .store
            .transaction(false, |tx| snapshots::list(tx).map_err(snapshot_failure))
    })
    .await?;
    Ok(axum::Json(value).into_response())
}

async fn bars(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(snapshot_id): Path<String>,
) -> Result<Response, Failure> {
    let value = blocking(move || {
        let store = artifacts(&entry, false)?;
        entry.store.transaction(false, |tx| {
            snapshots::bars(tx, &store, &snapshot_id, 1000).map_err(snapshot_failure)
        })
    })
    .await?;
    Ok(axum::Json(value).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::post("/api/v1/imports", submit),
        Operation::post("/api/v1/imports/preview", preview),
        Operation::get("/api/v1/snapshots", list),
        Operation::get("/api/v1/snapshots/{snapshot_id}/bars", bars),
    ]
}

fn trace() -> Value {
    serde_json::to_value(communication::context(None, 600.0).expect("root context"))
        .expect("serializable context")
}

/// Claim and execute one import task; false when none is due.
fn execute_one(entry: &Entry) -> bool {
    let token = uuid::Uuid::new_v4().to_string();
    let claim = Request::Claim {
        worker_id: "entry".into(),
        now: now(),
        lease_seconds: entry.lease_seconds,
        token: token.clone(),
        kinds: Some(ClaimKinds::Only([KIND.to_string()].into_iter().collect())),
    };
    let claimed = entry.store.transaction(true, |tx| {
        entry
            .tasks
            .run(tx.connection(), claim, trace())
            .map_err(|_| Failure::internal())
    });
    let Ok(row) = claimed else { return false };
    let Some(job_id) = row["id"].as_str().map(String::from) else {
        return false;
    };
    let outcome = artifacts(entry, true)
        .map_err(|_| ExecuteError::Refused("数据目录不可用".into()))
        .and_then(|store| {
            let result = entry.store.transaction(true, |tx| {
                imports::execute(tx, &store, &entry.tasks, &job_id, &token, trace(), now())
                    .map_err(Outcome::Failed)
            });
            result.map_err(|error| match error {
                Outcome::Failed(error) => error,
                Outcome::Store => ExecuteError::Lease("数据库不可用".into()),
            })
        });
    if let Err(ExecuteError::Refused(message)) = outcome {
        let fail = Request::Apply {
            id: job_id,
            action: Action::Fail {
                token,
                error: message,
            },
            now: now(),
        };
        let _ = entry.store.transaction(true, |tx| {
            entry
                .tasks
                .run(tx.connection(), fail, trace())
                .map_err(|_| Failure::internal())
        });
    }
    true
}

enum Outcome {
    Failed(ExecuteError),
    Store,
}
impl From<asterion_store::StoreError> for Outcome {
    fn from(_: asterion_store::StoreError) -> Self {
        Self::Store
    }
}

/// Run import tasks as they arrive, and at least every second for retries.
pub(crate) fn start(entry: Arc<Entry>) {
    let Ok(runtime) = tokio::runtime::Handle::try_current() else {
        return;
    };
    runtime.spawn(async move {
        loop {
            let worker = entry.clone();
            while tokio::task::spawn_blocking({
                let worker = worker.clone();
                move || execute_one(&worker)
            })
            .await
            .unwrap_or(false)
            {}
            tokio::select! {
                _ = entry.imports.notified() => {}
                _ = tokio::time::sleep(Duration::from_secs(1)) => {}
            }
        }
    });
}
