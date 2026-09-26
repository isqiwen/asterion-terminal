//! Data source connections and their configurations, served from the data
//! service library. Probes reach the source outside any database transaction.
use crate::access::Unlocked;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_data::configuration::{self, ConfigurationUpdate, SourceError};
use asterion_data::connections::{self, ConnectionUpdate, NewConnection};
use asterion_data::credentials::Credentials;
use asterion_data::providers;
use asterion_data_store::provider::ProviderManifest;
use axum::{
    body::Bytes,
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use serde::Serialize;
use serde_json::{Map, Value};
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

fn credentials(entry: &Entry) -> Credentials {
    Credentials::new(&entry.secret, &entry.data_root)
}

fn sources() -> Vec<ProviderManifest> {
    providers::manifests()
}

/// Run `work` in one transaction on the blocking pool.
async fn run<T: Send + 'static>(
    entry: Arc<Entry>,
    write: bool,
    work: impl FnOnce(&Entry, &asterion_store::Transaction) -> Result<T, SourceError> + Send + 'static,
) -> Result<T, Failure> {
    tokio::task::spawn_blocking(move || {
        entry
            .store
            .transaction(write, |tx| work(&entry, tx).map_err(failure))
    })
    .await
    .map_err(|_| Failure::internal())?
}

/// Ask the connection's built-in source to verify values; network access
/// runs on the blocking pool and outside any transaction.
async fn probe(
    provider: String,
    values: Map<String, Value>,
) -> Result<Result<String, String>, Failure> {
    tokio::task::spawn_blocking(move || {
        providers::get(&provider).and_then(|source| source.probe(&values))
    })
    .await
    .map_err(|_| Failure::internal())
}

fn json(value: impl Serialize) -> Response {
    axum::Json(value).into_response()
}

async fn listing(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
) -> Result<Response, Failure> {
    let items = run(entry, false, |entry, tx| {
        connections::listing(tx, &credentials(entry), &sources())
    })
    .await?;
    Ok(json(items))
}

async fn create(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    body: Bytes,
) -> Result<Response, Failure> {
    let body: NewConnection = crate::input::body(&body, |body: &NewConnection| {
        crate::input::length(&body.name, 1, 80)
    })?;
    let created = run(entry, true, move |_, tx| {
        connections::create(tx, &sources(), &body)
    })
    .await?;
    Ok((StatusCode::CREATED, axum::Json(created)).into_response())
}

async fn update(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(identifier): Path<String>,
    body: Bytes,
) -> Result<Response, Failure> {
    let body: ConnectionUpdate = crate::input::body(&body, |body: &ConnectionUpdate| {
        body.valid().then_some(()).ok_or(crate::input::Invalid)
    })?;
    let state = run(entry, true, move |_, tx| {
        connections::update(tx, &sources(), &identifier, &body)
    })
    .await?;
    Ok(json(state))
}

fn update_body(content: &Bytes) -> Result<ConfigurationUpdate, Failure> {
    crate::input::body(content, |body: &ConfigurationUpdate| {
        body.valid().then_some(()).ok_or(crate::input::Invalid)
    })
}

async fn configuration(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(owner): Path<String>,
) -> Result<Response, Failure> {
    let state = run(entry, false, move |entry, tx| {
        let spec = connections::spec(tx, &sources(), &owner)?;
        configuration::state(tx, &credentials(entry), &owner, &spec)
    })
    .await?;
    Ok(json(state))
}

async fn apply(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(owner): Path<String>,
    body: Bytes,
) -> Result<Response, Failure> {
    let update = update_body(&body)?;
    let state = run(entry, true, move |entry, tx| {
        let spec = connections::spec(tx, &sources(), &owner)?;
        configuration::apply(tx, &credentials(entry), &owner, &spec, &update)
    })
    .await?;
    Ok(json(state))
}

/// Probe unsaved values; saved state and verification records are unchanged.
async fn check(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(owner): Path<String>,
    body: Bytes,
) -> Result<Response, Failure> {
    let update = update_body(&body)?;
    let expected = update.expected_revision;
    let key = owner.clone();
    let (provider, values) = run(entry.clone(), false, move |entry, tx| {
        let sources = sources();
        let provider = connections::resolve(tx, &sources, &key)?.provider;
        let spec = connections::spec(tx, &sources, &key)?;
        let values = configuration::draft(tx, &credentials(entry), &key, &spec, &update)?;
        Ok((provider, values))
    })
    .await?;
    let message = probe(provider, values)
        .await?
        .map_err(|message| failure(message.into()))?;
    let checked = run(entry, false, move |_, tx| {
        configuration::checked(tx, &owner, expected, message)
    })
    .await?;
    Ok(json(checked))
}

/// Probe the saved configuration and record the result for its revision.
async fn verify(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(owner): Path<String>,
) -> Result<Response, Failure> {
    let key = owner.clone();
    let (provider, revision, values) = run(entry.clone(), false, move |entry, tx| {
        let sources = sources();
        let lifecycle = connections::state(tx, &sources, &key)?;
        if lifecycle.state != "enabled" {
            return Err("连接已停用或归档，请先恢复连接".into());
        }
        let spec = connections::spec(tx, &sources, &key)?;
        let (revision, values) = configuration::current(tx, &credentials(entry), &key, &spec)?;
        let values = configuration::validate(&spec, &values, true)?;
        Ok((lifecycle.provider, revision, values))
    })
    .await?;
    let started = now();
    let verified = probe(provider, values).await?.is_ok();
    let checked = now();
    let state = run(entry, true, move |_, tx| {
        configuration::record_verification(tx, &owner, revision, started, checked, verified)?;
        configuration::verification(tx, &owner)
    })
    .await?;
    Ok(json(state))
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::get("/api/v1/data/providers", listing),
        Operation::post("/api/v1/data/connections", create),
        Operation::post("/api/v1/data/connections/{identifier}", update),
        Operation::post("/api/v1/data/providers/{provider}/verify", verify),
        Operation::get(
            "/api/v1/data/providers/{provider}/configuration",
            configuration,
        ),
        Operation::post("/api/v1/data/providers/{provider}/configuration", apply),
        Operation::post(
            "/api/v1/data/providers/{provider}/configuration/check",
            check,
        ),
    ]
}
