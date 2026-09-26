mod support;

use asterion_kernel::authority::{self, IssueRequest};
use asterion_kernel::communication;
use asterion_kernel::events::Topic;
use asterion_kernel::tasks::repository::Repository;
use asterion_server::{Authorization, StoreError, declared_operations};
use axum::body::Body;
use axum::http::{HeaderMap, Method, StatusCode};
use serde_json::{Value, json};
use std::time::{SystemTime, UNIX_EPOCH};
use support::{RULES, SECRET, Services, conforms, response, send, serve, services};

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs_f64()
}

fn trace() -> Value {
    serde_json::to_value(communication::context(None, 30.0).unwrap()).unwrap()
}

/// Queue tasks the way the internal process does, through the kernel repository.
fn submit(services: &Services, commands: &[(&str, &str)]) -> Vec<String> {
    let topic = Topic {
        id: "runtime.task.changed".into(),
        owner: "asterion.runtime".into(),
        payload: "fixture".into(),
        read_path: "/jobs".into(),
    };
    let repository = Repository::new(services.store.database_id(), topic).unwrap();
    commands
        .iter()
        .map(|(command, kind)| {
            let request = serde_json::from_value(json!({"op": "submit", "record": {
                "id": format!("job-{command}"), "command_id": command, "kind": kind,
                "payload": {"command": command}, "now": now(),
            }}))
            .unwrap();
            services
                .store
                .transaction(true, |tx| {
                    repository
                        .run(tx.connection(), request, trace())
                        .map_err(|error| StoreError::from(error.to_string()))
                })
                .unwrap()["id"]
                .as_str()
                .unwrap()
                .to_string()
        })
        .collect()
}

struct Client {
    entry: String,
    credential: String,
    session: String,
}
impl Client {
    async fn call(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
    ) -> (StatusCode, HeaderMap, Value) {
        let request = axum::http::Request::builder()
            .method(method)
            .uri(format!("{}{path}", self.entry))
            .header("authorization", format!("Bearer {}", self.credential))
            .header("x-account-session", &self.session)
            .header("content-type", "application/json")
            .body(body.map_or_else(Body::empty, |b| Body::from(b.to_string())))
            .unwrap();
        let (status, headers, content) = send(request).await;
        (
            status,
            headers,
            serde_json::from_slice(&content).unwrap_or(Value::Null),
        )
    }
    /// Call a described operation and check a successful result against it.
    async fn described(
        &self,
        method: Method,
        template: &str,
        path: &str,
        body: Option<Value>,
    ) -> Value {
        let (status, _, value) = self.call(method.clone(), path, body).await;
        assert!(status.is_success(), "{path}: {status} {value}");
        let schema = response(&method.as_str().to_lowercase(), template);
        conforms(&schema, &value).unwrap_or_else(|why| panic!("{path}: {why}"));
        value
    }
}

async fn start(require_account: bool) -> (Services, String) {
    // Sync tasks queued here are collected by the entry: never from the network.
    asterion_provider_tushare::set_test_endpoint("http://127.0.0.1:9");
    let services = services();
    let entry = serve(support::app(
        "http://127.0.0.1:9",
        "[]",
        require_account,
        &services,
    ))
    .await;
    (services, entry)
}

#[tokio::test]
async fn every_described_operation_is_served_natively() {
    let (_, entry) = start(false).await;
    let client = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    for (method, path) in declared_operations().unwrap() {
        let path = path.replace("{job_id}", "missing");
        let method = Method::from_bytes(method.as_bytes()).unwrap();
        let body = (method == Method::POST).then(|| json!({}));
        let (status, headers, value) = client.call(method, &path, body).await;
        // Only native operations answer with their communication context;
        // nothing reaches the (absent) internal process.
        assert!(
            headers.contains_key("x-asterion-context"),
            "{path}: {status} {value}"
        );
        assert_ne!(status, StatusCode::BAD_GATEWAY, "{path}");
    }
}

#[tokio::test]
async fn workers_claim_renew_and_fail_tasks_the_workbench_lists_and_cancels() {
    let (services, entry) = start(false).await;
    let ids = submit(
        &services,
        &[
            ("first", "contract_roles.continue"),
            ("second", "research.backtest"),
        ],
    );
    let root = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let worker = Client {
        entry,
        credential: authority::worker_token(SECRET),
        session: String::new(),
    };

    let claimed = worker
        .described(
            Method::POST,
            "/api/v1/jobs/claim",
            "/api/v1/jobs/claim",
            Some(json!({"worker_id": "worker-1"})),
        )
        .await;
    assert_eq!(claimed["id"], ids[0].as_str());
    assert_eq!(claimed["state"], "RUNNING");
    assert_eq!(claimed["payload"], json!({"command": "first"}));
    assert_eq!(claimed["worker_id"], "worker-1");
    communication::parse_context(claimed["communication"].clone()).unwrap();
    let token = claimed["token"].as_str().unwrap().to_string();
    let job = format!("/api/v1/jobs/{}", ids[0]);

    let renewed = worker
        .described(
            Method::POST,
            "/api/v1/jobs/{job_id}/heartbeat",
            &format!("{job}/heartbeat"),
            Some(json!({"token": token})),
        )
        .await;
    assert_eq!(renewed, json!({"status": "renewed"}));
    let (status, _, refused) = worker
        .call(
            Method::POST,
            &format!("{job}/heartbeat"),
            Some(json!({"token": "stale"})),
        )
        .await;
    assert_eq!(
        (status, refused["code"].as_str()),
        (StatusCode::CONFLICT, Some("CONFLICT"))
    );
    let (status, _, refused) = worker
        .call(
            Method::POST,
            "/api/v1/jobs/claim",
            Some(json!({"worker_id": ""})),
        )
        .await;
    assert_eq!(
        (status, refused["code"].as_str()),
        (StatusCode::UNPROCESSABLE_ENTITY, Some("INVALID_INPUT"))
    );
    // Workers cannot read the queue; the workbench cannot be refused it.
    assert_eq!(
        worker.call(Method::GET, "/api/v1/jobs", None).await.0,
        StatusCode::UNAUTHORIZED
    );

    let jobs = root
        .described(Method::GET, "/api/v1/jobs", "/api/v1/jobs", None)
        .await;
    assert_eq!(jobs.as_array().unwrap().len(), 2);
    assert!(
        jobs.as_array()
            .unwrap()
            .iter()
            .all(|job| job.get("token").is_none())
    );

    let failed = worker
        .described(
            Method::POST,
            "/api/v1/jobs/{job_id}/fail",
            &format!("{job}/fail"),
            Some(json!({"token": token, "error": "source unavailable"})),
        )
        .await;
    assert_eq!(failed, json!({"status": "failed"}));
    let one = root
        .described(Method::GET, "/api/v1/jobs/{job_id}", &job, None)
        .await;
    assert_eq!(
        (one["state"].as_str(), one["error"].as_str()),
        (Some("FAILED"), Some("source unavailable"))
    );
    let (status, _, missing) = root.call(Method::GET, "/api/v1/jobs/missing", None).await;
    assert_eq!(
        (status, missing["code"].as_str()),
        (StatusCode::NOT_FOUND, Some("NOT_FOUND"))
    );

    let second = format!("/api/v1/jobs/{}/cancel", ids[1]);
    let cancelled = root
        .described(Method::POST, "/api/v1/jobs/{job_id}/cancel", &second, None)
        .await;
    assert_eq!(cancelled, json!({"status": "cancelled"}));
    let (status, _, again) = root.call(Method::POST, &second, None).await;
    assert_eq!(
        (status, again["code"].as_str()),
        (StatusCode::CONFLICT, Some("CONFLICT"))
    );
    let nothing = worker
        .described(
            Method::POST,
            "/api/v1/jobs/claim",
            "/api/v1/jobs/claim",
            Some(json!({"worker_id": "worker-1"})),
        )
        .await;
    assert_eq!(nothing, Value::Null);
}

