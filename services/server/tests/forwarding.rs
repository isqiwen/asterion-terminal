mod support;

use asterion_kernel::authority::{self, IssueRequest};
use asterion_server::{
    AUTHORIZATION, Authorization, FORWARDED_ROUTES, Matched, RouteTable, application,
    declared_operations,
};
use axum::{
    Router,
    body::{Body, Bytes},
    extract::Request,
    http::{Method, StatusCode},
    response::IntoResponse,
};
use http_body_util::BodyExt;
use serde_json::{Value, json};
use std::sync::{
    Arc,
    atomic::{AtomicUsize, Ordering},
};
use std::time::{SystemTime, UNIX_EPOCH};
use support::{RULES, SECRET, send, serve, services};

const ROUTES: &str = r#"[
 {"method":"POST","path":"/api/v1/echo/{id}"},
 {"method":"GET","path":"/api/v1/data/history"},
 {"method":"POST","path":"/api/v1/jobs/{id}/publish-data"},
 {"method":"GET","path":"/api/v1/health"}
]"#;

#[test]
fn generated_tables_parse_and_match_templates_by_segment() {
    let routes = RouteTable::parse(FORWARDED_ROUTES).unwrap();
    assert!(!routes.is_empty());
    Authorization::parse(AUTHORIZATION).unwrap();
    assert_eq!(
        routes.resolve(&Method::GET, "/api/v1/health"),
        Matched::Route
    );
    assert_eq!(
        routes.resolve(&Method::POST, "/api/v1/jobs/abc/publish-data"),
        Matched::Route
    );
    assert_eq!(
        routes.resolve(&Method::GET, "/api/v1/jobs/abc/publish-data"),
        Matched::WrongMethod
    );
    for path in [
        "/api/v1/jobs/abc/publish-data/more",
        "/api/v1/jobs/",
        "/api/v1//jobs",
        "api/v1/health",
        "/",
    ] {
        assert_eq!(
            routes.resolve(&Method::GET, path),
            Matched::Missing,
            "{path}"
        );
    }
    // The generated tables and the native description assemble without overlap.
    let services = services();
    let _assembled = application(
        support::settings("http://127.0.0.1:9", true),
        routes,
        Authorization::parse(AUTHORIZATION).unwrap(),
        services.store,
        services.identity,
    )
    .unwrap();
    assert_eq!(declared_operations().unwrap().len(), 51);
}

#[test]
fn invalid_or_duplicate_routes_are_rejected() {
    for table in [
        r#"[{"method":"GET","path":"/other/x"}]"#,
        r#"[{"method":"GET","path":"/api/v1//x"}]"#,
        r#"[{"method":"G ET","path":"/api/v1/x"}]"#,
        r#"[{"method":"GET","path":"/api/v1/x"},{"method":"GET","path":"/api/v1/x"}]"#,
        r#"[{"method":"GET","path":"/api/v1/x","extra":1}]"#,
    ] {
        assert!(RouteTable::parse(table).is_err(), "{table}");
    }
}

fn upstream(calls: Arc<AtomicUsize>) -> Router {
    Router::new().fallback(move |request: Request| {
        let calls = calls.clone();
        async move {
            calls.fetch_add(1, Ordering::SeqCst);
            let (parts, body) = request.into_parts();
            let body = body.collect().await.unwrap().to_bytes();
            let header = |name: &str| parts.headers.get(name).map(|v| v.to_str().unwrap());
            let echo = json!({
                "method": parts.method.as_str(),
                "uri": parts.uri.to_string(),
                "authorization": header("authorization"),
                "forwarded": header("x-asterion-forwarded"),
                "principal": header("x-asterion-principal"),
                "session": header("x-account-session"),
                "account_status": header("x-asterion-account-status"),
                "account": header("x-asterion-account"),
                "length": body.len(),
                "digest": body.iter().map(|b| *b as u64).sum::<u64>(),
            });
            (
                StatusCode::CREATED,
                [("x-asterion-context", "reply"), ("x-upstream", "python")],
                axum::Json(echo),
            )
                .into_response()
        }
    })
}

