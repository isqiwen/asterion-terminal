//! Task queue operations: listing for the workbench, and claim, lease renewal
//! and failure for workers. Persistence and every transition are the fixed
//! kernel task repository's; this module only binds HTTP to it.
use crate::access::Unlocked;
use crate::context::Trace;
use crate::input::{body, length};
use crate::{Authorized, Entry, Failure, Operation};
use asterion_kernel::communication;
use asterion_kernel::database::OperationError;
use asterion_kernel::tasks::{Action, repository::Request};
use axum::{
    Extension,
    body::Bytes,
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use serde::Deserialize;
use serde_json::{Map, Value, json};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

/// Fields of the public task description, in contract order.
const JOB: [&str; 10] = [
    "id",
    "command_id",
    "kind",
    "state",
    "attempt",
    "created_at",
    "worker_id",
    "lease_until",
    "error",
    "result",
];
/// A claimed task's communication budget, independent of the claim request.
const CONTINUATION_SECONDS: f64 = 86400.0;

pub(crate) fn job(row: &Value) -> Map<String, Value> {
    JOB.iter()
        .map(|key| ((*key).to_string(), row[*key].clone()))
        .collect()
}

fn now() -> Result<f64, Failure> {
    Ok(SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| Failure::internal())?
        .as_secs_f64())
}

/// Task contract refusals are conflicts, as in the rest of the API.
fn refused(error: OperationError) -> Failure {
    match error {
        OperationError::Contract(message) => {
            Failure::new(StatusCode::CONFLICT, &message, "CONFLICT")
        }
        OperationError::Database(_) => Failure::internal(),
    }
}

/// Run one repository request in its own transaction on the blocking pool.
async fn run(
    entry: &Arc<Entry>,
    write: bool,
    request: Request,
    trace: Value,
) -> Result<Value, Failure> {
    let entry = entry.clone();
    tokio::task::spawn_blocking(move || {
        entry.store.transaction(write, |tx| {
            entry
                .tasks
                .run(tx.connection(), request, trace)
                .map_err(refused)
        })
    })
    .await
    .map_err(|_| Failure::internal())?
}

async fn list(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Extension(Trace(trace)): Extension<Trace>,
) -> Result<Response, Failure> {
    let rows = run(&entry, false, Request::List, trace).await?;
    let jobs: Vec<Value> = rows
        .as_array()
        .ok_or_else(Failure::internal)?
        .iter()
        .map(|row| Value::Object(job(row)))
        .collect();
    Ok(axum::Json(jobs).into_response())
}

async fn get(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Extension(Trace(trace)): Extension<Trace>,
    Path(id): Path<String>,
) -> Result<Response, Failure> {
    let row = run(&entry, false, Request::Get { id }, trace).await?;
    if row.is_null() {
        return Err(Failure::new(
            StatusCode::NOT_FOUND,
            "任务不存在",
            "NOT_FOUND",
        ));
    }
    Ok(axum::Json(job(&row)).into_response())
}

#[derive(Deserialize)]
struct Claim {
    worker_id: String,
}

async fn claim(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    content: Bytes,
) -> Result<Response, Failure> {
    let request = body::<Claim>(&content, |b| length(&b.worker_id, 1, 100))?;
    let row = run(
        &entry,
        true,
        Request::Claim {
            worker_id: request.worker_id,
            now: now()?,
            lease_seconds: entry.lease_seconds,
            token: uuid::Uuid::new_v4().to_string(),
            // The entry executes its own kinds; the internal worker the rest.
            kinds: Some(asterion_kernel::tasks::ClaimKinds::Except(
                crate::native_kinds(),
            )),
        },
        trace,
    )
    .await?;
    if row.is_null() {
        return Ok(axum::Json(Value::Null).into_response());
    }
    // The claim's committed event starts the worker's own causal budget.
    let event = &row["communication_event"];
    let continuation = communication::continuation(
        event["correlation_id"]
            .as_str()
            .ok_or_else(Failure::internal)?,
        event["id"].as_str().ok_or_else(Failure::internal)?,
        CONTINUATION_SECONDS,
    )
    .map_err(|_| Failure::internal())?;
    let mut claimed = job(&row);
    claimed.insert("communication".into(), json!(continuation));
    claimed.insert("payload".into(), row["payload"].clone());
    claimed.insert("token".into(), row["token"].clone());
    Ok(axum::Json(claimed).into_response())
}

#[derive(Deserialize)]
struct Lease {
    token: String,
}

#[derive(Deserialize)]
struct Failed {
    token: String,
    error: String,
}

async fn apply(
    entry: &Arc<Entry>,
    id: String,
    action: Action,
    trace: Value,
) -> Result<(), Failure> {
    let now = now()?;
    run(entry, true, Request::Apply { id, action, now }, trace)
        .await
        .map(drop)
}

async fn heartbeat(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    Path(id): Path<String>,
    content: Bytes,
) -> Result<Response, Failure> {
    let lease = body::<Lease>(&content, |_| Ok(()))?;
    let action = Action::Heartbeat {
        token: lease.token,
        lease_seconds: entry.lease_seconds,
    };
    apply(&entry, id, action, trace).await?;
    Ok(axum::Json(json!({"status": "renewed"})).into_response())
}

async fn fail(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    Extension(Trace(trace)): Extension<Trace>,
    Path(id): Path<String>,
    content: Bytes,
) -> Result<Response, Failure> {
    let failed = body::<Failed>(&content, |_| Ok(()))?;
    let action = Action::Fail {
        token: failed.token,
        error: failed.error,
    };
    apply(&entry, id, action, trace).await?;
    Ok(axum::Json(json!({"status": "failed"})).into_response())
}

async fn cancel(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Extension(Trace(trace)): Extension<Trace>,
    Path(id): Path<String>,
) -> Result<Response, Failure> {
    let request = Request::CancelBatch {
        ids: [id].into_iter().collect(),
    };
    if run(&entry, true, request, trace).await? != json!(1) {
        return Err(Failure::new(
            StatusCode::CONFLICT,
            "Job is missing or already terminal",
            "CONFLICT",
        ));
    }
    Ok(axum::Json(json!({"status": "cancelled"})).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::get("/api/v1/jobs", list),
        Operation::get("/api/v1/jobs/{job_id}", get),
        Operation::post("/api/v1/jobs/claim", claim),
        Operation::post("/api/v1/jobs/{job_id}/heartbeat", heartbeat),
        Operation::post("/api/v1/jobs/{job_id}/fail", fail),
        Operation::post("/api/v1/jobs/{job_id}/cancel", cancel),
    ]
}