#[tokio::test]
async fn events_replay_by_topic_under_the_topic_read_permission() {
    let (services, entry) = start(false).await;
    submit(
        &services,
        &[
            ("first", "research.backtest"),
            ("second", "research.backtest"),
        ],
    );
    let session = "session-a";
    let now = now();
    let token = authority::issue(&IssueRequest {
        secret: SECRET.into(),
        policies: Authorization::parse(RULES).unwrap().policies,
        scope: "tasks".into(),
        session: session.into(),
        now,
    })
    .unwrap()
    .token;
    let tasks = Client {
        entry,
        credential: token,
        session: session.into(),
    };
    let events = "/api/v1/communication/events";
    let page = tasks
        .described(
            Method::GET,
            events,
            &format!("{events}?topic=runtime.task.changed&limit=1"),
            None,
        )
        .await;
    assert_eq!(page["items"].as_array().unwrap().len(), 1);
    assert_eq!(page["cursor"], "1");
    let rest = tasks
        .described(
            Method::GET,
            events,
            &format!("{events}?topic=runtime.task.changed&after=1"),
            None,
        )
        .await;
    assert_eq!(rest["items"][0]["payload"]["state"], "QUEUED");
    assert_eq!(rest["cursor"], "2");
    let latest = tasks
        .described(
            Method::GET,
            events,
            &format!("{events}?topic=runtime.task.changed&after=latest"),
            None,
        )
        .await;
    assert_eq!(latest, json!({"items": [], "cursor": "2"}));
    for query in ["limit=0", "limit=501", "limit=many", "after=x"] {
        let (status, _, refused) = tasks
            .call(
                Method::GET,
                &format!("{events}?topic=runtime.task.changed&{query}"),
                None,
            )
            .await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some("INVALID_INPUT")),
            "{query}"
        );
    }
    // Other topics need their own read permission; undeclared topics none.
    for topic in ["data.version.published", "missing.topic"] {
        let (status, _, _) = tasks
            .call(Method::GET, &format!("{events}?topic={topic}"), None)
            .await;
        assert_eq!(status, StatusCode::UNAUTHORIZED, "{topic}");
    }
}

async fn signed_in(entry: &str) -> Client {
    let root = Client {
        entry: entry.into(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let user = json!({
        "email": "owner@example.com", "password": "owner-password-123", "pin": "246810",
        "first_name": "Owner", "last_name": "Account"
    });
    root.described(
        Method::POST,
        "/api/v1/account/register",
        "/api/v1/account/register",
        Some(user),
    )
    .await;
    let code = json!({"email": "owner@example.com", "code": "000000"});
    root.described(
        Method::POST,
        "/api/v1/account/verify",
        "/api/v1/account/verify",
        Some(code),
    )
    .await;
    let credentials = json!({"email": "owner@example.com", "password": "owner-password-123"});
    let login = root
        .described(
            Method::POST,
            "/api/v1/account/login",
            "/api/v1/account/login",
            Some(credentials),
        )
        .await;
    Client {
        entry: entry.into(),
        credential: SECRET.into(),
        session: login["session"].as_str().unwrap().into(),
    }
}

#[tokio::test]
async fn scopes_are_issued_to_the_workbench_of_an_unlocked_account() {
    let (_, entry) = start(true).await;
    let anonymous = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    for (method, path) in [
        (Method::POST, "/api/v1/access/scopes"),
        (Method::GET, "/api/v1/jobs"),
        (
            Method::GET,
            "/api/v1/communication/events?topic=runtime.task.changed",
        ),
    ] {
        let (status, _, refused) = anonymous
            .call(method, path, Some(json!({"scope": "tasks"})))
            .await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::UNAUTHORIZED, Some("SESSION_EXPIRED")),
            "{path}"
        );
    }
    let owner = signed_in(&entry).await;
    let scopes = "/api/v1/access/scopes";
    let grant = owner
        .described(
            Method::POST,
            scopes,
            scopes,
            Some(json!({"scope": "tasks"})),
        )
        .await;
    let scoped = Client {
        entry: entry.clone(),
        credential: grant["token"].as_str().unwrap().into(),
        session: owner.session.clone(),
    };
    scoped
        .described(Method::GET, "/api/v1/jobs", "/api/v1/jobs", None)
        .await;
    let other = Client {
        session: "another-session".into(),
        ..scoped
    };
    assert_eq!(
        other.call(Method::GET, "/api/v1/jobs", None).await.0,
        StatusCode::UNAUTHORIZED
    );
    // A scope credential cannot request further scopes.
    let scoped = Client {
        session: owner.session.clone(),
        ..other
    };
    assert_eq!(
        scoped
            .call(Method::POST, scopes, Some(json!({"scope": "tasks"})))
            .await
            .0,
        StatusCode::UNAUTHORIZED
    );
    for scope in [json!("unknown"), json!(""), json!("x".repeat(81)), json!(1)] {
        let (status, _, refused) = owner
            .call(Method::POST, scopes, Some(json!({"scope": scope})))
            .await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some("INVALID_INPUT")),
            "{scope}"
        );
    }
    owner
        .described(
            Method::POST,
            "/api/v1/account/security/lock",
            "/api/v1/account/security/lock",
            None,
        )
        .await;
    for (method, path) in [(Method::POST, scopes), (Method::GET, "/api/v1/jobs")] {
        let (status, _, refused) = owner
            .call(method, path, Some(json!({"scope": "tasks"})))
            .await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::LOCKED, Some("TERMINAL_LOCKED")),
            "{path}"
        );
    }
}

#[tokio::test]
async fn native_operations_carry_the_communication_context() {
    let (_, entry) = start(false).await;
    let call = |context: Option<String>| {
        let entry = entry.clone();
        async move {
            let mut request =
                axum::http::Request::get(format!("{entry}/api/v1/account/capabilities"))
                    .header("authorization", format!("Bearer {SECRET}"));
            if let Some(context) = context {
                request = request.header("x-asterion-context", context);
            }
            let (status, headers, content) = send(request.body(Body::empty()).unwrap()).await;
            let value: Value = serde_json::from_slice(&content).unwrap();
            let context = headers
                .get("x-asterion-context")
                .map(|value| serde_json::from_slice::<Value>(value.as_bytes()).unwrap());
            (status, context, value)
        }
    };
    let (status, context, _) = call(None).await;
    assert_eq!(status, StatusCode::OK);
    let root = communication::parse_context(context.unwrap()).unwrap();
    assert_eq!(root.correlation_id, root.request_id);

    let parent = serde_json::to_value(communication::context(None, 30.0).unwrap()).unwrap();
    let (_, context, _) = call(Some(parent.to_string())).await;
    assert_eq!(context.unwrap(), parent);

    let mut expired = parent.clone();
    expired["deadline_ms"] = json!(1);
    let (status, _, refused) = call(Some(expired.to_string())).await;
    assert_eq!(
        (status, refused["code"].as_str()),
        (StatusCode::REQUEST_TIMEOUT, Some("DEADLINE_EXCEEDED"))
    );
    let (status, _, refused) = call(Some("{\"version\":1}".into())).await;
    assert_eq!(
        (status, refused["code"].as_str()),
        (
            StatusCode::UNPROCESSABLE_ENTITY,
            Some("INVALID_COMMUNICATION")
        )
    );
}

#[tokio::test]
async fn a_task_older_than_the_recent_list_is_found_by_id_without_private_fields() {
    let (services, entry) = start(false).await;
    let names: Vec<String> = (0..101).map(|index| format!("task-{index:03}")).collect();
    let commands: Vec<(&str, &str)> = names
        .iter()
        .map(|name| (name.as_str(), "research.backtest"))
        .collect();
    let ids = submit(&services, &commands);
    let root = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let recent = root
        .described(Method::GET, "/api/v1/jobs", "/api/v1/jobs", None)
        .await;
    assert_eq!(recent.as_array().unwrap().len(), 100);
    assert!(
        recent
            .as_array()
            .unwrap()
            .iter()
            .all(|job| job["id"] != ids[0].as_str())
    );
    let oldest = root
        .described(
            Method::GET,
            "/api/v1/jobs/{job_id}",
            &format!("/api/v1/jobs/{}", ids[0]),
            None,
        )
        .await;
    assert_eq!(oldest["state"], "QUEUED");
    assert!(oldest.get("payload").is_none() && oldest.get("token").is_none());
    let worker = Client {
        entry,
        credential: authority::worker_token(SECRET),
        session: String::new(),
    };
    let (status, _, _) = worker
        .call(
            Method::GET,
            "/api/v1/communication/events?topic=runtime.task.changed",
            None,
        )
        .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}

