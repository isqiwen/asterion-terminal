//! Partitioned cumulative versions: index verification and whole-partition
//! Parquet reads and writes. Bytes pass through the L1 write-once artifact grant;
//! partition meaning, cell types and index structure belong to this plugin.
use crate::Result;
use arrow_array::{
    Array, ArrayRef, Int64Array, NullArray, RecordBatch, StringArray,
    cast::AsArray,
    types::{Int8Type, Int16Type, Int32Type, Int64Type},
};
use arrow_schema::{DataType, Field, Schema};
use arrow_select::concat::concat_batches;
use asterion_kernel::artifacts::{ArtifactRef, ArtifactStore};
use asterion_kernel::files;
use bytes::Bytes;
use parquet::{
    arrow::{ArrowWriter, arrow_reader::ParquetRecordBatchReaderBuilder},
    basic::Compression,
    file::properties::WriterProperties,
};
use schemars::JsonSchema;
use serde::{Deserialize, Deserializer, Serialize};
use std::collections::BTreeSet;
use std::sync::Arc;

pub(crate) const PARTITION_DIRECTORY: &str = "artifacts";
pub(crate) const INDEX_BYTES: u64 = 16 * 1024 * 1024;
pub(crate) const PARTITION_BYTES: u64 = 512 * 1024 * 1024;
pub(crate) const MAX_PARTITIONS: usize = 10_000;
pub(crate) const MAX_COLUMNS: usize = 128;
pub(crate) const MAX_VALUE_BYTES: usize = 64 * 1024;
pub(crate) const MAX_ROWS: i64 = 100_000_000;
pub(crate) const OBSERVED: &str = "_observed_at";
pub(crate) const RAW_VERSION: &str = "_raw_version_id";

pub(crate) fn required_option<'de, D: Deserializer<'de>, T: Deserialize<'de>>(
    deserializer: D,
) -> std::result::Result<Option<T>, D::Error> {
    Option::<T>::deserialize(deserializer)
}

