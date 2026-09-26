//! Published chart snapshots: their public manifests and chart bars. A chart
//! is either a stored bar table or a projection of a fixed daily version.
use crate::preview::{PreviewError, preview};
use asterion_data_store::bars::{daily_chart, read_bars};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::files;
use asterion_store::{StoreError, Transaction};
use serde_json::{Map, Value, json};

#[derive(Debug)]
pub enum SnapshotError {
    NotFound,
    Refused(String),
    Store(StoreError),
}
impl From<StoreError> for SnapshotError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
impl From<String> for SnapshotError {
    fn from(message: String) -> Self {
        Self::Refused(message)
    }
}
impl From<&str> for SnapshotError {
    fn from(message: &str) -> Self {
        Self::Refused(message.into())
    }
}
type Result<T> = std::result::Result<T, SnapshotError>;

fn document(value: &Value) -> Result<Value> {
    match value {
        Value::String(text) => {
            serde_json::from_str(text).map_err(|_| "图表清单不符合当前契约".into())
        }
        other => Ok(other.clone()),
    }
}

/// The public fields of a snapshot manifest, with their defaults.
fn public(manifest: &Value) -> Result<Value> {
    let required = [
        "storage",
        "schema_version",
        "rows",
        "checksum",
        "contracts",
        "start",
        "end",
        "uri",
        "source",
        "state",
    ];
    if required.iter().any(|key| manifest.get(*key).is_none())
        || !matches!(manifest["storage"].as_str(), Some("parquet" | "version"))
        || manifest["state"] != "PUBLISHED"
    {
        return Err("图表清单不符合当前契约".into());
    }
    let mut out = Map::new();
    for key in [
        "storage",
        "version_id",
        "demo",
        "frequency",
        "time_semantics",
        "dataset_id",
        "schema_version",
        "rows",
        "checksum",
        "contracts",
        "start",
        "end",
        "uri",
        "source",
        "state",
    ] {
        let default = if key == "demo" {
            json!(false)
        } else {
            Value::Null
        };
        out.insert(key.into(), manifest.get(key).cloned().unwrap_or(default));
    }
    Ok(Value::Object(out))
}

/// Up to 100 published snapshots.
pub fn list(tx: &Transaction) -> Result<Vec<Value>> {
    tx.rows(
        "SELECT id, job_id, manifest FROM snapshots LIMIT 100",
        vec![],
    )?
    .into_iter()
    .map(|row| {
        Ok(json!({"id": row[0], "job_id": row[1], "manifest": public(&document(&row[2])?)?}))
    })
    .collect()
}

/// Up to `limit` chart bars of a snapshot.
pub fn bars(
    tx: &Transaction,
    store: &ArtifactStore,
    snapshot_id: &str,
    limit: usize,
) -> Result<Vec<Value>> {
    let rows = tx.rows(
        "SELECT manifest FROM snapshots WHERE id = ?",
        vec![json!(snapshot_id)],
    )?;
    let manifest = public(&document(&rows.first().ok_or(SnapshotError::NotFound)?[0])?)?;
    if manifest["storage"] == "parquet" {
        let checksum = manifest["checksum"].as_str().unwrap_or_default();
        let content = store
            .read(&format!("published/{snapshot_id}.parquet"), checksum, None)
            .map_err(|error| match error {
                files::Error::Io(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    SnapshotError::Refused("图表文件缺失".into())
                }
                files::Error::Invalid(_) => SnapshotError::Refused("图表文件校验和不一致".into()),
                other => SnapshotError::Store(StoreError::from(other.to_string())),
            })?;
        return Ok(read_bars(content, limit)?);
    }
    let version_id = manifest["version_id"]
        .as_str()
        .ok_or("图表投影缺少固定版本")?;
    let fixed = preview(tx, store, version_id, 0, limit).map_err(|error| match error {
        PreviewError::NotFound => SnapshotError::Refused("图表投影与固定版本不一致".into()),
        PreviewError::Refused(message) => SnapshotError::Refused(message),
        PreviewError::Store(error) => SnapshotError::Store(error),
    })?;
    let version = &fixed.version;
    if version["manifest"]["checksum"] != manifest["checksum"]
        || version["manifest"]["type"]["id"] != "futures.daily"
        || version["manifest"]["format"] != "partition_manifest"
        || version["rows"] != manifest["rows"]
    {
        return Err("图表投影与固定版本不一致".into());
    }
    let sources = fixed.row_sources.unwrap_or_default();
    if sources.len() != fixed.rows.len() {
        return Err("图表投影与固定版本不一致".into());
    }
    let values: Vec<Value> = fixed
        .rows
        .iter()
        .zip(&sources)
        .map(|(row, source)| {
            let mut value: Map<String, Value> = row.0.iter().cloned().collect();
            value.insert("_observed_at".into(), source["observed_at"].clone());
            Value::Object(value)
        })
        .collect();
    let observed = version["manifest"]["observed_at"]
        .as_str()
        .ok_or("固定版本缺少采集时间")?;
    let (content, _) = daily_chart(&values, observed)?;
    Ok(read_bars(content, limit)?)
}