fn time_spec() -> Value {
    let calendar: Vec<Value> = (2..15)
        .map(|day| {
            let open = [2, 3, 7, 8, 9, 10, 11, 14].contains(&day);
            let night = [2, 7, 8, 9, 10, 11, 14].contains(&day);
            json!({"date": format!("2025-04-{day:02}"), "is_open": open, "night_open": night})
        })
        .collect();
    let slot = |start: &str, end: &str, offset: u8, phase: &str| json!({"start": start, "end": end, "end_offset": offset, "phase": phase});
    json!({
        "schema_version": 1, "exchange": "SHFE", "product": "AU", "title": "测试跨周末及假日",
        "timezone": "Asia/Shanghai", "calendar_source": "测试完整自然日日历",
        "night_source": "测试节前不开夜盘", "calendar": calendar,
        "periods": [{"start": "2025-04-03", "end": "2025-04-14", "source": "测试时段",
            "day": [slot("09:00:00", "10:15:00", 0, "continuous"),
                    slot("10:30:00", "11:30:00", 0, "continuous"),
                    slot("13:30:00", "15:00:00", 0, "continuous")],
            "night": [slot("20:55:00", "21:00:00", 0, "auction"),
                      slot("21:00:00", "02:30:00", 1, "continuous")]}],
        "exceptions": []
    })
}

#[tokio::test]
async fn trading_time_versions_are_stored_once_and_resolve_sessions() {
    let (_, entry) = start(true).await;
    let anonymous = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let (status, _, _) = anonymous
        .call(Method::GET, "/api/v1/trading-time", None)
        .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let owner = signed_in(&entry).await;
    let base = "/api/v1/trading-time";
    let saved = owner
        .described(Method::POST, base, base, Some(time_spec()))
        .await;
    assert_eq!(saved["spec"], time_spec());
    assert_eq!(saved["id"].as_str().unwrap().len(), 64);
    let again = owner
        .described(Method::POST, base, base, Some(time_spec()))
        .await;
    assert_eq!(again, saved);
    let listed = owner.described(Method::GET, base, base, None).await;
    assert_eq!(listed, json!([saved.clone()]));

    let resolve = format!("{base}/resolve");
    let request = |version: &Value, timestamp: &str| {
        json!({"version": version, "contract": "SHFE.au2506",
               "timestamp": timestamp, "boundary": "event"})
    };
    let span = owner
        .described(
            Method::POST,
            &resolve,
            &resolve,
            Some(request(&saved, "2025-04-12T01:00:00+08:00")),
        )
        .await;
    assert_eq!(
        (span["trading_day"].as_str(), span["session"].as_str()),
        (Some("2025-04-14"), Some("night"))
    );
    let day = format!("{base}/day");
    let spans = owner
        .described(
            Method::POST,
            &day,
            &day,
            Some(json!({"version": saved, "contract": "SHFE.au2506", "trading_day": "2025-04-14"})),
        )
        .await;
    assert_eq!(spans.as_array().unwrap().len(), 4);

    // A changed specification no longer matches its fingerprint.
    let mut altered = saved.clone();
    altered["spec"]["title"] = json!("改动");
    let mut extra = request(&saved, "2025-04-12T01:00:00+08:00");
    extra["unknown"] = json!(1);
    for body in [
        request(&altered, "2025-04-12T01:00:00+08:00"),
        extra,
        request(&saved, "yesterday"),
    ] {
        let (status, _, refused) = owner.call(Method::POST, &resolve, Some(body)).await;
        assert_eq!(
            (status, refused["detail"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some("请求参数格式不正确"))
        );
    }
    // The domain's own refusals keep their reason.
    for (body, path) in [
        (request(&saved, "2025-04-13T10:00:00+08:00"), &resolve),
        (
            json!({"version": saved, "contract": "SHFE.au2506", "trading_day": "2025-04-13"}),
            &day,
        ),
        (
            json!({"version": saved, "contract": "DCE.m2505", "trading_day": "2025-04-14"}),
            &day,
        ),
    ] {
        let (status, _, refused) = owner.call(Method::POST, path, Some(body)).await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
        assert_ne!(refused["detail"], "请求参数格式不正确", "{refused}");
    }
    let mut invalid = time_spec();
    invalid["timezone"] = json!("UTC");
    assert_eq!(
        owner.call(Method::POST, base, Some(invalid)).await.0,
        StatusCode::UNPROCESSABLE_ENTITY
    );
    assert_eq!(
        owner.described(Method::GET, base, base, None).await,
        json!([saved])
    );
}

/// Publish one version of a collection as the internal process's data module does.
fn publish(services: &Services, dataset: &str, version: &str, created_at: f64, scope: Value) {
    let identity = json!({"type_id": "futures.daily", "source": "tushare", "scope": scope, "layer": "STANDARD"});
    let manifest = json!({"type": {"id": "futures.daily", "frequency": "1d"}, "source": "tushare",
                          "layer": "STANDARD", "scope": scope, "path": "private/file.parquet"});
    services
        .store
        .transaction(true, |tx| {
            tx.execute(
                "INSERT INTO data_collections VALUES (?, 'futures.daily', 'market', 'tushare', 'STANDARD', ?) \
                 ON CONFLICT (id) DO NOTHING",
                vec![json!(dataset), json!(identity.to_string())],
            )?;
            tx.execute(
                "INSERT INTO data_versions VALUES (?, ?, ?, ?, 1, ?)",
                vec![json!(version), json!(dataset), json!(version), json!(created_at), json!(manifest.to_string())],
            )
        })
        .unwrap();
}

#[tokio::test]
async fn catalogue_lists_latest_versions_directories_and_history() {
    let (services, entry) = start(false).await;
    let rb = json!({"contract_ids": ["SHFE.RB.202610.20251001"], "connection_id": "c_one"});
    let ma = json!({"contract_ids": ["CZCE.MA.202601.20250101"]});
    publish(&services, "rb", "rb-1", 1.0, rb.clone());
    publish(&services, "rb", "rb-2", 2.0, rb);
    publish(&services, "ma", "ma-1", 3.0, ma);
    services
        .store
        .transaction(true, |tx| {
            tx.execute(
                "INSERT INTO data_version_states VALUES ('ma-1', TRUE, 1, 1)",
                vec![],
            )
        })
        .unwrap();
    let root = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let catalog = "/api/v1/data/catalog";
    let page = root.described(Method::GET, catalog, catalog, None).await;
    assert_eq!(page["total"], 1);
    let latest = &page["items"][0];
    assert_eq!(
        (latest["id"].as_str(), latest["version_count"].as_i64()),
        (Some("rb-2"), Some(2))
    );
    assert!(
        latest["manifest"].get("path").is_none(),
        "storage paths stay private"
    );
    let all = root
        .described(
            Method::GET,
            catalog,
            &format!("{catalog}?include_archived=true"),
            None,
        )
        .await;
    assert_eq!(all["items"][0]["id"], "ma-1");
    assert_eq!(all["items"][0]["archived"], true);
    for (query, total) in [
        ("search=RB.2026", 1),
        ("search=%25", 0),
        ("source=c_one", 1),
        ("source=c_two", 0),
        ("directory=SHFE/RB", 1),
        ("directory=SHFE/R", 0),
        ("directory=CZCE&include_archived=1", 1),
        ("layer=RAW", 0),
    ] {
        let found = root
            .described(Method::GET, catalog, &format!("{catalog}?{query}"), None)
            .await;
        assert_eq!(found["total"], total, "{query}");
    }
    let history = "/api/v1/data/catalog/{dataset_id}/versions";
    let versions = root
        .described(
            Method::GET,
            history,
            "/api/v1/data/catalog/rb/versions?limit=1&offset=1",
            None,
        )
        .await;
    assert_eq!(
        (
            versions["total"].as_i64(),
            versions["items"][0]["id"].as_str()
        ),
        (Some(2), Some("rb-1"))
    );
    let tree = "/api/v1/data/hierarchy";
    let nodes = root.described(Method::GET, tree, tree, None).await;
    assert_eq!(
        nodes[0],
        json!({"path": ["SHFE"], "label": "SHFE · 上期所", "count": 1})
    );
    assert_eq!(
        root.described(
            Method::GET,
            tree,
            &format!("{tree}?include_archived=true"),
            None
        )
        .await[0]["path"],
        json!(["CZCE"])
    );
    for query in [
        "limit=0",
        "limit=101",
        "offset=-1",
        "include_archived=maybe",
    ] {
        let (status, _, refused) = root
            .call(Method::GET, &format!("{catalog}?{query}"), None)
            .await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some("INVALID_INPUT")),
            "{query}"
        );
    }
    // A stored identity outside the current contract is refused, not guessed.
    publish(
        &services,
        "bad",
        "bad-1",
        4.0,
        json!({"contract_ids": ["rb2610"]}),
    );
    let (status, _, refused) = root.call(Method::GET, tree, None).await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    assert_eq!(refused["detail"], "目录实际合约身份不符合当前契约");
    let data = Client {
        entry,
        credential: authority::worker_token(SECRET),
        session: String::new(),
    };
    assert_eq!(
        data.call(Method::GET, catalog, None).await.0,
        StatusCode::UNAUTHORIZED
    );
}

