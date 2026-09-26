//! Named connection instances of built-in data sources, their lifecycle and
//! the source listing with configuration and verification status. Each
//! built-in source is also its own default connection, identified by its id.
use crate::configuration::{self, Result, SourceError, VerificationState};
use crate::credentials::Credentials;
use asterion_data_store::provider::{ConfigurationSpec, ProviderManifest};
use asterion_store::Transaction;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};

#[derive(Debug, Clone, Serialize)]
pub struct Connection {
    pub id: String,
    pub provider: String,
    pub name: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct ConnectionState {
    pub id: String,
    pub provider: String,
    pub name: String,
    pub state: String,
    pub revision: i64,
}

#[derive(Debug, Deserialize)]
pub struct NewConnection {
    pub provider: String,
    pub name: String,
}

#[derive(Debug, Deserialize)]
pub struct ConnectionUpdate {
    pub expected_revision: i64,
    pub name: String,
    pub state: String,
}
impl ConnectionUpdate {
    pub fn valid(&self) -> bool {
        self.expected_revision >= 0
            && (1..=80).contains(&self.name.chars().count())
            && ["enabled", "disabled", "archived"].contains(&self.state.as_str())
    }
}

const CONFLICT: &str = "连接已在其他窗口更新，请刷新后重试";

fn manifest<'a>(sources: &'a [ProviderManifest], id: &str) -> Result<&'a ProviderManifest> {
    sources
        .iter()
        .find(|source| source.id == id)
        .ok_or_else(|| "不支持该数据源；数据源只能是内置实现".into())
}

/// Python's `str.strip()` of a display name.
fn stripped(name: &str) -> &str {
    name.trim_matches(|c: char| c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c))
}

pub fn all(tx: &Transaction) -> Result<Vec<Connection>> {
    Ok(tx
        .rows("SELECT id, provider, name FROM data_connections", vec![])?
        .into_iter()
        .map(|row| Connection {
            id: row[0].as_str().unwrap_or_default().into(),
            provider: row[1].as_str().unwrap_or_default().into(),
            name: row[2].as_str().unwrap_or_default().into(),
        })
        .collect())
}

pub fn resolve(
    tx: &Transaction,
    sources: &[ProviderManifest],
    identifier: &str,
) -> Result<Connection> {
    if let Some(source) = sources.iter().find(|source| source.id == identifier) {
        return Ok(Connection {
            id: source.id.clone(),
            provider: source.id.clone(),
            name: source.name.clone(),
        });
    }
    all(tx)?
        .into_iter()
        .find(|connection| connection.id == identifier)
        .ok_or_else(|| "连接实例不存在".into())
}

/// The configuration declaration of a connection's source.
pub fn spec(
    tx: &Transaction,
    sources: &[ProviderManifest],
    identifier: &str,
) -> Result<ConfigurationSpec> {
    let connection = resolve(tx, sources, identifier)?;
    Ok(manifest(sources, &connection.provider)?
        .configuration
        .clone())
}

pub fn create(
    tx: &Transaction,
    sources: &[ProviderManifest],
    body: &NewConnection,
) -> Result<Connection> {
    manifest(sources, &body.provider)?;
    let name = stripped(&body.name);
    if name.is_empty() {
        return Err("连接名称不能为空".into());
    }
    let record = Connection {
        id: format!("c_{}", uuid::Uuid::new_v4().simple()),
        provider: body.provider.clone(),
        name: name.into(),
    };
    tx.execute(
        "INSERT INTO data_connections (id, provider, name) VALUES (?, ?, ?)",
        vec![json!(record.id), json!(record.provider), json!(record.name)],
    )?;
    Ok(record)
}

pub fn state(
    tx: &Transaction,
    sources: &[ProviderManifest],
    identifier: &str,
) -> Result<ConnectionState> {
    let base = resolve(tx, sources, identifier)?;
    let saved = tx.rows(
        "SELECT name, state, revision FROM data_connection_settings WHERE id = ?",
        vec![json!(identifier)],
    )?;
    Ok(match saved.into_iter().next() {
        Some(row) => ConnectionState {
            id: base.id,
            provider: base.provider,
            name: row[0].as_str().unwrap_or_default().into(),
            state: row[1].as_str().unwrap_or_default().into(),
            revision: row[2].as_i64().unwrap_or_default(),
        },
        None => ConnectionState {
            id: base.id,
            provider: base.provider,
            name: base.name,
            state: "enabled".into(),
            revision: 0,
        },
    })
}

