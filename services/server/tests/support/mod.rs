//! Shared fixtures of the entry's integration tests.
#![allow(dead_code)]

use asterion_server::{
    Authorization, Identity, Mailer, OPERATIONS, RouteTable, Settings, Store, Verification,
    application,
};
use axum::{
    Router,
    body::{Body, Bytes},
    http::{HeaderMap, StatusCode},
};
use http_body_util::BodyExt;
use hyper_util::{client::legacy::Client, rt::TokioExecutor};
use serde_json::Value;
use std::path::PathBuf;
use std::sync::Arc;

pub const SECRET: &str = "entry-test-secret-at-least-24-characters";
pub const RULES: &str = r#"{
 "policies": {"data": [{"path": "/data/history", "methods": ["GET"], "descendants": false}],
              "sources": [{"path": "/data/providers", "methods": ["GET", "POST"], "descendants": true},
                          {"path": "/data/connections", "methods": ["GET", "POST"], "descendants": true}],
              "tasks": [{"path": "/jobs", "methods": ["GET"], "descendants": true}]},
 "worker_grants": [
  {"path": "/jobs/claim", "methods": ["POST"], "descendants": false},
  {"path": "/jobs/:id/heartbeat", "methods": ["POST"], "descendants": false},
  {"path": "/jobs/:id/fail", "methods": ["POST"], "descendants": false},
  {"path": "/jobs/:id/progress", "methods": ["POST"], "descendants": false},
  {"path": "/jobs/:id/publish-data", "methods": ["POST"], "descendants": false}],
 "topics": {
  "runtime.task.changed": {"owner": "asterion.runtime", "read_path": "/jobs"},
  "data.version.published": {"owner": "asterion.data", "read_path": "/data/history"}}
}"#;
/// Core and data tables the internal process creates before the entry starts.
const CORE: [&str; 20] = [
    "CREATE TABLE snapshots(id VARCHAR PRIMARY KEY,job_id VARCHAR NOT NULL UNIQUE,manifest JSON NOT NULL)",
    "CREATE TABLE data_coverage_reports(id VARCHAR PRIMARY KEY,daily_version_id VARCHAR NOT NULL,created_at FLOAT NOT NULL,report JSON NOT NULL)",
    "CREATE TABLE reference_releases(id VARCHAR(64) PRIMARY KEY,published_at FLOAT NOT NULL,catalog JSON NOT NULL)",
    "CREATE TABLE contract_rule_versions(id VARCHAR PRIMARY KEY,spec JSON NOT NULL)",
    "CREATE TABLE contract_role_versions(id VARCHAR PRIMARY KEY,spec JSON NOT NULL)",
    "CREATE TABLE computed_role_versions(id VARCHAR PRIMARY KEY,spec JSON NOT NULL,published_at VARCHAR NOT NULL)",
    "CREATE TABLE role_sync_workflows(id VARCHAR PRIMARY KEY,request JSON NOT NULL,job_id VARCHAR,error VARCHAR)",
    "CREATE TABLE research_documents(owner VARCHAR,id VARCHAR,name VARCHAR NOT NULL,revision INTEGER NOT NULL,content JSON NOT NULL,updated_at FLOAT NOT NULL,deleted BOOLEAN NOT NULL,PRIMARY KEY(owner,id))",
    "CREATE TABLE research_packages(owner VARCHAR,id VARCHAR,package JSON NOT NULL,PRIMARY KEY(owner,id))",
    "CREATE TABLE data_collections(id VARCHAR PRIMARY KEY,type_id VARCHAR NOT NULL,domain VARCHAR NOT NULL,source VARCHAR NOT NULL,layer VARCHAR NOT NULL,identity JSON NOT NULL)",
    "CREATE TABLE data_versions(id VARCHAR PRIMARY KEY,dataset_id VARCHAR NOT NULL REFERENCES data_collections(id),job_id VARCHAR NOT NULL,created_at FLOAT NOT NULL,rows INTEGER NOT NULL,manifest JSON NOT NULL)",
    "CREATE TABLE data_version_states(version_id VARCHAR PRIMARY KEY REFERENCES data_versions(id),archived BOOLEAN NOT NULL,revision INTEGER NOT NULL,updated_at FLOAT NOT NULL)",
    "CREATE TABLE data_connections(id VARCHAR PRIMARY KEY,provider VARCHAR NOT NULL,name VARCHAR NOT NULL)",
    "CREATE TABLE data_connection_settings(id VARCHAR PRIMARY KEY,name VARCHAR NOT NULL,state VARCHAR NOT NULL,revision INTEGER NOT NULL)",
    "CREATE TABLE data_provider_configurations(provider VARCHAR PRIMARY KEY,revision INTEGER NOT NULL,schema_version INTEGER NOT NULL,snapshot_ref VARCHAR NOT NULL)",
    "CREATE TABLE data_connection_verifications(provider VARCHAR PRIMARY KEY,revision INTEGER NOT NULL,started_at FLOAT NOT NULL,checked_at FLOAT NOT NULL,status VARCHAR NOT NULL)",
    "CREATE TABLE ingestion_observations(job_id VARCHAR,attempt INTEGER,partition_index INTEGER,manifest JSON NOT NULL,PRIMARY KEY(job_id,attempt,partition_index))",
    "CREATE TABLE jobs(id TEXT PRIMARY KEY,command_id TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,payload JSON NOT NULL,state TEXT NOT NULL,attempt INTEGER NOT NULL,token TEXT,worker_id TEXT,lease_until FLOAT,created_at FLOAT NOT NULL,error TEXT,result JSON)",
    "CREATE TABLE communication_heads(topic TEXT PRIMARY KEY,sequence BIGINT NOT NULL)",
    "CREATE TABLE communication_events(id TEXT PRIMARY KEY,topic TEXT NOT NULL,sequence BIGINT NOT NULL,stream TEXT NOT NULL,message JSON NOT NULL,UNIQUE(topic,sequence))",
];