/// Record a version whose manifest points at an artifact stored under the data root.
fn stored_version(services: &Services, id: &str, manifest: Value, rows: i64) {
    let identity = json!({"type_id": "futures.daily", "source": "tushare", "id": id});
    services
        .store
        .transaction(true, |tx| {
            tx.execute(
                "INSERT INTO data_collections VALUES (?, 'futures.daily', 'market', 'tushare', 'RAW', ?)",
                vec![json!(format!("set-{id}")), json!(identity.to_string())],
            )?;
            tx.execute(
                "INSERT INTO data_versions VALUES (?, ?, ?, 1, ?, ?)",
                vec![json!(id), json!(format!("set-{id}")), json!(id), json!(rows), json!(manifest.to_string())],
            )
        })
        .unwrap();
}

fn artifact(services: &Services, name: &str, content: &[u8], format: &str) -> Value {
    let store = asterion_kernel::artifacts::ArtifactStore::new(&services.root, 1 << 20).unwrap();
    let stored = store.put(name, content).unwrap();
    json!({"format": format, "path": stored.name, "checksum": stored.sha256, "bytes": stored.bytes,
           "snapshot_id": null, "type": {"id": "futures.daily"}, "source": "tushare", "layer": "RAW"})
}

fn parquet_file() -> Vec<u8> {
    use arrow_array::{ArrayRef, Int64Array, RecordBatch, StringArray};
    use std::sync::Arc;
    let batch = RecordBatch::try_from_iter(vec![
        (
            "z",
            Arc::new(StringArray::from(vec!["a", "b", "c"])) as ArrayRef,
        ),
        ("n", Arc::new(Int64Array::from(vec![1, 2, 3])) as ArrayRef),
    ])
    .unwrap();
    let mut content = Vec::new();
    let mut writer =
        parquet::arrow::ArrowWriter::try_new(&mut content, batch.schema(), None).unwrap();
    writer.write(&batch).unwrap();
    writer.close().unwrap();
    content
}

