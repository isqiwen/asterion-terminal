//! Replay of committed events by topic. Reads are authorized against the
//! topic's declared read path (see `Entry::principal`); ordering, cursors and
//! page validation are the fixed kernel journal's.
use crate::access::Unlocked;
use crate::{Authorized, Entry, Failure, Operation};
use asterion_kernel::database::OperationError;
use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::{IntoResponse, Response},
};
use std::collections::HashMap;
use std::sync::Arc;

fn invalid(message: &str) -> Failure {
    Failure::new(StatusCode::UNPROCESSABLE_ENTITY, message, "INVALID_INPUT")
}

async fn page(
    State(entry): State<Arc<Entry>>,
    _: Authorized,
    _: Unlocked,
    Query(query): Query<HashMap<String, String>>,
) -> Result<Response, Failure> {
    let topic = query.get("topic").ok_or_else(Failure::invalid)?;
    let after = query.get("after").map_or("0", String::as_str);
    let limit = match query.get("limit") {
        Some(limit) => limit.parse::<i64>().map_err(|_| Failure::invalid())?,
        None => 100,
    };
    let plan = entry
        .journal
        .registry()
        .read_plan(topic, after, limit)
        .map_err(|message| invalid(&message))?;
    let journal = entry.clone();
    let page = tokio::task::spawn_blocking(move || {
        journal.store.transaction(false, |tx| {
            journal
                .journal
                .read(tx.connection(), plan)
                .map_err(|error| match error {
                    OperationError::Contract(message) => invalid(&message),
                    OperationError::Database(_) => Failure::internal(),
                })
        })
    })
    .await
    .map_err(|_| Failure::internal())??;
    Ok(axum::Json(page).into_response())
}

pub(crate) fn operations() -> Vec<Operation> {
    vec![Operation::get("/api/v1/communication/events", page)]
}