pub struct Services {
    pub root: PathBuf,
    pub store: Arc<Store>,
    pub identity: Arc<Identity>,
}

pub fn services() -> Services {
    let root = tempfile::tempdir().unwrap().keep();
    let store =
        Arc::new(Store::open(&format!("sqlite:///{}", root.join("entry.db").display())).unwrap());
    store
        .transaction(true, |tx| {
            for statement in CORE {
                tx.execute(statement, vec![])?;
            }
            Ok::<_, asterion_server::StoreError>(())
        })
        .unwrap();
    let identity = Arc::new(
        Identity::new(
            store.clone(),
            SECRET,
            Verification::Local,
            Box::new(Mailer::new(root.clone())),
        )
        .unwrap(),
    );
    Services {
        root,
        store,
        identity,
    }
}

pub fn settings(upstream: &str, require_account: bool) -> Settings {
    Settings {
        upstream: upstream.into(),
        secret: SECRET.into(),
        require_account,
        lease_seconds: 60.0,
        data_root: std::env::temp_dir(),
    }
}

pub fn app(upstream: &str, routes: &str, require_account: bool, services: &Services) -> Router {
    let mut settings = settings(upstream, require_account);
    settings.data_root = services.root.clone();
    application(
        settings,
        RouteTable::parse(routes).unwrap(),
        Authorization::parse(RULES).unwrap(),
        services.store.clone(),
        services.identity.clone(),
    )
    .unwrap()
}

pub async fn serve(app: Router) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    format!("http://{address}")
}

pub async fn send(request: axum::http::Request<Body>) -> (StatusCode, HeaderMap, Bytes) {
    let client = Client::builder(TokioExecutor::new()).build_http::<Body>();
    let response = client.request(request).await.unwrap();
    let (parts, body) = response.into_parts();
    (
        parts.status,
        parts.headers,
        body.collect().await.unwrap().to_bytes(),
    )
}

/// Check `value` against a schema of the published description. Only the
/// keywords the description uses are understood; any other keyword fails.
pub fn conforms(schema: &Value, value: &Value) -> Result<(), String> {
    let description: Value = serde_json::from_str(OPERATIONS).unwrap();
    check(&description, schema, value, "$")
}

