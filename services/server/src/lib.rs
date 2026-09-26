//! Rust HTTP entry for the terminal backend (L3 application service).
//!
//! During the Python-to-Rust migration this is the only listener clients use.
//! Every request is authorized here with the fixed kernel authority. Operations
//! not yet migrated are forwarded to the internal Python process by an explicit
//! route table generated from that process's OpenAPI description, carrying the
//! forwarding credential and the authorized principal instead of the client's
//! credential. Each operation has exactly one implementation at any time; the
//! table and forwarding code are deleted when the last operation is migrated.
mod access;
mod account;
mod collection;
mod context;
mod data_catalog;
mod events;
pub mod identity;
mod imports;
mod input;
mod mail;
mod observations;
mod sources;
mod sync;
mod tasks;
mod trading_time;

pub use asterion_store::{self as store, Store, StoreError};
pub use identity::{Delivery, Identity, IdentityError, Verification};
pub use mail::Mailer;

use asterion_kernel::authority::{self, Check, Grant, Policies, Rules};
use asterion_kernel::events::{Journal, Topic};
use asterion_kernel::tasks::{self as kernel_tasks, repository::Repository};
use axum::{
    Router,
    body::Body,
    extract::{FromRequestParts, Query, Request, State},
    handler::Handler,
    http::request::Parts,
    http::{HeaderMap, HeaderName, HeaderValue, Method, StatusCode, Uri, header},
    response::{IntoResponse, Response},
    routing::MethodRouter,
};
use hyper_util::{
    client::legacy::{Client, connect::HttpConnector},
    rt::TokioExecutor,
};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tower_http::cors::{AllowOrigin, CorsLayer};

/// Operations still owned by the internal Python process.
pub const FORWARDED_ROUTES: &str = include_str!("../forwarded_routes.json");
/// Scope policies, worker grants and event topics of the product assembly.
pub const AUTHORIZATION: &str = include_str!("../authorization.json");
/// The published description of the operations this entry implements.
pub const OPERATIONS: &str = include_str!("../openapi.json");

pub(crate) const FORWARDED: &str = "x-asterion-forwarded";
pub(crate) const PRINCIPAL: &str = "x-asterion-principal";
const ACCOUNT: &str = "x-asterion-account";
pub(crate) const ACCOUNT_STATUS: &str = "x-asterion-account-status";
const EVENTS: &str = "/communication/events";
const TASK_TOPIC: &str = "runtime.task.changed";

/// A declared event topic: its publishing owner and the path whose read
/// permission grants replay.
#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TopicDeclaration {
    pub owner: String,
    pub read_path: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Authorization {
    pub policies: Policies,
    pub worker_grants: Vec<Grant>,
    pub topics: BTreeMap<String, TopicDeclaration>,
}
impl Authorization {
    fn topics(&self) -> Vec<Topic> {
        self.topics
            .iter()
            .map(|(id, topic)| Topic {
                id: id.clone(),
                owner: topic.owner.clone(),
                // Registry-local identity of the payload type; replay does not
                // interpret payloads.
                payload: id.clone(),
                read_path: topic.read_path.clone(),
            })
            .collect()
    }
}
impl Authorization {
    pub fn parse(json: &str) -> Result<Self, String> {
        serde_json::from_str(json).map_err(|e| e.to_string())
    }
}

const ORIGINS: [&str; 4] = [
    "http://localhost:1420",
    "http://127.0.0.1:1420",
    "tauri://localhost",
    "http://tauri.localhost",
];
const HOP_BY_HOP: [&str; 8] = [
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
];

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Route {
    pub method: String,
    pub path: String,
}

