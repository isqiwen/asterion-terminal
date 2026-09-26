//! Sync tasks are collected here, by the entry itself: it claims only sync
//! tasks, collects them from the built-in source under their lease, and hands
//! the evidence to the internal process, which still owns publication.
use crate::{ACCOUNT_STATUS, Entry, FORWARDED, PRINCIPAL};
use asterion_data::collect::{self, CollectError, Host};
use asterion_data::credentials::Credentials;
use asterion_data::providers;
use asterion_data::sync::KIND;
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::communication;
use asterion_kernel::tasks::{ClaimKinds, repository::Request};
use axum::body::Body;
use axum::http::{HeaderName, HeaderValue, Method, StatusCode, Uri, header};
use http_body_util::BodyExt;
use serde_json::Value;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

fn trace() -> Value {
    serde_json::to_value(communication::context(None, 600.0).expect("root context"))
        .expect("serializable context")
}

/// A claimed sync task and its lease token.
struct Claimed {
    job: Value,
    token: String,
}

fn claim(entry: &Entry) -> Option<Claimed> {
    let token = uuid::Uuid::new_v4().to_string();
    let request = Request::Claim {
        worker_id: "entry-collector".into(),
        now: now(),
        lease_seconds: entry.lease_seconds,
        token: token.clone(),
        kinds: Some(ClaimKinds::Only([KIND.to_string()].into_iter().collect())),
    };
    let row = entry
        .store
        .transaction(true, |tx| {
            entry
                .tasks
                .run(tx.connection(), request, trace())
                .map_err(|error| asterion_store::StoreError::from(error.to_string()))
        })
        .ok()?;
    row["id"].is_string().then_some(Claimed { job: row, token })
}

/// Collect a claimed task; its evidence, or `None` once it failed or its
/// lease was lost.
fn collected(entry: &Entry, claimed: &Claimed) -> Option<Vec<u8>> {
    let job_id = claimed.job["id"].as_str().unwrap_or_default();
    let trace = trace();
    let artifacts = ArtifactStore::new(&entry.data_root, 1 << 30).ok()?;
    let credentials = Credentials::new(&entry.secret, &entry.data_root);
    let host = Host {
        store: &entry.store,
        artifacts: &artifacts,
        tasks: &entry.tasks,
        credentials: &credentials,
        lease_seconds: entry.lease_seconds,
        trace: &trace,
    };
    let provider = claimed.job["payload"]["request"]["provider"]
        .as_str()
        .ok_or_else(|| "同步请求格式不受支持".to_string())
        .and_then(providers::get);
    let outcome = provider.map_err(CollectError::Failed).and_then(|provider| {
        collect::collect(&host, provider.as_ref(), &claimed.job, &claimed.token)
    });
    match outcome {
        Ok(content) => Some(content),
        Err(CollectError::Failed(reason)) => {
            host.fail(job_id, &claimed.token, reason);
            None
        }
        Err(CollectError::Lease(_)) => None,
    }
}

/// Hand the evidence to the internal process's publication, as a worker.
async fn publish(entry: &Entry, claimed: &Claimed, content: Vec<u8>) -> Result<(), String> {
    let job_id = claimed.job["id"].as_str().unwrap_or_default();
    let path = format!("/api/v1/jobs/{job_id}/publish-data");
    let uri = Uri::builder()
        .scheme(entry.base.scheme().cloned().ok_or("后台服务地址无效")?)
        .authority(entry.base.authority().cloned().ok_or("后台服务地址无效")?)
        .path_and_query(path)
        .build()
        .map_err(|_| "后台服务地址无效")?;
    let context = HeaderValue::from_str(&trace().to_string()).map_err(|_| "通信上下文无效")?;
    let request = axum::http::Request::builder()
        .method(Method::POST)
        .uri(uri)
        .header(header::CONTENT_TYPE, "application/octet-stream")
        .header("x-lease-token", claimed.token.as_str())
        .header(HeaderName::from_static(FORWARDED), entry.forwarding.clone())
        .header(HeaderName::from_static(PRINCIPAL), "worker")
        .header(HeaderName::from_static(ACCOUNT_STATUS), "expired")
        .header(crate::context::HEADER, context)
        .body(Body::from(content))
        .map_err(|_| "发布请求无效")?;
    let response = entry
        .client
        .request(request)
        .await
        .map_err(|_| "后台服务暂不可用，采集结果未发布")?;
    let status = response.status();
    if status.is_success() || status == StatusCode::CONFLICT {
        // A conflict means the lease moved on; the new holder owns the task.
        return Ok(());
    }
    let body = response
        .into_body()
        .collect()
        .await
        .map(|body| body.to_bytes())
        .unwrap_or_default();
    let detail = serde_json::from_slice::<Value>(&body)
        .ok()
        .and_then(|value| value["detail"].as_str().map(str::to_string));
    Err(match (status, detail) {
        (StatusCode::UNPROCESSABLE_ENTITY, Some(detail)) => detail,
        (StatusCode::UNPROCESSABLE_ENTITY, None) => "数据发布校验失败".into(),
        (status, _) => format!("采集结果发布失败（HTTP {}）", status.as_u16()),
    })
}

/// Collect and publish one sync task; false when none is due.
async fn execute_one(entry: Arc<Entry>) -> bool {
    let worker = entry.clone();
    let Ok(Some(claimed)) = tokio::task::spawn_blocking(move || claim(&worker)).await else {
        return false;
    };
    let claimed = Arc::new(claimed);
    let (worker, task) = (entry.clone(), claimed.clone());
    let content = tokio::task::spawn_blocking(move || collected(&worker, &task))
        .await
        .ok()
        .flatten();
    if let Some(content) = content
        && let Err(reason) = publish(&entry, &claimed, content).await
    {
        let (worker, task) = (entry.clone(), claimed.clone());
        let _ = tokio::task::spawn_blocking(move || {
            let trace = trace();
            let artifacts = ArtifactStore::reader(&worker.data_root, 1 << 30);
            let credentials = Credentials::new(&worker.secret, &worker.data_root);
            if let Ok(artifacts) = artifacts {
                Host {
                    store: &worker.store,
                    artifacts: &artifacts,
                    tasks: &worker.tasks,
                    credentials: &credentials,
                    lease_seconds: worker.lease_seconds,
                    trace: &trace,
                }
                .fail(
                    task.job["id"].as_str().unwrap_or_default(),
                    &task.token,
                    reason,
                );
            }
        })
        .await;
    }
    true
}

/// Collect sync tasks as they are queued, one at a time.
pub(crate) fn start(entry: Arc<Entry>) {
    let Ok(runtime) = tokio::runtime::Handle::try_current() else {
        return;
    };
    runtime.spawn(async move {
        loop {
            while execute_one(entry.clone()).await {}
            tokio::select! {
                _ = entry.syncs.notified() => {}
                _ = tokio::time::sleep(Duration::from_secs(1)) => {}
            }
        }
    });
}