async fn entry(target: &str) -> String {
    serve(support::app(target, ROUTES, false, &services())).await
}

fn request(
    method: Method,
    url: String,
    credential: Option<&str>,
    session: Option<&str>,
) -> axum::http::Request<Body> {
    let mut builder = axum::http::Request::builder().method(method).uri(url);
    if let Some(credential) = credential {
        builder = builder.header("authorization", format!("Bearer {credential}"));
    }
    if let Some(session) = session {
        builder = builder.header("x-account-session", session);
    }
    builder.body(Body::empty()).unwrap()
}

fn scope_token(scope: &str, session: &str) -> String {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs_f64();
    authority::issue(&IssueRequest {
        secret: SECRET.into(),
        policies: Authorization::parse(RULES).unwrap().policies,
        scope: scope.into(),
        session: session.into(),
        now,
    })
    .unwrap()
    .token
}

#[tokio::test]
async fn authorized_requests_forward_verbatim_with_the_entry_decision() {
    let calls = Arc::new(AtomicUsize::new(0));
    let target = serve(upstream(calls.clone())).await;
    let entry = entry(&target).await;

    let body: Vec<u8> = (0..5_000_000).map(|i| (i % 251) as u8).collect();
    let expected: u64 = body.iter().map(|b| *b as u64).sum();
    let forwarded = axum::http::Request::post(format!("{entry}/api/v1/echo/42?x=1&y=%E4%B8%AD"))
        .header("authorization", format!("Bearer {SECRET}"))
        .header("x-account-session", "session-value")
        .header("x-asterion-principal", "forged")
        .header("x-asterion-forwarded", "forged")
        .header("x-asterion-account-status", "unlocked")
        .header("x-asterion-account", "forged@example.com")
        .header("connection", "keep-alive")
        .body(Body::from(body))
        .unwrap();
    let (status, headers, content) = send(forwarded).await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(headers["x-upstream"], "python");
    assert_eq!(headers["x-asterion-context"], "reply");
    let echo: Value = serde_json::from_slice(&content).unwrap();
    assert_eq!(echo["method"], "POST");
    assert_eq!(echo["uri"], "/api/v1/echo/42?x=1&y=%E4%B8%AD");
    assert_eq!(echo["authorization"], Value::Null);
    assert_eq!(echo["principal"], "root");
    assert_eq!(echo["forwarded"], authority::forwarding_token(SECRET));
    assert_eq!(echo["session"], "session-value");
    // An unknown session is reported as expired; forged account headers never pass.
    assert_eq!(echo["account_status"], "expired");
    assert_eq!(echo["account"], Value::Null);
    assert_eq!(echo["length"], 5_000_000);
    assert_eq!(echo["digest"], expected);
    assert_eq!(calls.load(Ordering::SeqCst), 1);

    for (method, path, status) in [
        (
            Method::GET,
            "/api/v1/echo/42",
            StatusCode::METHOD_NOT_ALLOWED,
        ),
        (Method::POST, "/api/v1/other", StatusCode::NOT_FOUND),
        (
            Method::POST,
            "/api/v1/echo/42/deeper",
            StatusCode::NOT_FOUND,
        ),
        // Native paths answer wrong methods themselves, in the same format.
        (Method::POST, "/api/v1/jobs", StatusCode::METHOD_NOT_ALLOWED),
    ] {
        let (actual, _, content) = send(request(
            method,
            format!("{entry}{path}"),
            Some(SECRET),
            None,
        ))
        .await;
        assert_eq!(actual, status, "{path}");
        let error: Value = serde_json::from_slice(&content).unwrap();
        assert!(error["detail"].is_string() && error["code"].is_string());
    }
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[tokio::test]
async fn credentials_are_authorized_before_anything_reaches_the_upstream() {
    let calls = Arc::new(AtomicUsize::new(0));
    let target = serve(upstream(calls.clone())).await;
    let entry = entry(&target).await;
    let principal =
        |content: Bytes| serde_json::from_slice::<Value>(&content).unwrap()["principal"].clone();
    let refused = |status: StatusCode, content: &Bytes| {
        assert_eq!(status, StatusCode::UNAUTHORIZED);
        let error: Value = serde_json::from_slice(content).unwrap();
        assert_eq!(error["code"], "UNAUTHORIZED");
    };

    let health = format!("{entry}/api/v1/health");
    for credential in [
        None,
        Some("wrong-credential"),
        Some(authority::forwarding_token(SECRET).leak() as &str),
    ] {
        let (status, _, content) =
            send(request(Method::GET, health.clone(), credential, None)).await;
        refused(status, &content);
    }
    assert_eq!(calls.load(Ordering::SeqCst), 0);

    let worker = authority::worker_token(SECRET);
    let publish = format!("{entry}/api/v1/jobs/job-1/publish-data");
    let (status, _, content) = send(request(Method::POST, publish, Some(&worker), None)).await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(principal(content), "worker");
    let (status, _, content) =
        send(request(Method::GET, health.clone(), Some(&worker), None)).await;
    refused(status, &content);

    let token = scope_token("data", "session-a");
    let catalog = format!("{entry}/api/v1/data/history");
    let (status, _, content) = send(request(
        Method::GET,
        catalog.clone(),
        Some(&token),
        Some("session-a"),
    ))
    .await;
    assert_eq!(status, StatusCode::CREATED);
    assert_eq!(principal(content), "data");
    for (url, session) in [(catalog, "session-b"), (health, "session-a")] {
        let (status, _, content) =
            send(request(Method::GET, url, Some(&token), Some(session))).await;
        refused(status, &content);
    }
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

#[tokio::test]
async fn cors_is_owned_by_the_entry_for_approved_origins_only() {
    let calls = Arc::new(AtomicUsize::new(0));
    let target = serve(upstream(calls.clone())).await;
    let entry = entry(&target).await;
    let preflight = |origin: &'static str| {
        axum::http::Request::builder()
            .method(Method::OPTIONS)
            .uri(format!("{entry}/api/v1/echo/1"))
            .header("origin", origin)
            .header("access-control-request-method", "POST")
            .header(
                "access-control-request-headers",
                "authorization,x-account-session",
            )
            .body(Body::empty())
            .unwrap()
    };
    let (status, headers, _) = send(preflight("tauri://localhost")).await;
    assert!(status.is_success());
    assert_eq!(headers["access-control-allow-origin"], "tauri://localhost");
    let (_, headers, _) = send(preflight("https://evil.example")).await;
    assert!(headers.get("access-control-allow-origin").is_none());
    assert_eq!(calls.load(Ordering::SeqCst), 0);
    let request = axum::http::Request::post(format!("{entry}/api/v1/echo/1"))
        .header("origin", "http://localhost:1420")
        .header("authorization", format!("Bearer {SECRET}"))
        .body(Body::empty())
        .unwrap();
    let (_, headers, _) = send(request).await;
    assert_eq!(
        headers["access-control-allow-origin"],
        "http://localhost:1420"
    );
    assert_eq!(
        headers["access-control-expose-headers"],
        "x-asterion-context"
    );
}

#[tokio::test]
async fn unavailable_upstream_and_invalid_configuration_are_refused() {
    let unused = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let target = format!("http://{}", unused.local_addr().unwrap());
    drop(unused);
    let entry = entry(&target).await;
    let (status, _, content) = send(request(
        Method::GET,
        format!("{entry}/api/v1/health"),
        Some(SECRET),
        None,
    ))
    .await;
    assert_eq!(status, StatusCode::BAD_GATEWAY);
    let error: Value = serde_json::from_slice(&content).unwrap();
    assert_eq!(error["code"], "UPSTREAM_UNAVAILABLE");

    let services = services();
    let build = |settings, routes: &str, rules: &str| {
        application(
            settings,
            RouteTable::parse(routes).unwrap(),
            Authorization::parse(rules).unwrap(),
            services.store.clone(),
            services.identity.clone(),
        )
    };
    for upstream in ["https://127.0.0.1:1", "http://127.0.0.1:1/api", "not a url"] {
        assert!(build(support::settings(upstream, false), "[]", RULES).is_err());
    }
    let mut short = support::settings(&target, false);
    short.secret = "short".into();
    assert!(build(short, "[]", RULES).is_err());
    let mut lease = support::settings(&target, false);
    lease.lease_seconds = 0.0;
    assert!(build(lease, "[]", RULES).is_err());
    // A native operation can never also be forwarded.
    for owned in [
        r#"[{"method":"POST","path":"/api/v1/account/login"}]"#,
        r#"[{"method":"GET","path":"/api/v1/jobs/{id}"}]"#,
        r#"[{"method":"GET","path":"/api/v1/communication/events"}]"#,
    ] {
        let refused = build(support::settings(&target, false), owned, RULES)
            .err()
            .unwrap();
        assert!(refused.contains("owned twice"), "{refused}");
    }
    // Task events must be declared; other declaration shapes are not accepted.
    let undeclared = RULES.replace("runtime.task.changed", "runtime.other.changed");
    assert!(build(support::settings(&target, false), "[]", &undeclared).is_err());
    assert!(
        Authorization::parse(
            r#"{"policies":{},"worker_grants":[],"event_read_paths":{"a.b":"/jobs"}}"#
        )
        .is_err()
    );
}

#[tokio::test]
async fn forwarded_requests_carry_the_account_state_of_the_session() {
    let target = serve(upstream(Arc::new(AtomicUsize::new(0)))).await;
    let services = services();
    let store = services.store.clone();
    let entry = serve(support::app(&target, ROUTES, false, &services)).await;
    let account = |path: &str, session: &str, body: Value| {
        axum::http::Request::post(format!("{entry}/api/v1/account/{path}"))
            .header("authorization", format!("Bearer {SECRET}"))
            .header("x-account-session", session)
            .header("content-type", "application/json")
            .body(Body::from(body.to_string()))
            .unwrap()
    };
    let user = json!({
        "email": "owner@example.com", "password": "owner-password-123", "pin": "246810",
        "first_name": "Owner", "last_name": "Account"
    });
    assert_eq!(send(account("register", "", user)).await.0, StatusCode::OK);
    let code = json!({"email": "owner@example.com", "code": "000000"});
    assert_eq!(send(account("verify", "", code)).await.0, StatusCode::OK);
    let credentials = json!({"email": "owner@example.com", "password": "owner-password-123"});
    let (_, _, login) = send(account("login", "", credentials)).await;
    let login: Value = serde_json::from_slice(&login).unwrap();
    let session = login["session"].as_str().unwrap().to_string();

    let observed = || async {
        let catalog = request(
            Method::GET,
            format!("{entry}/api/v1/data/history"),
            Some(SECRET),
            Some(&session),
        );
        let (status, _, content) = send(catalog).await;
        assert_eq!(status, StatusCode::CREATED);
        let echo: Value = serde_json::from_slice(&content).unwrap();
        (echo["account_status"].clone(), echo["account"].clone())
    };
    let owner = json!("owner@example.com");
    assert_eq!(observed().await, ("unlocked".into(), owner.clone()));
    let lock = account("security/lock", &session, json!({}));
    assert_eq!(send(lock).await.0, StatusCode::OK);
    assert_eq!(observed().await, ("locked".into(), owner));

    // A missing security record is not repaired, and a store fault decides nothing.
    store
        .transaction(true, |tx| {
            tx.execute("DELETE FROM identity_pin_security", vec![])
        })
        .unwrap();
    assert_eq!(observed().await, ("unsupported".into(), Value::Null));
    store
        .transaction(true, |tx| {
            tx.execute("DROP TABLE identity_pin_security", vec![])
        })
        .unwrap();
    assert_eq!(observed().await, ("unavailable".into(), Value::Null));
}
