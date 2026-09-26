//! The entry collects queued sync tasks from the built-in source and hands the
//! evidence to the internal process's publication as a worker. The vendor and
//! the internal process are local stand-ins.
mod support;

use asterion_kernel::authority;
use axum::body::{Body, Bytes};
use axum::http::{HeaderMap, Method, StatusCode};
use axum::routing::post;
use serde_json::{Value, json};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use support::{SECRET, send, serve, services};

/// Tushare's calendar interface: one row per requested day.
async fn vendor(body: Bytes) -> axum::Json<Value> {
    let request: Value = serde_json::from_slice(&body).unwrap();
    let day = |key: &str| {
        chrono::NaiveDate::parse_from_str(request["params"][key].as_str().unwrap(), "%Y%m%d")
            .unwrap()
    };
    let (mut cursor, end) = (day("start_date"), day("end_date"));
    let mut items = Vec::new();
    while cursor <= end {
        items.push(json!([
            "SHFE",
            cursor.format("%Y%m%d").to_string(),
            1,
            null
        ]));
        cursor = cursor.succ_opt().unwrap();
    }
    axum::Json(json!({"code": 0, "data": {
        "fields": ["exchange", "cal_date", "is_open", "pretrade_date"], "items": items}}))
}

#[derive(Default)]
struct Publications {
    /// Status the stand-in publication answers with.
    answer: Option<(StatusCode, Value)>,
    received: Vec<(String, HeaderMap, Bytes)>,
}

async fn call(entry: &str, method: Method, path: &str, body: Option<Value>) -> (StatusCode, Value) {
    let request = axum::http::Request::builder()
        .method(method)
        .uri(format!("{entry}{path}"))
        .header("authorization", format!("Bearer {SECRET}"))
        .header("content-type", "application/json")
        .body(body.map_or_else(Body::empty, |b| Body::from(b.to_string())))
        .unwrap();
    let (status, _, content) = send(request).await;
    (
        status,
        serde_json::from_slice(&content).unwrap_or(Value::Null),
    )
}

async fn until<T>(what: &str, mut check: impl FnMut() -> Option<T>) -> T {
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if let Some(value) = check() {
            return value;
        }
        assert!(Instant::now() < deadline, "timed out waiting for {what}");
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

#[tokio::test]
async fn queued_sync_tasks_are_collected_and_handed_to_publication() {
    let vendor = serve(axum::Router::new().route("/", post(vendor))).await;
    asterion_provider_tushare::set_test_endpoint(&vendor);
    let publications = Arc::new(Mutex::new(Publications::default()));
    let record = publications.clone();
    let internal = serve(axum::Router::new().route(
        "/api/v1/jobs/{job_id}/publish-data",
        post(
            move |axum::extract::Path(job_id): axum::extract::Path<String>,
                  headers: HeaderMap,
                  body: Bytes| {
                let record = record.clone();
                async move {
                    let mut state = record.lock().unwrap();
                    state.received.push((job_id, headers, body));
                    let (status, detail) = state
                        .answer
                        .clone()
                        .unwrap_or((StatusCode::OK, json!({"id": "version"})));
                    (status, axum::Json(detail))
                }
            },
        ),
    ))
    .await;
    let services = services();
    let entry = serve(support::app(&internal, "[]", false, &services)).await;
    let configuration = "/api/v1/data/providers/tushare/configuration";
    let body = json!({"expected_revision": 0, "secrets": {"token": "vendor-token"}});
    assert_eq!(
        call(&entry, Method::POST, configuration, Some(body))
            .await
            .0,
        StatusCode::OK
    );

    let submit = |command: &str| {
        json!({"command_id": command, "provider": "tushare", "dataset": "calendar",
               "exchange": "SHFE", "start": "2024-01-01", "end": "2024-01-05"})
    };
    let (status, job) = call(
        &entry,
        Method::POST,
        "/api/v1/data/sync",
        Some(submit("first")),
    )
    .await;
    assert_eq!(status, StatusCode::ACCEPTED);
    let job_id = job["id"].as_str().unwrap().to_string();
    let (published, headers, content) = until("publication", || {
        publications.lock().unwrap().received.first().cloned()
    })
    .await;
    assert_eq!(published, job_id);
    let header = |name: &str| headers.get(name).unwrap().to_str().unwrap().to_string();
    assert_eq!(
        header("x-asterion-forwarded"),
        authority::forwarding_token(SECRET)
    );
    assert_eq!(header("x-asterion-principal"), "worker");
    assert!(!header("x-lease-token").is_empty());
    let evidence: Vec<Value> = serde_json::from_slice(&content).unwrap();
    assert_eq!(evidence.len(), 1);
    assert_eq!(evidence[0]["rows"].as_array().unwrap().len(), 5);
    let observations = format!("/api/v1/data/jobs/{job_id}/observations");
    let (_, page) = call(&entry, Method::GET, &observations, None).await;
    assert_eq!(page["total"], 1);
    assert_eq!(page["items"][0]["manifest"]["status"], "RECEIVED");
    let (_, state) = call(&entry, Method::GET, &format!("/api/v1/jobs/{job_id}"), None).await;
    assert_eq!(state["result"], json!({"completed": 1, "total": 1}));

    // A refused publication fails the task with the publisher's reason.
    publications.lock().unwrap().answer = Some((
        StatusCode::UNPROCESSABLE_ENTITY,
        json!({"detail": "交易日历缺少日期，未发布", "code": "INVALID_INPUT"}),
    ));
    let (_, job) = call(
        &entry,
        Method::POST,
        "/api/v1/data/sync",
        Some(submit("second")),
    )
    .await;
    let path = format!("/api/v1/jobs/{}", job["id"].as_str().unwrap());
    let mut failed = Value::Null;
    for _ in 0..300 {
        let (_, state) = call(&entry, Method::GET, &path, None).await;
        if state["state"] == "FAILED" {
            failed = state;
            break;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    assert_eq!(failed["error"], "交易日历缺少日期，未发布");
    assert_eq!(publications.lock().unwrap().received.len(), 2);
}