/// Rename, disable, archive or restore; the expected revision must be current.
pub fn update(
    tx: &Transaction,
    sources: &[ProviderManifest],
    identifier: &str,
    body: &ConnectionUpdate,
) -> Result<ConnectionState> {
    resolve(tx, sources, identifier)?;
    let name = stripped(&body.name);
    if name.is_empty() {
        return Err("连接名称不能为空".into());
    }
    let revision = body.expected_revision + 1;
    let changed = if body.expected_revision == 0 {
        tx.execute(
            "INSERT INTO data_connection_settings (id, name, state, revision) VALUES (?, ?, ?, ?)",
            vec![
                json!(identifier),
                json!(name),
                json!(body.state),
                json!(revision),
            ],
        )
    } else {
        tx.execute(
            "UPDATE data_connection_settings SET name = ?, state = ?, revision = ? WHERE id = ? AND revision = ?",
            vec![json!(name), json!(body.state), json!(revision), json!(identifier), json!(body.expected_revision)],
        )
    };
    match changed {
        Ok(1) => state(tx, sources, identifier),
        Ok(_) => Err(SourceError::Conflict(CONFLICT.into())),
        Err(error) if error.integrity => Err(SourceError::Conflict(CONFLICT.into())),
        Err(error) => Err(error.into()),
    }
}

#[derive(Debug, Serialize)]
pub struct SourceStatus {
    #[serde(flatten)]
    pub manifest: ProviderManifest,
    pub lifecycle: ConnectionState,
    pub verification: VerificationState,
    pub plugin_id: Option<String>,
    pub connection_id: Option<String>,
    pub configured: bool,
    pub credential_error: Option<String>,
}

/// Every built-in source and connection instance with its lifecycle,
/// configuration completeness and latest verification.
pub fn listing(
    tx: &Transaction,
    credentials: &Credentials,
    sources: &[ProviderManifest],
) -> Result<Vec<SourceStatus>> {
    let mut items: Vec<(ProviderManifest, Option<String>)> = sources
        .iter()
        .map(|source| (source.clone(), None))
        .collect();
    for connection in all(tx)? {
        if let Ok(source) = manifest(sources, &connection.provider) {
            let mut instance = source.clone();
            instance.id = connection.id.clone();
            instance.name = connection.name;
            items.push((instance, Some(connection.provider)));
        }
    }
    items
        .into_iter()
        .map(|(mut manifest, plugin)| {
            let configuration =
                configuration::state(tx, credentials, &manifest.id, &manifest.configuration);
            let (configured, credential_error) = match configuration {
                Ok(state) => (state.configured, state.error),
                Err(SourceError::Refused(message)) => (false, Some(message)),
                Err(error) => return Err(error),
            };
            let lifecycle = state(tx, sources, &manifest.id)?;
            manifest.name = lifecycle.name.clone();
            Ok(SourceStatus {
                verification: configuration::verification(tx, &manifest.id)?,
                connection_id: plugin.is_some().then(|| manifest.id.clone()),
                plugin_id: plugin,
                lifecycle,
                manifest,
                configured,
                credential_error,
            })
        })
        .collect()
}

/// Configuration fixed for a new sync task of an enabled connection.
pub fn fix_for_task(
    tx: &Transaction,
    credentials: &Credentials,
    sources: &[ProviderManifest],
    owner: &str,
    provider: &str,
) -> Result<Map<String, Value>> {
    let lifecycle = state(tx, sources, owner)?;
    if lifecycle.state != "enabled" {
        return Err("连接已停用或归档，不能提交新的同步任务".into());
    }
    if lifecycle.provider != provider {
        return Err("连接实例与供应商插件不匹配".into());
    }
    let spec = manifest(sources, provider)?.configuration.clone();
    let fixed = configuration::freeze(tx, credentials, owner, &spec)?;
    let mut result = Map::new();
    result.insert(
        "configuration".into(),
        serde_json::to_value(fixed).expect("fixed"),
    );
    result.insert("connection_name".into(), json!(lifecycle.name));
    Ok(result)
}