/// Exact method and segment-wise path templates; `{name}` matches one
/// non-empty segment. No route may appear twice.
#[derive(Debug, Clone)]
pub struct RouteTable {
    routes: Vec<(Method, Vec<Segment>)>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
enum Segment {
    Literal(String),
    Parameter,
}
#[derive(Debug, PartialEq, Eq)]
pub enum Matched {
    Route,
    WrongMethod,
    Missing,
}

fn segments(path: &str) -> Option<Vec<&str>> {
    let rest = path.strip_prefix('/')?;
    let parts: Vec<&str> = rest.split('/').collect();
    if parts.iter().any(|part| part.is_empty()) {
        return None;
    }
    Some(parts)
}

impl RouteTable {
    pub fn parse(json: &str) -> Result<Self, String> {
        let routes: Vec<Route> = serde_json::from_str(json).map_err(|e| e.to_string())?;
        let mut parsed = Vec::with_capacity(routes.len());
        for route in routes {
            let method = Method::from_bytes(route.method.as_bytes())
                .map_err(|_| format!("Invalid route method: {}", route.method))?;
            let template = segments(&route.path)
                .filter(|_| route.path.starts_with("/api/v1/"))
                .ok_or_else(|| format!("Invalid route path: {}", route.path))?
                .into_iter()
                .map(|part| {
                    if part.starts_with('{') && part.ends_with('}') && part.len() > 2 {
                        Segment::Parameter
                    } else {
                        Segment::Literal(part.to_string())
                    }
                })
                .collect::<Vec<_>>();
            if parsed.iter().any(|(m, t)| *m == method && *t == template) {
                return Err(format!("Duplicate route: {} {}", route.method, route.path));
            }
            parsed.push((method, template));
        }
        Ok(Self { routes: parsed })
    }
    pub fn len(&self) -> usize {
        self.routes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.routes.is_empty()
    }
    pub fn resolve(&self, method: &Method, path: &str) -> Matched {
        let Some(parts) = segments(path) else {
            return Matched::Missing;
        };
        let mut path_found = false;
        for (route_method, template) in &self.routes {
            let same = template.len() == parts.len()
                && template
                    .iter()
                    .zip(&parts)
                    .all(|(segment, part)| match segment {
                        Segment::Literal(literal) => literal == part,
                        Segment::Parameter => true,
                    });
            if same {
                if route_method == method {
                    return Matched::Route;
                }
                path_found = true;
            }
        }
        if path_found {
            Matched::WrongMethod
        } else {
            Matched::Missing
        }
    }
}

type Upstream = Client<HttpConnector, Body>;

/// Runtime settings of the entry.
pub struct Settings {
    /// `http://host:port` of the internal Python process.
    pub upstream: String,
    /// Runtime secret shared with the Python process (at least 24 characters).
    pub secret: String,
    /// Whether protected operations require an unlocked account session.
    pub require_account: bool,
    /// Task lease granted by claims and renewals.
    pub lease_seconds: f64,
    /// Root of the data artifacts that published versions reference.
    pub data_root: std::path::PathBuf,
}

/// Shared state of the entry: authorization, native services and forwarding.
pub struct Entry {
    base: Uri,
    routes: RouteTable,
    client: Upstream,
    authorization: Authorization,
    secret: String,
    forwarding: HeaderValue,
    require_account: bool,
    lease_seconds: f64,
    data_root: std::path::PathBuf,
    store: Arc<Store>,
    identity: Arc<Identity>,
    tasks: Repository,
    journal: Journal,
    /// Wakes the import executor when an import is submitted.
    imports: tokio::sync::Notify,
    /// Wakes the sync collector when a sync task is submitted.
    syncs: tokio::sync::Notify,
}

/// Task kinds this entry executes; every other kind belongs to the internal worker.
pub(crate) fn native_kinds() -> BTreeSet<String> {
    [asterion_data::imports::KIND, asterion_data::sync::KIND]
        .into_iter()
        .map(String::from)
        .collect()
}

/// A refused native operation: `{"detail", "code"}` with its status.
pub(crate) struct Failure {
    status: StatusCode,
    detail: String,
    code: &'static str,
}
impl Failure {
    pub(crate) fn new(status: StatusCode, detail: &str, code: &'static str) -> Self {
        Self {
            status,
            detail: detail.into(),
            code,
        }
    }
    pub(crate) fn invalid() -> Self {
        Self::new(
            StatusCode::UNPROCESSABLE_ENTITY,
            "请求参数格式不正确",
            "INVALID_INPUT",
        )
    }
    pub(crate) fn internal() -> Self {
        Self::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "服务暂不可用，请稍后重试",
            "INTERNAL_ERROR",
        )
    }
}
impl From<StoreError> for Failure {
    fn from(_: StoreError) -> Self {
        Self::internal()
    }
}
impl IntoResponse for Failure {
    fn into_response(self) -> Response {
        error(self.status, &self.detail, self.code)
    }
}