/// The 200 response schema of a described operation.
pub fn response(method: &str, path: &str) -> Value {
    let description: Value = serde_json::from_str(OPERATIONS).unwrap();
    let responses = &description["paths"][path][method]["responses"];
    let success = ["200", "201", "202"]
        .into_iter()
        .find(|status| responses.get(*status).is_some())
        .expect("a success response");
    responses[success]["content"]["application/json"]["schema"].clone()
}

fn check(description: &Value, schema: &Value, value: &Value, at: &str) -> Result<(), String> {
    let fail = |why: &str| Err(format!("{at}: {why} ({value})"));
    let object = schema.as_object().ok_or("schema must be an object")?;
    for key in object.keys() {
        if !matches!(
            key.as_str(),
            "$ref"
                | "type"
                | "properties"
                | "required"
                | "additionalProperties"
                | "items"
                | "enum"
                | "const"
                | "anyOf"
                | "maxLength"
                | "minLength"
                | "pattern"
                | "title"
                | "default"
                | "format"
                | "maxItems"
                | "minItems"
                | "maximum"
                | "minimum"
        ) {
            return Err(format!("{at}: unsupported schema keyword {key}"));
        }
    }
    if let Some(reference) = schema["$ref"].as_str() {
        let name = reference
            .strip_prefix("#/components/schemas/")
            .ok_or("bad reference")?;
        return check(
            description,
            &description["components"]["schemas"][name],
            value,
            at,
        );
    }
    if let Some(options) = schema["anyOf"].as_array()
        && !options
            .iter()
            .any(|option| check(description, option, value, at).is_ok())
    {
        return fail("matches no alternative");
    }
    if let Some(kind) = schema["type"].as_str() {
        let ok = match kind {
            "object" => value.is_object(),
            "array" => value.is_array(),
            "string" => value.is_string(),
            "integer" => value.is_i64() || value.is_u64(),
            "number" => value.is_number(),
            "boolean" => value.is_boolean(),
            "null" => value.is_null(),
            _ => return fail("unknown type"),
        };
        if !ok {
            return fail(&format!("expected {kind}"));
        }
    }
    if let Some(allowed) = schema["enum"].as_array()
        && !allowed.contains(value)
    {
        return fail("not an allowed value");
    }
    if let Some(constant) = schema.get("const")
        && constant != value
    {
        return fail("not the constant");
    }
    if let Some(text) = value.as_str() {
        let length = text.chars().count() as u64;
        if schema["maxLength"].as_u64().is_some_and(|max| length > max)
            || schema["minLength"].as_u64().is_some_and(|min| length < min)
        {
            return fail("length out of range");
        }
    }
    if let Some(number) = value.as_f64()
        && (schema["maximum"].as_f64().is_some_and(|max| number > max)
            || schema["minimum"].as_f64().is_some_and(|min| number < min))
    {
        return fail("number out of range");
    }
    if let Some(values) = value.as_array() {
        let count = values.len() as u64;
        if schema["maxItems"].as_u64().is_some_and(|max| count > max)
            || schema["minItems"].as_u64().is_some_and(|min| count < min)
        {
            return fail("item count out of range");
        }
    }
    if let (Some(items), Some(values)) = (schema.get("items"), value.as_array()) {
        for (index, item) in values.iter().enumerate() {
            check(description, items, item, &format!("{at}[{index}]"))?;
        }
    }
    if let Some(fields) = value.as_object() {
        let properties = schema["properties"].as_object();
        for name in schema["required"].as_array().into_iter().flatten() {
            if !fields.contains_key(name.as_str().unwrap()) {
                return fail(&format!("missing {name}"));
            }
        }
        for (name, field) in fields {
            match properties.and_then(|properties| properties.get(name)) {
                Some(property) => check(description, property, field, &format!("{at}.{name}"))?,
                None if schema["additionalProperties"] == Value::Bool(true) => {}
                None if properties.is_some() => return fail(&format!("undeclared field {name}")),
                None => {}
            }
        }
    }
    Ok(())
}