#[tokio::test]
async fn version_rows_are_read_verified_and_keep_column_order() {
    let (services, entry) = start(false).await;
    stored_version(
        &services,
        "parquet",
        artifact(&services, "data.parquet", &parquet_file(), "parquet"),
        3,
    );
    let evidence = br#"[{"rows":[{"b":1,"a":2}]},{"rows":[{"b":3,"a":4}]}]"#;
    stored_version(
        &services,
        "evidence",
        artifact(&services, "evidence.json", evidence, "provider_evidence"),
        2,
    );
    stored_version(
        &services,
        "csv",
        artifact(&services, "raw.csv", b"b,a\r\n1,2\r\n", "csv"),
        1,
    );
    stored_version(
        &services,
        "other",
        artifact(&services, "raw.xlsx", b"x", "xlsx"),
        1,
    );
    let mut missing = artifact(&services, "gone.csv", b"b\n1\n", "csv");
    missing["path"] = json!("never-written.csv");
    stored_version(&services, "missing", missing, 1);
    let mut changed = artifact(&services, "changed.csv", b"b\n1\n", "csv");
    changed["checksum"] = json!("0".repeat(64));
    stored_version(&services, "changed", changed, 1);

    let root = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let template = "/api/v1/data/versions/{version_id}";
    let page = root
        .described(
            Method::GET,
            template,
            "/api/v1/data/versions/parquet?offset=1&limit=1",
            None,
        )
        .await;
    assert_eq!(
        (page["total"].as_i64(), page["offset"].as_i64()),
        (Some(3), Some(1))
    );
    assert_eq!(page["rows"], json!([{"z": "b", "n": 2}]));
    assert!(page["version"]["manifest"].get("path").is_none());
    assert_eq!(
        (page["snapshot"].clone(), page["row_sources"].clone()),
        (Value::Null, Value::Null)
    );
    // Columns keep their stored order on the wire.
    for (id, ordered) in [
        ("parquet", r#"{"z":"a","n":1}"#),
        ("evidence", r#"{"b":1,"a":2}"#),
        ("csv", r#"{"b":"1","a":"2"}"#),
    ] {
        let request = axum::http::Request::get(format!("{entry}/api/v1/data/versions/{id}"))
            .header("authorization", format!("Bearer {SECRET}"))
            .body(Body::empty())
            .unwrap();
        let (status, _, content) = send(request).await;
        assert_eq!(status, StatusCode::OK);
        assert!(
            String::from_utf8(content.to_vec())
                .unwrap()
                .contains(ordered),
            "{id}"
        );
    }
    let evidence = root
        .described(
            Method::GET,
            template,
            "/api/v1/data/versions/evidence?offset=1",
            None,
        )
        .await;
    assert_eq!(evidence["rows"], json!([{"b": 3, "a": 4}]));
    for (id, detail) in [
        ("other", "该数据格式尚未安装预览器"),
        ("missing", "数据文件缺失"),
        ("changed", "数据文件校验和不一致"),
    ] {
        let (status, _, refused) = root
            .call(Method::GET, &format!("/api/v1/data/versions/{id}"), None)
            .await;
        assert_eq!(
            (status, refused["detail"].as_str()),
            (StatusCode::UNPROCESSABLE_ENTITY, Some(detail)),
            "{id}"
        );
    }
    let (status, _, refused) = root
        .call(Method::GET, "/api/v1/data/versions/unknown", None)
        .await;
    assert_eq!(
        (status, refused["code"].as_str()),
        (StatusCode::NOT_FOUND, Some("NOT_FOUND"))
    );
    let (status, _, _) = root
        .call(Method::GET, "/api/v1/data/versions/parquet?limit=501", None)
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
}

#[tokio::test]
async fn archiving_is_compare_and_set_and_counts_every_fixed_reference() {
    let (services, entry) = start(false).await;
    let scope = json!({"contract_ids": ["SHFE.RB.202610.20251001"]});
    publish(&services, "rb", "v", 1.0, scope.clone());
    publish(&services, "rb", "newer", 2.0, scope);
    let catalog = json!({"inputs": [{"version_id": "v"}]}).to_string();
    let sql = |statement: &str, params: Vec<Value>| {
        services
            .store
            .transaction(true, |tx| tx.execute(statement, params))
            .unwrap();
    };
    let job = |id: &str, kind: &str, payload: Value, result: Value| {
        sql(
            "INSERT INTO jobs(id,command_id,kind,payload,state,attempt,created_at,result) VALUES(?,?,?,?,'SUCCEEDED',1,1,?)",
            vec![
                json!(id),
                json!(id),
                json!(kind),
                json!(payload.to_string()),
                if result.is_null() {
                    Value::Null
                } else {
                    json!(result.to_string())
                },
            ],
        )
    };
    sql(
        "INSERT INTO data_coverage_reports VALUES('r','v',1,?)",
        vec![json!(json!({"daily_version_id": "v"}).to_string())],
    );
    job(
        "plan",
        "data.history",
        json!({"history_plan": {"request": {"contracts_version_id": "v", "calendar_version_id": "c"}}}),
        Value::Null,
    );
    sql(
        "INSERT INTO reference_releases VALUES('rel',1,?)",
        vec![json!(catalog)],
    );
    sql("INSERT INTO contract_rule_versions VALUES('rule',?)", vec![json!(json!({"basis": null, "contract": {"provenance": {"source_version": "x"}}, "periods": [{"settlement_basis": {"evidence": {"version_id": "v"}}}]}).to_string())]);
    sql(
        "INSERT INTO contract_role_versions VALUES('role',?)",
        vec![json!(
            json!({"source_version": "x", "catalog": {"inputs": [{"version_id": "v"}]}})
                .to_string()
        )],
    );
    sql("INSERT INTO computed_role_versions VALUES('computed',?, 't')", vec![json!(json!({"request": {"contracts_version_id": "x", "daily_inputs": [{"version_id": "v"}]}}).to_string())]);
    job(
        "sync",
        "data.sync",
        json!({"type_id": "futures.daily", "contract_identity": {}}),
        json!({"version_id": "v"}),
    );
    sql(
        "INSERT INTO role_sync_workflows VALUES('flow',?,NULL,NULL)",
        vec![json!(json!({"sync_job_ids": ["sync"]}).to_string())],
    );
    job(
        "run",
        "research.backtest",
        json!({"request": {"version_id": "v"}}),
        Value::Null,
    );
    sql(
        "INSERT INTO research_documents VALUES('a','d','draft',1,?,1,FALSE)",
        vec![json!(json!({"config": {"version_id": "v"}}).to_string())],
    );
    sql(
        "INSERT INTO research_documents VALUES('a','gone','old',1,?,1,TRUE)",
        vec![json!(json!({"config": {"version_id": "v"}}).to_string())],
    );
    sql(
        "INSERT INTO research_packages VALUES('a','p',?)",
        vec![json!(
            json!({"content": {"coverage": {"calendar_version_id": "v"}}}).to_string()
        )],
    );

    let root = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    let state_path = "/api/v1/data/versions/{version_id}/lifecycle";
    let state = root
        .described(
            Method::GET,
            state_path,
            "/api/v1/data/versions/v/lifecycle",
            None,
        )
        .await;
    assert_eq!(
        state["references"],
        json!({
            "coverage_reports": 1, "history_plans": 1, "reference_catalogs": 1, "contract_rules": 1,
            "contract_roles": 3, "research_runs": 1, "research_documents": 1, "research_packages": 1,
        })
    );
    assert_eq!(
        (
            state["reference_count"].as_i64(),
            state["is_latest"].as_bool()
        ),
        (Some(10), Some(false))
    );
    assert_eq!(
        (state["archived"].as_bool(), state["revision"].as_i64()),
        (Some(false), Some(0))
    );
    assert_eq!(state["can_delete"], false);

    let archive_path = "/api/v1/data/versions/{version_id}/archive";
    let archive = |archived: bool, expected: i64| json!({"archived": archived, "expected_revision": expected});
    let archived = root
        .described(
            Method::POST,
            archive_path,
            "/api/v1/data/versions/v/archive",
            Some(archive(true, 0)),
        )
        .await;
    assert_eq!(
        (
            archived["archived"].as_bool(),
            archived["revision"].as_i64()
        ),
        (Some(true), Some(1))
    );
    // A retried request that already took effect returns the same state.
    let retried = root
        .described(
            Method::POST,
            archive_path,
            "/api/v1/data/versions/v/archive",
            Some(archive(true, 0)),
        )
        .await;
    assert_eq!(retried["revision"], 1);
    let (status, _, conflict) = root
        .call(
            Method::POST,
            "/api/v1/data/versions/v/archive",
            Some(archive(false, 0)),
        )
        .await;
    assert_eq!(
        (status, conflict["code"].as_str()),
        (StatusCode::CONFLICT, Some("CONFLICT"))
    );
    let restored = root
        .described(
            Method::POST,
            archive_path,
            "/api/v1/data/versions/v/archive",
            Some(archive(false, 1)),
        )
        .await;
    assert_eq!(
        (
            restored["archived"].as_bool(),
            restored["revision"].as_i64()
        ),
        (Some(false), Some(2))
    );
    for body in [
        json!({"archived": true}),
        archive(true, -1),
        json!({"archived": true, "expected_revision": 0, "x": 1}),
    ] {
        let (status, _, _) = root
            .call(Method::POST, "/api/v1/data/versions/v/archive", Some(body))
            .await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    }
    let (status, _, _) = root
        .call(
            Method::POST,
            "/api/v1/data/versions/missing/archive",
            Some(archive(true, 0)),
        )
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // A reference that cannot be resolved is refused, not counted as zero.
    sql(
        "INSERT INTO role_sync_workflows VALUES('broken',?,NULL,NULL)",
        vec![json!(json!({"sync_job_ids": ["absent"]}).to_string())],
    );
    let (status, _, refused) = root
        .call(Method::GET, "/api/v1/data/versions/v/lifecycle", None)
        .await;
    assert_eq!(
        (status, refused["detail"].as_str()),
        (StatusCode::UNPROCESSABLE_ENTITY, Some("同步依赖任务不存在"))
    );
}

#[tokio::test]
async fn data_types_are_the_fixed_built_in_definitions() {
    let (_, entry) = start(false).await;
    let root = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    let path = "/api/v1/data/types";
    let types = root.described(Method::GET, path, path, None).await;
    let ids: Vec<&str> = types
        .as_array()
        .unwrap()
        .iter()
        .map(|t| t["id"].as_str().unwrap())
        .collect();
    assert_eq!(
        ids,
        [
            "futures.role_mapping",
            "futures.contracts",
            "futures.calendar",
            "futures.settlement",
            "futures.daily",
            "futures.minute",
            "futures.bars"
        ]
    );
    assert_eq!(types[4]["primary_key"], json!(["contract", "trading_day"]));
}

fn fixture(name: &str) -> Value {
    let path = format!("{}/tests/fixtures/{name}.json", env!("CARGO_MANIFEST_DIR"));
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

const DAILY_CSV: &str =
    "合约,日期,开,高,低,收,量\nSHFE.rb2610,2024-01-02,3200,3220,3190,3210,100\n";

fn daily_options(source_id: &str) -> Value {
    json!({"identity": fixture("identity_rb2610"), "type_id": "futures.daily", "frequency": "1d",
           "source_id": source_id,
           "column_mapping": {"contract": "合约", "trading_day": "日期", "open": "开", "high": "高",
                              "low": "低", "close": "收", "vol": "量"}})
}

fn import(command: &str, csv: &str, options: Value) -> Value {
    json!({"command_id": command, "source": "真实供应商历史文件", "csv": csv, "options": options})
}

async fn finished(client: &Client, job: &str) -> Value {
    for _ in 0..200 {
        let (_, _, value) = client
            .call(Method::GET, &format!("/api/v1/jobs/{job}"), None)
            .await;
        if matches!(value["state"].as_str(), Some("SUCCEEDED" | "FAILED")) {
            return value;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    panic!("import {job} did not finish");
}

#[tokio::test]
async fn daily_files_preview_publish_versions_and_charts() {
    let (_, entry) = start(false).await;
    let root = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let preview = "/api/v1/imports/preview";
    let checked = root
        .described(
            Method::POST,
            preview,
            preview,
            Some(import("p", DAILY_CSV, daily_options("vendor_a"))),
        )
        .await;
    assert_eq!(
        (checked["valid"].as_bool(), checked["total"].as_i64()),
        (Some(true), Some(1))
    );
    assert_eq!(
        (
            checked["rows"][0]["exchange"].as_str(),
            checked["rows"][0]["vol"].as_str()
        ),
        (Some("SHFE"), Some("100"))
    );
    assert_eq!(checked["contract_ids"], json!(["SHFE.RB.202610.20240102"]));
    // Mapped fields keep the mapping's order.
    let request = axum::http::Request::post(format!("{entry}{preview}"))
        .header("authorization", format!("Bearer {SECRET}"))
        .header("content-type", "application/json")
        .body(Body::from({
            // A client's own key order: the mapping order is part of the request.
            let body = import("p", DAILY_CSV, daily_options("vendor_a")).to_string();
            let sorted = daily_options("vendor_a")["column_mapping"].to_string();
            body.replace(
                &sorted,
                r#"{"contract":"合约","trading_day":"日期","open":"开","high":"高","low":"低","close":"收","vol":"量"}"#,
            )
        }))
        .unwrap();
    let text = String::from_utf8(send(request).await.2.to_vec()).unwrap();
    assert!(
        text.contains(r#"{"contract":"SHFE.rb2610","trading_day":"2024-01-02","open":"3200""#),
        "{text}"
    );
    let unmapped = root
        .described(
            Method::POST,
            preview,
            preview,
            Some(import(
                "p",
                &DAILY_CSV.replace("rb2610", "rb2609"),
                daily_options("vendor_a"),
            )),
        )
        .await;
    assert_eq!(unmapped["valid"], false);
    let mut altered = daily_options("vendor_a");
    altered["identity"]["catalog"]["contracts"][0]["last_trade_on"] = json!("2026-10-16");
    let (status, _, _) = root
        .call(Method::POST, preview, Some(import("p", DAILY_CSV, altered)))
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    let mut partial = daily_options("vendor_a");
    partial["column_mapping"] = json!({"close": "收"});
    let incomplete = root
        .described(
            Method::POST,
            preview,
            preview,
            Some(import("p", DAILY_CSV, partial)),
        )
        .await;
    assert_eq!(
        incomplete["errors"],
        json!(["缺少必填字段映射：contract, trading_day, open, high, low, vol"])
    );
    let duplicate = format!("{DAILY_CSV}{}\n", DAILY_CSV.lines().nth(1).unwrap());
    for (csv, readable) in [
        (DAILY_CSV.replace("量", "收"), false),
        (DAILY_CSV.replace(",100", ",100,extra"), false),
        (DAILY_CSV.replace("2024-01-02", "2999-01-02"), true),
        (DAILY_CSV.replace("3210", "oops"), true),
        (duplicate, true),
    ] {
        let (status, _, value) = root
            .call(
                Method::POST,
                preview,
                Some(import("p", &csv, daily_options("vendor_a"))),
            )
            .await;
        if readable {
            assert_eq!(
                (status, value["valid"].as_bool()),
                (StatusCode::OK, Some(false)),
                "{csv}"
            );
        } else {
            assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY, "{csv}");
        }
        let (status, _, _) = root
            .call(
                Method::POST,
                "/api/v1/imports",
                Some(import("bad", &csv, daily_options("vendor_a"))),
            )
            .await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY, "{csv}");
    }

    let mut datasets = Vec::new();
    for (index, source) in ["vendor_a", "vendor_a", "vendor_b"].iter().enumerate() {
        let submitted = root
            .described(
                Method::POST,
                "/api/v1/imports",
                "/api/v1/imports",
                Some(import(
                    &format!("import-{index}"),
                    DAILY_CSV,
                    daily_options(source),
                )),
            )
            .await;
        assert_eq!(
            finished(&root, submitted["id"].as_str().unwrap()).await["state"],
            "SUCCEEDED"
        );
        let (_, _, catalog) = root
            .call(
                Method::GET,
                "/api/v1/data/catalog?type_id=futures.daily&layer=STANDARD&include_archived=true",
                None,
            )
            .await;
        let (_, _, history) = root
            .call(
                Method::GET,
                &format!(
                    "/api/v1/data/catalog/{}/versions",
                    catalog["items"][0]["dataset_id"].as_str().unwrap()
                ),
                None,
            )
            .await;
        let _ = history;
        let version = catalog["items"]
            .as_array()
            .unwrap()
            .iter()
            .find(|v| v["job_id"] == submitted["id"])
            .cloned()
            .unwrap_or_else(|| catalog["items"][0].clone());
        datasets.push(version["dataset_id"].clone());
    }
    assert_eq!(datasets[0], datasets[1]);
    assert_ne!(datasets[0], datasets[2]);
    let (_, _, catalog) = root
        .call(
            Method::GET,
            "/api/v1/data/catalog?type_id=futures.daily&layer=STANDARD",
            None,
        )
        .await;
    let standard = catalog["items"]
        .as_array()
        .unwrap()
        .iter()
        .find(|v| v["manifest"]["origin"]["source_id"] == "vendor_a")
        .unwrap()
        .clone();
    let rows = root
        .described(
            Method::GET,
            "/api/v1/data/versions/{version_id}",
            &format!("/api/v1/data/versions/{}", standard["id"].as_str().unwrap()),
            None,
        )
        .await;
    assert_eq!(rows["rows"][0]["close"], "3210");
    let raw_id = standard["manifest"]["inputs"][0].as_str().unwrap();
    let raw = root
        .described(
            Method::GET,
            "/api/v1/data/versions/{version_id}",
            &format!("/api/v1/data/versions/{raw_id}"),
            None,
        )
        .await;
    assert_eq!(raw["rows"][0]["合约"], "SHFE.rb2610");
    let snapshots = root
        .described(Method::GET, "/api/v1/snapshots", "/api/v1/snapshots", None)
        .await;
    assert_eq!(snapshots.as_array().unwrap().len(), 3);
    assert_eq!(
        snapshots[0]["manifest"]["time_semantics"],
        "trading_day_label"
    );
    let bars_path = "/api/v1/snapshots/{snapshot_id}/bars";
    let bars = root
        .described(
            Method::GET,
            bars_path,
            &format!(
                "/api/v1/snapshots/{}/bars",
                snapshots[0]["id"].as_str().unwrap()
            ),
            None,
        )
        .await;
    assert_eq!(
        (bars[0]["close"].as_str(), bars[0]["event_time"].as_str()),
        (Some("3210.00000000"), Some("2024-01-02T00:00:00Z"))
    );
    let (status, _, _) = root
        .call(Method::GET, "/api/v1/snapshots/missing/bars", None)
        .await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // The same command with another input is a conflict, not a new task.
    let (status, _, _) = root
        .call(
            Method::POST,
            "/api/v1/imports",
            Some(import("import-0", DAILY_CSV, daily_options("vendor_c"))),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT);
}

#[tokio::test]
async fn intraday_bars_follow_the_trading_time_version() {
    let (_, entry) = start(false).await;
    let root = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    let options = json!({"identity": fixture("identity_au2506"), "trading_time": fixture("time_au"),
                         "timestamp_semantics": "bar_end", "type_id": "futures.bars",
                         "frequency": "1m", "source_id": "test"});
    let header = "contract,event_time,available_at,trading_day,open,high,low,close,volume\n";
    let row =
        "SHFE.au2506,2025-04-12T02:30:00+08:00,2025-04-12T02:30:00+08:00,2025-04-14,10,11,9,10,2";
    let preview = "/api/v1/imports/preview";
    let check = |csv: String| {
        let root = &root;
        let options = options.clone();
        async move {
            root.described(
                Method::POST,
                preview,
                preview,
                Some(import("p", &csv, options)),
            )
            .await
        }
    };
    assert_eq!(
        check(format!("{header}{row}")).await["rows"][0]["trading_day"],
        "2025-04-14"
    );
    let derived = check(format!(
        "{}{}",
        header.replace("trading_day,", ""),
        row.replace("2025-04-14,", "")
    ))
    .await;
    assert_eq!(derived["rows"][0]["trading_day"], "2025-04-14");
    let wrong = check(format!(
        "{header}{}",
        row.replace("2025-04-14", "2025-04-12")
    ))
    .await;
    assert!(
        wrong["errors"][0].as_str().unwrap().contains("交易日错误"),
        "{wrong}"
    );
    assert_eq!(
        check(format!("{header}{}", row.replace("02:30:00", "21:00:00"))).await["valid"],
        false
    );
    let mut missing = options.clone();
    missing["trading_time"] = Value::Null;
    let (status, _, _) = root
        .call(
            Method::POST,
            preview,
            Some(import("p", &format!("{header}{row}"), missing)),
        )
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    let submitted = root
        .described(
            Method::POST,
            "/api/v1/imports",
            "/api/v1/imports",
            Some(import("bars", &format!("{header}{row}"), options.clone())),
        )
        .await;
    assert_eq!(
        finished(&root, submitted["id"].as_str().unwrap()).await["state"],
        "SUCCEEDED"
    );
    let (_, _, snapshots) = root.call(Method::GET, "/api/v1/snapshots", None).await;
    assert_eq!(snapshots[0]["manifest"]["frequency"], "1m");
    let (_, _, bars) = root
        .call(
            Method::GET,
            &format!(
                "/api/v1/snapshots/{}/bars",
                snapshots[0]["id"].as_str().unwrap()
            ),
            None,
        )
        .await;
    assert_eq!(
        bars,
        json!([{"contract": "SHFE.au2506", "event_time": "2025-04-11T18:30:00Z",
        "available_at": "2025-04-11T18:30:00Z", "trading_day": "2025-04-14", "open": "10.00000000",
        "high": "11.00000000", "low": "9.00000000", "close": "10.00000000", "volume": 2}])
    );
}

#[tokio::test]
async fn an_unpublishable_import_fails_its_task_with_the_reason() {
    let (services, entry) = start(false).await;
    let topic = Topic {
        id: "runtime.task.changed".into(),
        owner: "asterion.runtime".into(),
        payload: "t".into(),
        read_path: "/jobs".into(),
    };
    let repository = Repository::new(services.store.database_id(), topic).unwrap();
    let payload = import("x", "合约\n1\n", daily_options("vendor_a"));
    let request = serde_json::from_value(json!({"op": "submit", "record": {
        "id": "broken", "command_id": "broken", "kind": "data.import_csv",
        "payload": {"source": payload["source"], "csv": payload["csv"], "options": payload["options"]},
        "now": now()}})).unwrap();
    services
        .store
        .transaction(true, |tx| {
            repository
                .run(tx.connection(), request, trace())
                .map_err(|e| StoreError::from(e.to_string()))
        })
        .unwrap();
    let root = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    let job = finished(&root, "broken").await;
    assert_eq!(job["state"], "FAILED");
    assert!(job["error"].as_str().unwrap().contains("字段映射"), "{job}");
    // A refused publication records nothing.
    let (_, _, snapshots) = root.call(Method::GET, "/api/v1/snapshots", None).await;
    assert_eq!(snapshots, json!([]));
    let (_, _, catalog) = root
        .call(
            Method::GET,
            "/api/v1/data/catalog?include_archived=true",
            None,
        )
        .await;
    assert_eq!(catalog["total"], 0);
    assert!(!services.root.join("published").exists());
}

#[tokio::test]
async fn data_sources_are_configured_under_the_account_without_echoing_secrets() {
    const TOKEN: &str = "synthetic-provider-token-never-log";
    let (_, entry) = start(true).await;
    let anonymous = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let configuration = "/api/v1/data/providers/{provider}/configuration";
    let endpoints = [
        (Method::GET, "/api/v1/data/providers", None),
        (
            Method::GET,
            "/api/v1/data/providers/tushare/configuration",
            None,
        ),
        (
            Method::POST,
            "/api/v1/data/providers/tushare/configuration",
            Some(json!({"expected_revision": 0})),
        ),
        (
            Method::POST,
            "/api/v1/data/providers/tushare/configuration/check",
            Some(json!({"expected_revision": 0})),
        ),
    ];
    for (method, path, body) in endpoints.clone() {
        let (status, _, refused) = anonymous.call(method, path, body).await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::UNAUTHORIZED, Some("SESSION_EXPIRED")),
            "{path}"
        );
    }
    let owner = signed_in(&entry).await;
    let listing = owner
        .described(
            Method::GET,
            "/api/v1/data/providers",
            "/api/v1/data/providers",
            None,
        )
        .await;
    assert_eq!(listing[0]["id"], "tushare");
    assert_eq!(listing[0]["configured"], false);
    let path = "/api/v1/data/providers/tushare/configuration";
    let saved = owner
        .described(
            Method::POST,
            configuration,
            path,
            Some(json!({"expected_revision": 0, "secrets": {"token": TOKEN}})),
        )
        .await;
    assert_eq!(
        (
            &saved["revision"],
            &saved["configured"],
            &saved["secret_fields"]
        ),
        (&json!(1), &json!(true), &json!(["token"]))
    );
    assert!(!saved.to_string().contains(TOKEN));
    let read = owner
        .described(Method::GET, configuration, path, None)
        .await;
    assert_eq!(read["values"], json!({}));
    // Invalid requests are refused without echoing what was sent.
    let secrets: serde_json::Map<String, Value> =
        (0..31).map(|i| (i.to_string(), json!(TOKEN))).collect();
    for (suffix, body) in [
        (
            "",
            json!({"expected_revision": 1, "secrets": {"token": [TOKEN]}}),
        ),
        (
            "/check",
            json!({"expected_revision": 1, "secrets": {"token": [TOKEN]}}),
        ),
        ("", json!({"expected_revision": 1, "secrets": secrets})),
        ("", json!({"expected_revision": 1, "extra": TOKEN})),
        ("", json!({"expected_revision": -1})),
    ] {
        let (status, _, refused) = owner
            .call(Method::POST, &format!("{path}{suffix}"), Some(body))
            .await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY, "{suffix}");
        assert!(!refused.to_string().contains(TOKEN));
    }
    let (status, _, stale) = owner
        .call(Method::POST, path, Some(json!({"expected_revision": 0})))
        .await;
    assert_eq!(
        (status, stale["code"].as_str()),
        (StatusCode::CONFLICT, Some("CONFLICT"))
    );
    // A draft that is not runnable is refused before the source is contacted.
    let (status, _, refused) = owner
        .call(
            Method::POST,
            &format!("{path}/check"),
            Some(json!({"expected_revision": 1, "secrets": {"token": null}})),
        )
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    assert!(refused["detail"].as_str().unwrap().contains("请先配置"));

    let connections = "/api/v1/data/connections";
    let created = owner
        .described(
            Method::POST,
            connections,
            connections,
            Some(json!({"provider": "tushare", "name": " 研究 B "})),
        )
        .await;
    assert_eq!(created["name"], "研究 B");
    for body in [
        json!({"provider": "synthetic", "name": "演示"}),
        json!({"provider": "tushare", "name": ""}),
        json!({"provider": "tushare", "name": "x".repeat(81)}),
    ] {
        let (status, _, _) = owner.call(Method::POST, connections, Some(body)).await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    }
    let lifecycle = "/api/v1/data/connections/{identifier}";
    let disabled = owner
        .described(
            Method::POST,
            lifecycle,
            "/api/v1/data/connections/tushare",
            Some(json!({"expected_revision": 0, "name": "研究连接", "state": "disabled"})),
        )
        .await;
    assert_eq!(
        (&disabled["revision"], &disabled["state"]),
        (&json!(1), &json!("disabled"))
    );
    let (status, _, _) = owner
        .call(
            Method::POST,
            "/api/v1/data/connections/tushare",
            Some(json!({"expected_revision": 0, "name": "过期", "state": "enabled"})),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT);
    let (status, _, refused) = owner
        .call(Method::POST, "/api/v1/data/providers/tushare/verify", None)
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    assert!(refused["detail"].as_str().unwrap().contains("停用"));
    let listing = owner
        .described(
            Method::GET,
            "/api/v1/data/providers",
            "/api/v1/data/providers",
            None,
        )
        .await;
    assert_eq!(listing[0]["name"], "研究连接");
    assert_eq!(listing[0]["configured"], true);
    assert_eq!(listing[1]["id"], created["id"]);
    assert_eq!(listing[1]["plugin_id"], "tushare");
    assert_eq!(listing[1]["configured"], false);
    let (status, _, refused) = owner
        .call(
            Method::GET,
            "/api/v1/data/providers/missing/configuration",
            None,
        )
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
    assert_eq!(refused["detail"], "连接实例不存在");
    // The settings workbench reaches sources with its scope, and nothing else.
    let scopes = "/api/v1/access/scopes";
    let grant = owner
        .described(
            Method::POST,
            scopes,
            scopes,
            Some(json!({"scope": "sources"})),
        )
        .await;
    let scoped = Client {
        entry: entry.clone(),
        credential: grant["token"].as_str().unwrap().into(),
        session: owner.session.clone(),
    };
    let (status, _, _) = scoped
        .call(Method::GET, "/api/v1/data/providers", None)
        .await;
    assert_eq!(status, StatusCode::OK);
    let (status, _, _) = scoped.call(Method::GET, "/api/v1/jobs", None).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);

    owner
        .described(
            Method::POST,
            "/api/v1/account/security/lock",
            "/api/v1/account/security/lock",
            None,
        )
        .await;
    for (method, path, body) in endpoints {
        let (status, _, refused) = owner.call(method, path, body).await;
        assert_eq!(
            (status, refused["code"].as_str()),
            (StatusCode::LOCKED, Some("TERMINAL_LOCKED")),
            "{path}"
        );
    }
}

#[tokio::test]
async fn sync_observations_are_retained_under_the_task_lease() {
    let (services, entry) = start(false).await;
    let client = Client {
        entry,
        credential: SECRET.into(),
        session: String::new(),
    };
    let job = submit(&services, &[("observed", "data.sync")]).remove(0);
    let observation = json!({
        "partition": {"api": "fut_daily", "params": {}, "fields": ["close"], "limit": 10},
        "observed_at": "2024-01-02T09:00:00Z", "rows": [{"close": 1}],
    });
    let lease = |token: &str| {
        axum::http::Request::builder()
            .method(Method::POST)
            .header("authorization", format!("Bearer {SECRET}"))
            .header("content-type", "application/json")
            .header("x-lease-token", token.to_string())
    };
    let record = format!("{}/api/v1/jobs/{job}/observations/0", client.entry);
    for (body, expected) in [
        (observation.to_string(), StatusCode::CONFLICT),
        ("x".repeat(8_000_001), StatusCode::PAYLOAD_TOO_LARGE),
        (
            json!({"token": "secret"}).to_string(),
            StatusCode::UNPROCESSABLE_ENTITY,
        ),
    ] {
        let request = lease("stale").uri(&record).body(Body::from(body)).unwrap();
        let (status, _, content) = send(request).await;
        assert_eq!(status, expected);
        assert!(!String::from_utf8_lossy(&content).contains("secret"));
    }
    let resume = format!("{}/api/v1/jobs/{job}/resume", client.entry);
    let (status, _, _) = send(lease("stale").uri(&resume).body(Body::empty()).unwrap()).await;
    assert_eq!(status, StatusCode::CONFLICT);
    let listing = "/api/v1/data/jobs/{job_id}/observations";
    let page = client
        .described(
            Method::GET,
            listing,
            &format!("/api/v1/data/jobs/{job}/observations"),
            None,
        )
        .await;
    assert_eq!(page, json!({"items": [], "total": 0}));
    for path in [
        "/api/v1/data/jobs/missing/observations".to_string(),
        format!("/api/v1/data/jobs/{job}/observations/1/0"),
    ] {
        let (status, _, _) = client.call(Method::GET, &path, None).await;
        assert_eq!(status, StatusCode::NOT_FOUND, "{path}");
    }
    let (status, _, _) = client
        .call(
            Method::GET,
            &format!("/api/v1/data/jobs/{job}/observations?limit=101"),
            None,
        )
        .await;
    assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY);
}