/// One native operation and its handler.
pub(crate) struct Operation {
    method: Method,
    path: &'static str,
    handler: MethodRouter<Arc<Entry>>,
}
impl Operation {
    pub(crate) fn get<H: Handler<T, Arc<Entry>>, T: 'static>(
        path: &'static str,
        handler: H,
    ) -> Self {
        Self {
            method: Method::GET,
            path,
            handler: axum::routing::get(handler),
        }
    }
    pub(crate) fn post<H: Handler<T, Arc<Entry>>, T: 'static>(
        path: &'static str,
        handler: H,
    ) -> Self {
        Self {
            method: Method::POST,
            path,
            handler: axum::routing::post(handler),
        }
    }
    /// Accept request bodies up to `bytes` (larger than the default limit).
    pub(crate) fn limited(mut self, bytes: usize) -> Self {
        self.handler = self
            .handler
            .layer(axum::extract::DefaultBodyLimit::max(bytes));
        self
    }
}

fn operations() -> Vec<Operation> {
    [
        account::operations(),
        access::operations(),
        tasks::operations(),
        events::operations(),
        trading_time::operations(),
        data_catalog::operations(),
        imports::operations(),
        sources::operations(),
        observations::operations(),
        sync::operations(),
    ]
    .into_iter()
    .flatten()
    .collect()
}

/// Method and path of every operation in the published native description.
pub fn declared_operations() -> Result<BTreeSet<(String, String)>, String> {
    let description: serde_json::Value =
        serde_json::from_str(OPERATIONS).map_err(|e| e.to_string())?;
    let paths = description["paths"]
        .as_object()
        .ok_or("The native description has no paths")?;
    Ok(paths
        .iter()
        .flat_map(|(path, item)| {
            item.as_object()
                .into_iter()
                .flat_map(|methods| methods.keys())
                .map(move |method| (method.to_uppercase(), path.clone()))
        })
        .collect())
}

/// The account session a request presents; empty when absent.
pub(crate) fn session_token(headers: &HeaderMap) -> String {
    headers
        .get("x-account-session")
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .to_string()
}

/// The principal of a request authorized by the entry.
pub struct Authorized(pub String);

impl FromRequestParts<Arc<Entry>> for Authorized {
    type Rejection = Response;
    async fn from_request_parts(parts: &mut Parts, entry: &Arc<Entry>) -> Result<Self, Response> {
        entry
            .principal(&parts.method, &parts.uri, &parts.headers)
            .map(Authorized)
            .ok_or_else(unauthorized)
    }
}

fn unauthorized() -> Response {
    error(
        StatusCode::UNAUTHORIZED,
        "请求身份无效或无权访问此接口",
        "UNAUTHORIZED",
    )
}