/// A stored scalar. Current partitions hold strings (dates, decimals and
/// identifiers), integers and nulls; any other value is rejected, not coerced.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum Cell {
    Null,
    Int(i64),
    Text(String),
}
impl Cell {
    pub(crate) fn text(&self) -> Option<&str> {
        match self {
            Self::Text(value) => Some(value),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Partition {
    #[schemars(length(min = 1, max = 256))]
    pub key: String,
    pub checksum: String,
    #[schemars(range(min = 0))]
    pub rows: i64,
    #[schemars(range(min = 1))]
    pub bytes: u64,
    pub first: String,
    pub last: String,
    pub inputs: Vec<String>,
    #[serde(deserialize_with = "required_option")]
    pub replaces: Option<String>,
}

pub(crate) fn checksum(value: &str) -> Result<()> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    {
        return Err("分区校验和格式不合法".into());
    }
    Ok(())
}

impl Partition {
    pub fn artifact_name(&self) -> String {
        format!("{PARTITION_DIRECTORY}/{}.parquet", self.checksum)
    }
    pub fn validate(&self) -> Result<()> {
        checksum(&self.checksum)?;
        if let Some(replaced) = &self.replaces {
            checksum(replaced)?;
        }
        if self.rows < 0
            || self.rows > MAX_ROWS
            || self.bytes == 0
            || self.bytes > PARTITION_BYTES
            || self.first.is_empty()
            || self.first.len() > 64
            || self.last.len() > 64
            || self.first > self.last
            || self.key.is_empty()
            || self.key.len() > 256
            || self.inputs.len() > 100_000
            || self.inputs.iter().any(|s| s.is_empty() || s.len() > 256)
        {
            return Err("分区范围、行数或标识不合法".into());
        }
        Ok(())
    }
}

pub(crate) fn check_partitions(parts: &[Partition]) -> Result<i64> {
    if parts.len() > MAX_PARTITIONS {
        return Err("分区数量超出预算".into());
    }
    let mut keys = BTreeSet::new();
    let mut rows = 0_i64;
    for part in parts {
        part.validate()?;
        if !keys.insert(part.key.as_str()) {
            return Err("分区键重复".into());
        }
        rows = rows.checked_add(part.rows).ok_or("分区行数溢出")?;
    }
    Ok(rows)
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Index {
    schema_version: u32,
    partitions: Vec<Partition>,
}

/// Parse an index already read from its recorded artifact and require it to
/// equal the partitions recorded in the version manifest.
pub(crate) fn parse_index(content: &[u8], expected: &[Partition]) -> Result<Vec<Partition>> {
    let value = asterion_foundation::communication::parse_json(content)?;
    let index: Index = serde_json::from_value(value).map_err(|_| "分区清单结构不合法")?;
    if index.schema_version != 1 || index.partitions != expected {
        return Err("分区清单与版本记录不一致".into());
    }
    check_partitions(&index.partitions)?;
    Ok(index.partitions)
}

pub(crate) fn encode_index(partitions: &[Partition]) -> Result<Vec<u8>> {
    let value = serde_json::to_value(Index {
        schema_version: 1,
        partitions: partitions.to_vec(),
    })
    .map_err(|e| e.to_string())?;
    asterion_foundation::canonical(&value)
}

pub(crate) fn artifact_error(error: files::Error, missing: &str, changed: &str) -> String {
    match error {
        files::Error::Io(e) if e.kind() == std::io::ErrorKind::NotFound => missing.into(),
        files::Error::Invalid(_) => changed.into(),
        e => e.to_string(),
    }
}

/// The recorded location and content of a partitioned version's index.
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PartitionedVersion {
    pub path: String,
    pub checksum: String,
    #[schemars(range(min = 1))]
    pub bytes: u64,
    pub partitions: Vec<Partition>,
}

pub fn read_index(store: &ArtifactStore, version: &PartitionedVersion) -> Result<Vec<Partition>> {
    checksum(&version.checksum)?;
    if version.bytes > INDEX_BYTES {
        return Err("分区清单超出预算".into());
    }
    let content = store
        .read(&version.path, &version.checksum, Some(version.bytes))
        .map_err(|e| artifact_error(e, "分区清单缺失", "分区清单校验和不一致"))?;
    parse_index(&content, &version.partitions)
}

/// Verify the index and every partition's recorded bytes without decoding.
pub fn verify_version(
    store: &ArtifactStore,
    version: &PartitionedVersion,
) -> Result<Vec<Partition>> {
    let parts = read_index(store, version)?;
    for part in &parts {
        store
            .verify(&part.artifact_name(), &part.checksum, Some(part.bytes))
            .map_err(|e| artifact_error(e, "分区文件缺失", "分区文件校验和不一致"))?;
    }
    Ok(parts)
}

fn allowed(data_type: &DataType) -> bool {
    matches!(
        data_type,
        DataType::Null
            | DataType::Int8
            | DataType::Int16
            | DataType::Int32
            | DataType::Int64
            | DataType::Utf8
            | DataType::LargeUtf8
            | DataType::Utf8View
    )
}

/// Decode one complete, checksum-verified partition as a single batch.
pub fn read_partition(store: &ArtifactStore, part: &Partition) -> Result<RecordBatch> {
    part.validate()?;
    let content = store
        .read(&part.artifact_name(), &part.checksum, Some(part.bytes))
        .map_err(|e| artifact_error(e, "分区文件缺失", "分区文件校验和不一致"))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(Bytes::from(content))
        .map_err(|_| "分区 Parquet 文件无法解析")?;
    let schema = builder.schema().clone();
    if schema.fields().len() > MAX_COLUMNS
        || schema
            .fields()
            .iter()
            .any(|field| !allowed(field.data_type()))
    {
        return Err("分区字段数量或类型不受支持".into());
    }
    let names: BTreeSet<_> = schema.fields().iter().map(|f| f.name()).collect();
    if names.len() != schema.fields().len() {
        return Err("分区字段重复".into());
    }
    if builder.metadata().file_metadata().num_rows() != part.rows {
        return Err("分区文件行数不一致".into());
    }
    let batches = builder
        .with_batch_size(8192)
        .build()
        .map_err(|_| "分区 Parquet 文件无法解析")?
        .collect::<std::result::Result<Vec<_>, _>>()
        .map_err(|_| "分区 Parquet 文件解码失败")?;
    let batch = concat_batches(&schema, &batches).map_err(|e| e.to_string())?;
    if batch.num_rows() as i64 != part.rows {
        return Err("分区文件行数不一致".into());
    }
    Ok(batch)
}

/// Columns of a decoded partition as cells, in file column order.
pub(crate) fn cells(batch: &RecordBatch) -> Result<(Vec<String>, Vec<Vec<Cell>>)> {
    let names: Vec<String> = batch
        .schema()
        .fields()
        .iter()
        .map(|f| f.name().clone())
        .collect();
    let mut rows = vec![Vec::with_capacity(names.len()); batch.num_rows()];
    for column in batch.columns() {
        let value = |row: usize| -> Result<Cell> {
            if column.is_null(row) {
                return Ok(Cell::Null);
            }
            Ok(match column.data_type() {
                DataType::Null => Cell::Null,
                DataType::Int8 => Cell::Int(column.as_primitive::<Int8Type>().value(row).into()),
                DataType::Int16 => Cell::Int(column.as_primitive::<Int16Type>().value(row).into()),
                DataType::Int32 => Cell::Int(column.as_primitive::<Int32Type>().value(row).into()),
                DataType::Int64 => Cell::Int(column.as_primitive::<Int64Type>().value(row)),
                DataType::Utf8 => Cell::Text(column.as_string::<i32>().value(row).into()),
                DataType::LargeUtf8 => Cell::Text(column.as_string::<i64>().value(row).into()),
                DataType::Utf8View => Cell::Text(column.as_string_view().value(row).into()),
                _ => return Err("分区字段类型不受支持".into()),
            })
        };
        for (index, row) in rows.iter_mut().enumerate() {
            row.push(value(index)?);
        }
    }
    Ok((names, rows))
}

/// Encode rows with per-column types inferred from the values: all-null
/// columns are Null, integer columns Int64 and text columns Utf8. Mixed kinds
/// in one column are rejected. Snappy matches the scanner's accepted codecs.
pub(crate) fn encode(columns: &[String], rows: &[Vec<Cell>]) -> Result<Vec<u8>> {
    let mut fields = Vec::with_capacity(columns.len());
    let mut arrays: Vec<ArrayRef> = Vec::with_capacity(columns.len());
    for (index, name) in columns.iter().enumerate() {
        let values = rows.iter().map(|row| &row[index]);
        let text = values.clone().any(|v| matches!(v, Cell::Text(_)));
        let int = values.clone().any(|v| matches!(v, Cell::Int(_)));
        let (data_type, array): (DataType, ArrayRef) = match (text, int) {
            (true, true) => return Err(format!("字段 {name} 同时包含文本与整数")),
            (true, false) => (
                DataType::Utf8,
                Arc::new(values.map(Cell::text).collect::<StringArray>()),
            ),
            (false, true) => (
                DataType::Int64,
                Arc::new(
                    values
                        .map(|v| match v {
                            Cell::Int(n) => Some(*n),
                            _ => None,
                        })
                        .collect::<Int64Array>(),
                ),
            ),
            (false, false) => (DataType::Null, Arc::new(NullArray::new(rows.len()))),
        };
        fields.push(Field::new(name, data_type, true));
        arrays.push(array);
    }
    let schema = Arc::new(Schema::new(fields));
    let batch = RecordBatch::try_new(schema.clone(), arrays).map_err(|e| e.to_string())?;
    let properties = WriterProperties::builder()
        .set_compression(Compression::SNAPPY)
        .build();
    let mut output = Vec::new();
    let mut writer =
        ArrowWriter::try_new(&mut output, schema, Some(properties)).map_err(|e| e.to_string())?;
    writer.write(&batch).map_err(|e| e.to_string())?;
    writer.close().map_err(|e| e.to_string())?;
    Ok(output)
}

pub(crate) fn store_partition(store: &ArtifactStore, content: &[u8]) -> Result<ArtifactRef> {
    store
        .put_addressed(PARTITION_DIRECTORY, ".parquet", content)
        .map_err(|e| artifact_error(e, "分区目录缺失", "已存在的分区文件校验和不一致"))
}