#[tokio::test]
async fn sync_tasks_are_admitted_once_per_command_under_the_account() {
    let (services, entry) = start(true).await;
    let anonymous = Client {
        entry: entry.clone(),
        credential: SECRET.into(),
        session: String::new(),
    };
    let path = "/api/v1/data/sync";
    let calendar = json!({
        "command_id": "calendar-sync", "provider": "tushare", "dataset": "calendar",
        "exchange": "SHFE", "start": "2024-01-02", "end": "2024-01-05",
    });
    let (status, _, _) = anonymous
        .call(Method::POST, path, Some(calendar.clone()))
        .await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let owner = signed_in(&entry).await;
    let changed = |key: &str, value: Value| {
        let mut changed = calendar.clone();
        changed[key] = value;
        changed
    };
    for (body, detail) in [
        (changed("token", json!("secret")), None),
        (changed("end", json!("2999-01-01")), None),
        (changed("command_id", json!("")), None),
        (
            changed("provider", json!("synthetic")),
            Some("不支持该数据源；数据源只能是内置实现"),
        ),
        (changed("dataset", json!("unknown")), None),
        (calendar.clone(), Some("请先配置 Tushare Token")),
    ] {
        let (status, _, refused) = owner.call(Method::POST, path, Some(body)).await;
        assert_eq!(status, StatusCode::UNPROCESSABLE_ENTITY, "{refused}");
        if let Some(detail) = detail {
            assert_eq!(refused["detail"], detail);
        }
    }
    let configuration = "/api/v1/data/providers/tushare/configuration";
    owner
        .call(
            Method::POST,
            configuration,
            Some(json!({"expected_revision": 0, "secrets": {"token": "entry-test-token"}})),
        )
        .await;
    let (status, _, job) = owner.call(Method::POST, path, Some(calendar.clone())).await;
    assert_eq!(status, StatusCode::ACCEPTED, "{job}");
    conforms(&response("post", path), &job).unwrap();
    assert!(job.get("payload").is_none() && job.get("token").is_none());
    assert_eq!(job["state"], "QUEUED");
    let (status, _, again) = owner.call(Method::POST, path, Some(calendar.clone())).await;
    assert_eq!((status, &again["id"]), (StatusCode::ACCEPTED, &job["id"]));
    let (status, _, _) = owner
        .call(
            Method::POST,
            path,
            Some(changed("end", json!("2024-01-04"))),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT);
    let payload = services
        .store
        .transaction(false, |tx| {
            tx.rows(
                "SELECT payload FROM jobs WHERE id = ?",
                vec![job["id"].clone()],
            )
        })
        .unwrap()
        .remove(0)
        .remove(0);
    let payload: Value = serde_json::from_str(payload.as_str().unwrap()).unwrap();
    assert_eq!(payload["type_id"], "futures.calendar");
    assert!(payload["configuration"]["ref"].is_string());
    assert!(payload.get("plugin_digest").is_none());
    let progress = format!("/api/v1/jobs/{}/progress", job["id"].as_str().unwrap());
    let (status, _, _) = owner
        .call(
            Method::POST,
            &progress,
            Some(json!({"token": "stale", "completed": 0, "total": 1})),
        )
        .await;
    assert_eq!(status, StatusCode::CONFLICT);
}