impl Entry {
    /// The authorized principal, or None. Event reads authorize against the
    /// declared read path of the requested topic.
    fn principal(&self, method: &Method, uri: &Uri, headers: &HeaderMap) -> Option<String> {
        let text = |name| {
            headers
                .get(name)
                .and_then(|v: &HeaderValue| v.to_str().ok())
        };
        let credential = text(header::AUTHORIZATION)
            .and_then(|value| value.strip_prefix("Bearer "))
            .unwrap_or("");
        let mut path = uri.path().strip_prefix("/api/v1")?.to_string();
        if path == EVENTS {
            let query: Query<HashMap<String, String>> = Query::try_from_uri(uri).ok()?;
            if method != Method::GET {
                return None;
            }
            path = self
                .authorization
                .topics
                .get(query.0.get("topic")?)?
                .read_path
                .clone();
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .ok()?
            .as_secs_f64();
        authority::check(
            &Rules {
                secret: &self.secret,
                policies: &self.authorization.policies,
                worker_grants: &self.authorization.worker_grants,
            },
            &Check {
                credential,
                method: method.as_str(),
                path: &path,
                session: text(HeaderName::from_static("x-account-session")).unwrap_or(""),
                now,
            },
        )
        .ok()
    }
}

pub(crate) fn error(status: StatusCode, detail: &str, code: &str) -> Response {
    (
        status,
        axum::Json(serde_json::json!({"detail": detail, "code": code})),
    )
        .into_response()
}

fn strip_hop_by_hop(headers: &mut HeaderMap) {
    let listed: Vec<HeaderName> = headers
        .get_all(header::CONNECTION)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .flat_map(|value| value.split(','))
        .filter_map(|name| HeaderName::from_bytes(name.trim().as_bytes()).ok())
        .collect();
    for name in listed {
        headers.remove(name);
    }
    for name in HOP_BY_HOP {
        headers.remove(name);
    }
}

async fn forward(State(state): State<Arc<Entry>>, request: Request) -> Response {
    match state.routes.resolve(request.method(), request.uri().path()) {
        Matched::Route => {}
        Matched::WrongMethod => {
            return error(
                StatusCode::METHOD_NOT_ALLOWED,
                "接口不支持该请求方法",
                "NOT_ALLOWED",
            );
        }
        Matched::Missing => return error(StatusCode::NOT_FOUND, "接口不存在", "NOT_FOUND"),
    }
    let Some(principal) = state.principal(request.method(), request.uri(), request.headers())
    else {
        return unauthorized();
    };
    let Ok(principal) = HeaderValue::from_str(&principal) else {
        return unauthorized();
    };
    // The account behind the session, evaluated by the identity service; the
    // internal process decides which of its operations require it.
    let token = session_token(request.headers());
    let account = if token.is_empty() {
        identity::Session::Expired
    } else {
        let identity = state.identity.clone();
        match tokio::task::spawn_blocking(move || identity.session(&token)).await {
            Ok(session) => session,
            Err(_) => identity::Session::Unavailable,
        }
    };
    let (mut parts, body) = request.into_parts();
    let path_and_query = parts
        .uri
        .path_and_query()
        .map(|value| value.as_str().to_string())
        .unwrap_or_else(|| parts.uri.path().to_string());
    let Ok(uri) = Uri::builder()
        .scheme(state.base.scheme().expect("validated upstream").clone())
        .authority(state.base.authority().expect("validated upstream").clone())
        .path_and_query(path_and_query)
        .build()
    else {
        return error(StatusCode::BAD_REQUEST, "请求地址无效", "INVALID_INPUT");
    };
    strip_hop_by_hop(&mut parts.headers);
    parts.headers.remove(header::HOST);
    // The upstream receives the entry's decision, never the client credential.
    parts.headers.remove(header::AUTHORIZATION);
    parts
        .headers
        .insert(HeaderName::from_static(FORWARDED), state.forwarding.clone());
    parts
        .headers
        .insert(HeaderName::from_static(PRINCIPAL), principal);
    parts.headers.remove(ACCOUNT);
    let (status, email) = match &account {
        identity::Session::Unlocked(email) => ("unlocked", Some(email)),
        identity::Session::Locked(email) => ("locked", Some(email)),
        identity::Session::Expired => ("expired", None),
        identity::Session::Unsupported => ("unsupported", None),
        identity::Session::Unavailable => ("unavailable", None),
    };
    parts.headers.insert(
        HeaderName::from_static(ACCOUNT_STATUS),
        HeaderValue::from_static(status),
    );
    if let Some(value) = email.and_then(|email| HeaderValue::from_str(email).ok()) {
        parts
            .headers
            .insert(HeaderName::from_static(ACCOUNT), value);
    }
    parts.uri = uri;
    let upstream = Request::from_parts(parts, body);
    match state.client.request(upstream).await {
        Ok(response) => {
            let (mut parts, body) = response.into_parts();
            strip_hop_by_hop(&mut parts.headers);
            Response::from_parts(parts, Body::new(body))
        }
        Err(_) => error(
            StatusCode::BAD_GATEWAY,
            "后台服务暂不可用，请稍后重试",
            "UPSTREAM_UNAVAILABLE",
        ),
    }
}

fn cors() -> CorsLayer {
    CorsLayer::new()
        .allow_origin(AllowOrigin::list(ORIGINS.map(HeaderValue::from_static)))
        .allow_methods([Method::GET, Method::POST])
        .allow_headers([
            header::AUTHORIZATION,
            header::CONTENT_TYPE,
            HeaderName::from_static("x-lease-token"),
            HeaderName::from_static("x-account-session"),
            HeaderName::from_static("x-asterion-context"),
        ])
        .expose_headers([HeaderName::from_static("x-asterion-context")])
}

/// The complete application: CORS for every response, native operations with
/// their communication context, then authorization and forwarding. Native
/// operations must match their published description exactly, and none may
/// also be forwarded.
pub fn application(
    settings: Settings,
    routes: RouteTable,
    authorization: Authorization,
    store: Arc<Store>,
    identity: Arc<Identity>,
) -> Result<Router, String> {
    let operations = operations();
    let served: BTreeSet<(String, String)> = operations
        .iter()
        .map(|operation| (operation.method.to_string(), operation.path.to_string()))
        .collect();
    let declared = declared_operations()?;
    if let Some((method, path)) = served.difference(&declared).next() {
        return Err(format!("Operation {method} {path} is not described"));
    }
    if let Some((method, path)) = declared.difference(&served).next() {
        return Err(format!("Described operation {method} {path} is not served"));
    }
    for operation in &operations {
        if routes.resolve(&operation.method, operation.path) != Matched::Missing {
            return Err(format!(
                "Operation {} {} is owned twice",
                operation.method, operation.path
            ));
        }
    }
    if settings.secret.len() < 24 {
        return Err("The runtime secret must have at least 24 characters".into());
    }
    kernel_tasks::check_duration(settings.lease_seconds)?;
    let base: Uri = settings
        .upstream
        .parse()
        .map_err(|_| "Invalid upstream URL")?;
    if base.scheme_str() != Some("http")
        || base.authority().is_none()
        || !matches!(base.path(), "" | "/")
        || base.query().is_some()
    {
        return Err("Upstream must be an http://host:port URL without a path".into());
    }
    store
        .ensure(&trading_time::TABLES)
        .map_err(|error| format!("Trading-time storage: {error}"))?;
    let topics = authorization.topics();
    let task_topic = topics
        .iter()
        .find(|topic| topic.id == TASK_TOPIC)
        .ok_or("The task event topic is not declared")?
        .clone();
    let tasks = Repository::new(store.database_id(), task_topic).map_err(|e| e.to_string())?;
    let journal = Journal::new(store.database_id(), topics)?;
    let client = Client::builder(TokioExecutor::new()).build_http();
    let forwarding = HeaderValue::from_str(&authority::forwarding_token(&settings.secret))
        .map_err(|_| "Invalid forwarding credential")?;
    let state = Arc::new(Entry {
        base,
        routes,
        client,
        authorization,
        secret: settings.secret,
        forwarding,
        require_account: settings.require_account,
        lease_seconds: settings.lease_seconds,
        data_root: settings.data_root,
        store,
        identity,
        tasks,
        journal,
        imports: tokio::sync::Notify::new(),
        syncs: tokio::sync::Notify::new(),
    });
    imports::start(state.clone());
    collection::start(state.clone());
    let native = operations
        .into_iter()
        .fold(Router::new(), |router, operation| {
            router.route(operation.path, operation.handler)
        })
        .route_layer(axum::middleware::from_fn(context::ingress));
    Ok(native
        .method_not_allowed_fallback(|| async {
            error(
                StatusCode::METHOD_NOT_ALLOWED,
                "接口不支持该请求方法",
                "NOT_ALLOWED",
            )
        })
        .fallback(forward)
        .with_state(state)
        .layer(cors()))
}
