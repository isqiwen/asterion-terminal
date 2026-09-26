//! Revisioned, schema-validated data source configurations. Secret values
//! never leave safe views; saved values live only in encrypted snapshots.
use crate::credentials::Credentials;
use asterion_data_store::provider::{ConfigurationField, ConfigurationSpec};
use asterion_store::{StoreError, Transaction};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};

#[derive(Debug)]
pub enum SourceError {
    /// Input or state the service refuses, with the user-facing reason.
    Refused(String),
    /// A concurrent change; the caller refreshes and retries.
    Conflict(String),
    Store(StoreError),
}
impl From<StoreError> for SourceError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
impl From<String> for SourceError {
    fn from(message: String) -> Self {
        Self::Refused(message)
    }
}
impl From<&str> for SourceError {
    fn from(message: &str) -> Self {
        Self::Refused(message.into())
    }
}

pub type Result<T> = std::result::Result<T, SourceError>;

/// Python's `str.isspace` also counts the ASCII information separators.
fn space(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// Validate the deliberately small declarative scalar schema without coercion.
pub fn validate(
    spec: &ConfigurationSpec,
    values: &Map<String, Value>,
    required: bool,
) -> Result<Map<String, Value>> {
    if values
        .keys()
        .any(|key| !spec.fields.iter().any(|field| &field.id == key))
    {
        return Err("配置包含未声明字段".into());
    }
    let mut result = Map::new();
    for field in &spec.fields {
        let value = match values.get(&field.id) {
            None | Some(Value::Null) => &field.default,
            Some(value) => value,
        };
        if value.is_null() || (field.kind == "string" && value == "") {
            if required && field.required {
                return Err(format!("请先配置 {}", field.label).into());
            }
            continue;
        }
        let typed = match field.kind.as_str() {
            "string" => value.is_string(),
            "integer" => value.is_i64() || value.is_u64(),
            "boolean" => value.is_boolean(),
            _ => false,
        };
        if !typed {
            return Err(format!("{} 的类型不正确", field.label).into());
        }
        if let Some(text) = value.as_str() {
            let length = text.chars().count() as u64;
            let max = field.max_length.filter(|max| *max != 0).unwrap_or(4096);
            if length > max || length < field.min_length.unwrap_or(0) {
                return Err(format!("{} 的长度不符合要求", field.label).into());
            }
            if field.secret && (text.trim_matches(space) != text || text.chars().any(space)) {
                return Err(format!("{} 格式不正确", field.label).into());
            }
        }
        let number = value
            .as_i64()
            .map(i128::from)
            .or(value.as_u64().map(i128::from));
        if let Some(number) = number
            && (field.minimum.is_some_and(|min| number < i128::from(min))
                || field.maximum.is_some_and(|max| number > i128::from(max)))
        {
            return Err(format!("{} 超出允许范围", field.label).into());
        }
        result.insert(field.id.clone(), value.clone());
    }
    Ok(result)
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ConfigurationUpdate {
    pub expected_revision: i64,
    #[serde(default)]
    pub values: Map<String, Value>,
    #[serde(default)]
    pub secrets: Map<String, Value>,
}
impl ConfigurationUpdate {
    /// Request shape: bounded maps, secrets are strings or null.
    pub fn valid(&self) -> bool {
        self.expected_revision >= 0
            && self.values.len() <= 30
            && self.secrets.len() <= 30
            && self
                .secrets
                .values()
                .all(|value| value.is_string() || value.is_null())
    }
}

#[derive(Debug, Serialize)]
pub struct ConfigurationState {
    pub provider: String,
    pub revision: i64,
    pub schema_version: u32,
    pub values: Map<String, Value>,
    pub secret_fields: Vec<String>,
    pub configured: bool,
    pub error: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct ConfigurationCheck {
    pub status: &'static str,
    pub message: String,
    pub revision: i64,
}

#[derive(Debug, Serialize)]
pub struct VerificationState {
    pub status: &'static str,
    pub checked_at: Option<f64>,
    pub revision: Option<i64>,
    pub message: &'static str,
}

/// The configuration a task is fixed to: never the values themselves.
#[derive(Debug, Serialize)]
pub struct Fixed {
    #[serde(rename = "ref")]
    pub reference: String,
    pub revision: i64,
    pub schema_version: u32,
}

struct Head {
    revision: i64,
    schema_version: i64,
    snapshot: String,
}

fn head(tx: &Transaction, owner: &str) -> Result<Option<Head>> {
    let rows = tx.rows(
        "SELECT revision, schema_version, snapshot_ref FROM data_provider_configurations WHERE provider = ?",
        vec![json!(owner)],
    )?;
    Ok(rows.into_iter().next().map(|row| Head {
        revision: row[0].as_i64().unwrap_or_default(),
        schema_version: row[1].as_i64().unwrap_or_default(),
        snapshot: row[2].as_str().unwrap_or_default().to_string(),
    }))
}

fn revision_of(tx: &Transaction, owner: &str) -> Result<i64> {
    Ok(head(tx, owner)?.map_or(0, |head| head.revision))
}

/// The saved revision and its values (unvalidated for runnability).
pub fn current(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
) -> Result<(i64, Map<String, Value>)> {
    match head(tx, owner)? {
        Some(head) => {
            if head.schema_version != i64::from(spec.schema_version) {
                return Err("配置格式不受支持，请按当前规范重新配置".into());
            }
            let values =
                credentials.read(&head.snapshot, owner, spec.schema_version, head.revision)?;
            Ok((head.revision, validate(spec, &values, false)?))
        }
        None => Ok((0, validate(spec, &Map::new(), false)?)),
    }
}

/// The safe view: ordinary values and which secrets are set, never secrets.
pub fn state(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
) -> Result<ConfigurationState> {
    let (revision, values, error) = match current(tx, credentials, owner, spec) {
        Ok((revision, values)) => (revision, values, None),
        Err(SourceError::Refused(message)) => (
            revision_of(tx, owner)?,
            validate(spec, &Map::new(), false)?,
            Some(message),
        ),
        Err(error) => return Err(error),
    };
    let secret = |key: &String| spec.fields.iter().any(|f| &f.id == key && f.secret);
    let configured = error.is_none() && validate(spec, &values, true).is_ok();
    Ok(ConfigurationState {
        provider: owner.into(),
        revision,
        schema_version: spec.schema_version,
        secret_fields: values.keys().filter(|key| secret(key)).cloned().collect(),
        values: values.into_iter().filter(|(key, _)| !secret(key)).collect(),
        configured,
        error,
    })
}

/// The values an update would save: new ordinary values, previous secrets
/// unless replaced or cleared. Clearing credentials is a valid saved state.
pub fn candidate(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
    update: &ConfigurationUpdate,
) -> Result<Map<String, Value>> {
    let field = |key: &str| spec.fields.iter().find(|f| f.id == key);
    let revision = revision_of(tx, owner)?;
    if revision != update.expected_revision {
        return Err(SourceError::Conflict(
            "配置已在其他窗口更新，请刷新已保存配置后重试".into(),
        ));
    }
    let previous = match current(tx, credentials, owner, spec) {
        Ok((current, _)) if current != revision => {
            return Err(SourceError::Conflict(
                "配置已更新，请刷新已保存配置后重试".into(),
            ));
        }
        Ok((_, previous)) => previous,
        Err(SourceError::Refused(_)) => {
            let complete = spec.fields.iter().all(|f: &ConfigurationField| {
                if f.secret {
                    update.secrets.contains_key(&f.id)
                } else {
                    update.values.contains_key(&f.id)
                }
            });
            if !complete {
                return Err(
                    "原配置无法读取，请重新填写全部配置和凭据；不能保留不可读的旧凭据".into(),
                );
            }
            Map::new()
        }
        Err(error) => return Err(error),
    };
    if update
        .values
        .keys()
        .any(|key| field(key).is_none_or(|f| f.secret))
    {
        return Err("普通配置包含未声明字段或秘密字段".into());
    }
    if update
        .secrets
        .keys()
        .any(|key| field(key).is_none_or(|f| !f.secret))
    {
        return Err("凭据配置包含未声明字段".into());
    }
    let mut values: Map<String, Value> = previous
        .into_iter()
        .filter(|(key, _)| field(key).is_some_and(|f| f.secret))
        .collect();
    values.extend(update.values.clone());
    for (key, secret) in &update.secrets {
        if secret.is_null() {
            values.remove(key);
        } else {
            values.insert(key.clone(), secret.clone());
        }
    }
    validate(spec, &values, false)
}

/// Save a new revision; the expected revision must still be current.
pub fn apply(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
    update: &ConfigurationUpdate,
) -> Result<ConfigurationState> {
    let values = candidate(tx, credentials, owner, spec, update)?;
    let revision = update.expected_revision + 1;
    let reference = credentials.freeze(owner, spec.schema_version, &values, revision)?;
    let conflict = || SourceError::Conflict("配置已更新，请刷新已保存配置后重试".into());
    let changed = if update.expected_revision == 0 {
        tx.execute(
            "INSERT INTO data_provider_configurations (provider, revision, schema_version, snapshot_ref) VALUES (?, ?, ?, ?)",
            vec![json!(owner), json!(revision), json!(spec.schema_version), json!(reference)],
        )
    } else {
        tx.execute(
            "UPDATE data_provider_configurations SET revision = ?, schema_version = ?, snapshot_ref = ? WHERE provider = ? AND revision = ?",
            vec![json!(revision), json!(spec.schema_version), json!(reference), json!(owner), json!(update.expected_revision)],
        )
    };
    match changed {
        Ok(1) => state(tx, credentials, owner, spec),
        Ok(_) => Err(conflict()),
        Err(error) if error.integrity => Err(conflict()),
        Err(error) => Err(error.into()),
    }
}

/// The runnable values of an unsaved update (a draft check reads only).
pub fn draft(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
    update: &ConfigurationUpdate,
) -> Result<Map<String, Value>> {
    let values = candidate(tx, credentials, owner, spec, update)?;
    validate(spec, &values, true)
}

/// A draft check result, if the saved revision did not change while probing.
pub fn checked(
    tx: &Transaction,
    owner: &str,
    expected: i64,
    message: String,
) -> Result<ConfigurationCheck> {
    if revision_of(tx, owner)? != expected {
        return Err(SourceError::Conflict(
            "测试期间配置已更新，请刷新后重试".into(),
        ));
    }
    Ok(ConfigurationCheck {
        status: "verified",
        message,
        revision: expected,
    })
}

/// Fix the current runnable configuration for a task.
pub fn freeze(
    tx: &Transaction,
    credentials: &Credentials,
    owner: &str,
    spec: &ConfigurationSpec,
) -> Result<Fixed> {
    let (revision, values) = current(tx, credentials, owner, spec)?;
    let values = validate(spec, &values, true)?;
    let reference = credentials.freeze(owner, spec.schema_version, &values, revision)?;
    Ok(Fixed {
        reference,
        revision,
        schema_version: spec.schema_version,
    })
}

/// The runnable values a task was fixed to; changed or unreadable
/// configurations are refused, never replaced by current settings.
pub fn resolve(
    credentials: &Credentials,
    fixed: &Value,
    owner: &str,
    spec: &ConfigurationSpec,
) -> Result<Map<String, Value>> {
    let invalid = || SourceError::from("固定配置引用无效，请重新提交同步");
    let fixed = fixed.as_object().ok_or_else(invalid)?;
    let keys: Vec<&str> = fixed.keys().map(String::as_str).collect();
    let revision = fixed
        .get("revision")
        .and_then(Value::as_i64)
        .filter(|r| *r >= 0);
    let schema_version = fixed.get("schema_version").and_then(Value::as_i64);
    let reference = fixed.get("ref").ok_or_else(invalid)?;
    let (["ref", "revision", "schema_version"], Some(revision), Some(schema_version)) =
        (keys.as_slice(), revision, schema_version)
    else {
        return Err(invalid());
    };
    if schema_version != i64::from(spec.schema_version) {
        return Err("配置版本已变化，请重新提交同步".into());
    }
    let reference = reference.as_str().ok_or("配置快照引用无效")?;
    let values = credentials.read(reference, owner, spec.schema_version, revision)?;
    validate(spec, &values, true)
}

/// The latest saved-configuration verification, stale once the revision changes.
pub fn verification(tx: &Transaction, owner: &str) -> Result<VerificationState> {
    let revision = revision_of(tx, owner)?;
    let rows = tx.rows(
        "SELECT revision, checked_at, status FROM data_connection_verifications WHERE provider = ?",
        vec![json!(owner)],
    )?;
    let Some(row) = rows.into_iter().next() else {
        return Ok(VerificationState {
            status: "never",
            checked_at: None,
            revision: None,
            message: "尚未验证已保存配置",
        });
    };
    let checked = row[0].as_i64();
    let (status, message) = match row[2].as_str() {
        _ if checked != Some(revision) => ("stale", "配置已修改，之前的验证结果不再适用"),
        Some("verified") => ("verified", "该配置通过连接测试；不代表所有数据接口均有权限"),
        _ => ("failed", "连接验证失败，请检查配置、网络及接口权限"),
    };
    Ok(VerificationState {
        status,
        checked_at: row[1].as_f64(),
        revision: checked,
        message,
    })
}

/// Record a saved-configuration verification; a later-started one wins.
pub fn record_verification(
    tx: &Transaction,
    owner: &str,
    revision: i64,
    started: f64,
    checked: f64,
    verified: bool,
) -> Result<()> {
    tx.execute(
        "INSERT INTO data_connection_verifications (provider, revision, started_at, checked_at, status) VALUES (?, ?, ?, ?, ?) \
         ON CONFLICT (provider) DO UPDATE SET revision = excluded.revision, started_at = excluded.started_at, \
         checked_at = excluded.checked_at, status = excluded.status \
         WHERE data_connection_verifications.started_at <= excluded.started_at",
        vec![
            json!(owner),
            json!(revision),
            json!(started),
            json!(checked),
            json!(if verified { "verified" } else { "failed" }),
        ],
    )?;
    Ok(())
}
