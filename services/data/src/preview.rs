//! Checksum-verified reading of a published version's rows for display and for
//! consumers that need complete small tables (contracts, calendars, mappings).
use crate::catalog::public_version;
use asterion_data_store::table::{Row, batch_rows, csv_rows};
use asterion_data_store::{PartitionedVersion, read_index, read_partition};
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::files;
use asterion_store::{StoreError, Transaction};
use bytes::Bytes;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Debug)]
pub enum PreviewError {
    NotFound,
    /// The version exists but cannot be read under the current contract.
    Refused(String),
    Store(StoreError),
}
impl From<StoreError> for PreviewError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
impl From<String> for PreviewError {
    fn from(message: String) -> Self {
        Self::Refused(message)
    }
}
impl From<&str> for PreviewError {
    fn from(message: &str) -> Self {
        Self::Refused(message.into())
    }
}
type Result<T> = std::result::Result<T, PreviewError>;

#[derive(Debug, Serialize)]
pub struct Preview {
    pub version: Value,
    pub rows: Vec<Row>,
    pub offset: usize,
    pub total: Value,
    pub snapshot: Value,
    /// Per-row observation time and raw version of cumulative versions.
    pub row_sources: Option<Vec<Value>>,
}

const VERSION: [&str; 6] = [
    "id",
    "dataset_id",
    "job_id",
    "created_at",
    "rows",
    "manifest",
];

fn artifact(store: &ArtifactStore, manifest: &Value) -> Result<Vec<u8>> {
    let path = manifest["path"]
        .as_str()
        .ok_or("数据版本清单不符合当前契约")?;
    let checksum = manifest["checksum"]
        .as_str()
        .ok_or("数据版本清单不符合当前契约")?;
    let size = manifest["bytes"]
        .as_u64()
        .ok_or("数据版本清单不符合当前契约")?;
    store
        .read(path, checksum, Some(size))
        .map_err(|error| match error {
            files::Error::Io(error) if error.kind() == std::io::ErrorKind::NotFound => {
                PreviewError::Refused("数据文件缺失".into())
            }
            files::Error::Io(_) | files::Error::Published(_) => {
                PreviewError::Store(StoreError::from("数据文件读取失败"))
            }
            files::Error::Invalid(_) => PreviewError::Refused("数据文件校验和不一致".into()),
        })
}

#[derive(Deserialize)]
struct EvidencePart {
    rows: Vec<Row>,
}

/// Rows `offset..offset + limit` of a version, with its public record and chart snapshot.
pub fn preview(
    tx: &Transaction,
    store: &ArtifactStore,
    version_id: &str,
    offset: usize,
    limit: usize,
) -> Result<Preview> {
    let record = tx
        .rows(
            "SELECT id, dataset_id, job_id, created_at, rows, manifest FROM data_versions \
             WHERE id = ?",
            vec![json!(version_id)],
        )?
        .into_iter()
        .next()
        .ok_or(PreviewError::NotFound)?;
    let manifest: Value = match &record[5] {
        Value::String(text) => {
            serde_json::from_str(text).map_err(|_| "数据版本清单不符合当前契约")?
        }
        other => other.clone(),
    };
    let snapshot = match manifest["snapshot_id"].as_str() {
        Some(snapshot_id) => tx
            .rows(
                "SELECT id, job_id, manifest FROM snapshots WHERE id = ?",
                vec![json!(snapshot_id)],
            )?
            .into_iter()
            .next()
            .map(|row| {
                let chart = match &row[2] {
                    Value::String(text) => serde_json::from_str(text).unwrap_or(Value::Null),
                    other => other.clone(),
                };
                json!({"id": row[0], "job_id": row[1], "manifest": chart})
            })
            .unwrap_or(Value::Null),
        None => Value::Null,
    };
    let total = record[4].clone();
    let version =
        public_version(&VERSION, record).map_err(|e| PreviewError::Refused(e.to_string()))?;
    let format = manifest["format"].as_str().unwrap_or_default();
    let (rows, row_sources) = match format {
        "partition_manifest" => {
            let index: PartitionedVersion = serde_json::from_value(json!({
                "path": manifest["path"], "checksum": manifest["checksum"],
                "bytes": manifest["bytes"], "partitions": manifest["partitions"],
            }))
            .map_err(|_| "版本分区记录不符合当前契约")?;
            let (mut selected, mut cursor) = (Vec::new(), 0usize);
            for part in read_index(store, &index)? {
                let count = usize::try_from(part.rows).map_err(|_| "版本分区记录不符合当前契约")?;
                if cursor < offset + limit && cursor + count > offset {
                    let batch = read_partition(store, &part)?;
                    let start = offset.saturating_sub(cursor);
                    selected.extend(batch_rows(&batch, start, offset + limit - cursor - start)?);
                }
                cursor += count;
            }
            let sources = selected
                .iter()
                .map(|row| {
                    json!({"observed_at": row.get("_observed_at"),
                           "raw_version_id": row.get("_raw_version_id")})
                })
                .collect();
            let rows = selected
                .into_iter()
                .map(|row| {
                    Row(row
                        .0
                        .into_iter()
                        .filter(|(k, _)| !k.starts_with('_'))
                        .collect())
                })
                .collect();
            (rows, Some(sources))
        }
        "parquet" => {
            let content = artifact(store, &manifest)?;
            let reader = ParquetRecordBatchReaderBuilder::try_new(Bytes::from(content))
                .and_then(|builder| builder.build())
                .map_err(|_| "数据文件无法解析")?;
            let (mut rows, mut skip) = (Vec::new(), offset);
            for batch in reader {
                let batch = batch.map_err(|_| "数据文件无法解析")?;
                if rows.len() >= limit {
                    break;
                }
                let taken = batch_rows(&batch, skip, limit - rows.len())?;
                skip = skip.saturating_sub(batch.num_rows());
                rows.extend(taken);
            }
            (rows, None)
        }
        "provider_evidence" => {
            let content = artifact(store, &manifest)?;
            let parts: Vec<EvidencePart> =
                serde_json::from_slice(&content).map_err(|_| "来源证据不符合当前契约")?;
            let rows = parts
                .into_iter()
                .flat_map(|part| part.rows)
                .skip(offset)
                .take(limit)
                .collect();
            (rows, None)
        }
        "csv" => {
            let content = artifact(store, &manifest)?;
            let text = String::from_utf8(content).map_err(|_| "CSV 文件不是 UTF-8 文本")?;
            (
                csv_rows(&text)?
                    .into_iter()
                    .skip(offset)
                    .take(limit)
                    .collect(),
                None,
            )
        }
        _ => return Err("该数据格式尚未安装预览器".into()),
    };
    Ok(Preview {
        version,
        rows,
        offset,
        total,
        snapshot,
        row_sources,
    })
}
