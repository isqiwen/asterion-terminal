//! Catalogue of published data versions, served from the data service library.
use crate::access::Unlocked;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_data::catalog::{self, CatalogError, CatalogQuery};
use asterion_data::lifecycle::{self, ArchiveRequest, LifecycleError};
use asterion_data::preview::{PreviewError, preview};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_store::Transaction;
use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::Arc;

type Parameters = Query<HashMap<String, String>>;

fn text(query: &HashMap<String, String>, name: &str, max: usize) -> Result<String, Failure> {
    let value = query.get(name).cloned().unwrap_or_default();
    if value.chars().count() > max {
        return Err(Failure::invalid());
    }
    Ok(value)
}

fn number(
    query: &HashMap<String, String>,
    name: &str,
    default: i64,
    range: std::ops::RangeInclusive<i64>,
) -> Result<i64, Failure> {
    let value = match query.get(name) {
        Some(value) => value.parse().map_err(|_| Failure::invalid())?,
        None => default,
    };
    range
        .contains(&value)
        .then_some(value)
        .ok_or_else(Failure::invalid)
}

fn flag(query: &HashMap<String, String>, name: &str) -> Result<bool, Failure> {
    match query.get(name).map(|value| value.to_ascii_lowercase()) {
        None => Ok(false),
        Some(value) => match value.as_str() {
            "true" | "1" | "yes" | "on" => Ok(true),
            "false" | "0" | "no" | "off" => Ok(false),
            _ => Err(Failure::invalid()),
        },
    }
}

async fn read(
    entry: Arc<Entry>,
    work: impl FnOnce(&Transaction) -> Result<Value, CatalogError> + Send + 'static,
) -> Result<Response, Failure> {
    let value = tokio::task::spawn_blocking(move || {
        entry.store.transaction(false, |tx| {
            work(tx).map_err(|error| match error {
                CatalogError::Invalid(message) => {
                    Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
                }
                CatalogError::Store(_) => Failure::internal(),
            })
        })
    })
    .await
    .map_err(|_| Failure::internal())??;
    Ok(axum::Json(value).into_response())
}

async fn listing(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Query(query): Parameters,
) -> Result<Response, Failure> {
    let request = CatalogQuery {
        directory: text(&query, "directory", 1000)?,
        include_archived: flag(&query, "include_archived")?,
        domain: text(&query, "domain", usize::MAX)?,
        type_id: text(&query, "type_id", usize::MAX)?,
        source: text(&query, "source", usize::MAX)?,
        layer: text(&query, "layer", usize::MAX)?,
        search: text(&query, "search", 200)?,
        offset: number(&query, "offset", 0, 0..=i64::MAX)?,
        limit: number(&query, "limit", 50, 1..=100)?,
    };
    read(entry, move |tx| catalog::list(tx, &request)).await
}

async fn history(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(dataset_id): Path<String>,
    Query(query): Parameters,
) -> Result<Response, Failure> {
    let offset = number(&query, "offset", 0, 0..=i64::MAX)?;
    let limit = number(&query, "limit", 50, 1..=100)?;
    read(entry, move |tx| {
        catalog::history(tx, &dataset_id, offset, limit)
    })
    .await
}

async fn hierarchy(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Query(query): Parameters,
) -> Result<Response, Failure> {
    let include_archived = flag(&query, "include_archived")?;
    read(entry, move |tx| {
        Ok(serde_json::to_value(catalog::hierarchy(tx, include_archived)?).expect("serializable"))
    })
    .await
}

/// Checksum-verified rows of one published version.
async fn version(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(version_id): Path<String>,
    Query(query): Parameters,
) -> Result<Response, Failure> {
    let offset = number(&query, "offset", 0, 0..=i64::MAX)? as usize;
    let limit = number(&query, "limit", 100, 1..=500)? as usize;
    let value = tokio::task::spawn_blocking(move || {
        let store =
            ArtifactStore::reader(&entry.data_root, 1 << 30).map_err(|_| Failure::internal())?;
        entry.store.transaction(false, |tx| {
            // Serialized as is, so rows keep their stored column order.
            preview(tx, &store, &version_id, offset, limit).map_err(|error| match error {
                PreviewError::NotFound => {
                    Failure::new(StatusCode::NOT_FOUND, "数据版本不存在", "NOT_FOUND")
                }
                PreviewError::Refused(message) => {
                    Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
                }
                PreviewError::Store(_) => Failure::internal(),
            })
        })
    })
    .await
    .map_err(|_| Failure::internal())??;
    Ok(axum::Json(value).into_response())
}

fn refused(error: LifecycleError) -> Failure {
    match error {
        LifecycleError::NotFound => {
            Failure::new(StatusCode::NOT_FOUND, "数据版本不存在", "NOT_FOUND")
        }
        LifecycleError::Conflict(message) => {
            Failure::new(StatusCode::CONFLICT, &message, "CONFLICT")
        }
        LifecycleError::Refused(message) => {
            Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT")
        }
        LifecycleError::Store(_) => Failure::internal(),
    }
}

async fn lifecycle_state(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(version_id): Path<String>,
) -> Result<Response, Failure> {
    let value = tokio::task::spawn_blocking(move || {
        entry.store.transaction(false, |tx| {
            lifecycle::inspect(tx, &version_id).map_err(refused)
        })
    })
    .await
    .map_err(|_| Failure::internal())??;
    Ok(axum::Json(value).into_response())
}

/// Archive or restore a version; nothing is ever deleted.
async fn archive(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Path(version_id): Path<String>,
    content: axum::body::Bytes,
) -> Result<Response, Failure> {
    let request = crate::input::body::<ArchiveRequest>(&content, |r| {
        if r.expected_revision >= 0 {
            Ok(())
        } else {
            Err(crate::input::Invalid)
        }
    })?;
    let value = tokio::task::spawn_blocking(move || {
        entry.store.transaction(true, |tx| {
            lifecycle::archive(tx, &version_id, &request).map_err(refused)
        })
    })
    .await
    .map_err(|_| Failure::internal())??;
    Ok(axum::Json(value).into_response())
}

/// The built-in data types of the data store.
async fn types(_: Authorized, _: Unlocked) -> Response {
    axum::Json(asterion_data_store::types::manifests()).into_response()
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![
        Operation::get("/api/v1/data/catalog", listing),
        Operation::get("/api/v1/data/catalog/{dataset_id}/versions", history),
        Operation::get("/api/v1/data/hierarchy", hierarchy),
        Operation::get("/api/v1/data/types", types),
        Operation::get("/api/v1/data/versions/{version_id}", version),
        Operation::get(
            "/api/v1/data/versions/{version_id}/lifecycle",
            lifecycle_state,
        ),
        Operation::post("/api/v1/data/versions/{version_id}/archive", archive),
    ]
}
