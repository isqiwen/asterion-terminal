use crate::{
    Result, ScanRequest,
    partitions::{INDEX_BYTES, MAX_COLUMNS, MAX_ROWS, MAX_VALUE_BYTES, Partition, parse_index},
    source::{Source, TRANSFER_BYTES},
};
use arrow_array::{
    Array, ArrayRef, Date32Array, LargeStringArray, RecordBatch, StringArray, StringViewArray,
    UInt32Array, new_null_array,
};
use arrow_schema::{DataType, Field, Schema};
use arrow_select::take::take;
use asterion_instrument_catalog::{ImportResolver, SourceResolver, validate_market_code};
use asterion_kernel::files::ReadRoot;
use chrono::NaiveDate;
use parquet::{
    arrow::{
        ProjectionMask,
        arrow_reader::{
            ArrowReaderMetadata, ParquetRecordBatchReader, ParquetRecordBatchReaderBuilder,
        },
    },
    basic::Compression,
    file::{metadata::RowGroupMetaData, statistics::Statistics},
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    collections::{BTreeSet, VecDeque},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

const JSON_BYTES: usize = 64 * 1024 * 1024;
const MAX_ROW_GROUPS: usize = 10_000;

#[derive(Debug, Default, Clone, Serialize)]
pub struct Metrics {
    /// Data files only; the small partition index is not a data file.
    pub files_opened: u64,
    pub row_groups_decoded: u64,
    pub rows_decoded: u64,
    /// Compressed data bytes and index bytes successfully checksum-verified.
    pub verified_bytes: u64,
    pub closed: bool,
}

#[derive(Deserialize)]
struct Version {
    id: String,
    rows: i64,
    manifest: Manifest,
}
#[derive(Deserialize)]
struct Manifest {
    layer: String,
    #[serde(rename = "type")]
    data_type: DataTypeDeclaration,
    scope: Scope,
    source: String,
    format: String,
    path: String,
    checksum: String,
    partitions: Option<Vec<Partition>>,
    import_options: Option<Value>,
    contract_identity: Option<Value>,
}
#[derive(Deserialize)]
struct DataTypeDeclaration {
    id: String,
    fields: Vec<FieldDeclaration>,
}
#[derive(Deserialize)]
struct FieldDeclaration {
    name: String,
}
#[derive(Deserialize)]
struct Scope {
    contract_ids: Vec<String>,
}

struct FileSpec {
    name: String,
    checksum: String,
    rows: i64,
    bytes: Option<u64>,
}
enum Identity {
    Source(SourceResolver),
    Import(ImportResolver),
}
struct OpenFile {
    source: Source,
    metadata: ArrowReaderMetadata,
    projection: ProjectionMask,
    groups: VecDeque<usize>,
    reader: Option<ParquetRecordBatchReader>,
    counted: bool,
}

/// A lazy fixed-version scan. It owns at most one Parquet file and one decoded
/// batch. Limits guard metadata, declared row-group sizes, transfers and output;
/// the third-party decoder is not an allocator sandbox for hostile Parquet.
pub struct DailyScan {
    root: Option<Arc<ReadRoot>>,
    request: ScanRequest,
    manifest: Manifest,
    rows: i64,
    identity: Identity,
    contracts: BTreeSet<String>,
    files: VecDeque<FileSpec>,
    initialized: bool,
    current: Option<OpenFile>,
    cancelled: Arc<AtomicBool>,
    metrics: Metrics,
}
fn json(raw: &str) -> Result<Value> {
    if raw.len() > JSON_BYTES {
        return Err("扫描元数据超出预算".into());
    }
    asterion_foundation::communication::parse_json(raw.as_bytes())
}
fn checksum(value: &str) -> Result<()> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
    {
        return Err("扫描校验和格式不合法".into());
    }
    Ok(())
}
fn file_error(error: asterion_kernel::files::Error) -> String {
    match error {
        asterion_kernel::files::Error::Io(e) if e.kind() == std::io::ErrorKind::NotFound => {
            "数据文件缺失".into()
        }
        e => e.to_string(),
    }
}
impl DailyScan {
    pub fn new(
        root: Arc<ReadRoot>,
        version_json: &str,
        request_json: &str,
        cancelled: Arc<AtomicBool>,
    ) -> Result<Self> {
        let request: ScanRequest =
            serde_json::from_value(json(request_json)?).map_err(|e| e.to_string())?;
        request.validate()?;
        let version: Version =
            serde_json::from_value(json(version_json)?).map_err(|e| e.to_string())?;
        if version.id != request.version_id || !(0..=MAX_ROWS).contains(&version.rows) {
            return Err("扫描固定版本标识或行数不合法".into());
        }
        let manifest = version.manifest;
        if manifest.layer != "STANDARD" || manifest.data_type.id != "futures.daily" {
            return Err("批次扫描当前只支持标准日线".into());
        }
        if manifest.data_type.fields.len() > MAX_COLUMNS
            || manifest.scope.contract_ids.len() > 100_000
        {
            return Err("扫描版本字段或合约超出预算".into());
        }
        let declared: BTreeSet<_> = manifest
            .data_type
            .fields
            .iter()
            .map(|v| v.name.as_str())
            .collect();
        if !request
            .columns
            .iter()
            .all(|v| declared.contains(v.as_str()))
        {
            return Err("扫描包含未声明字段".into());
        }
        let declared_contracts: BTreeSet<_> = manifest.scope.contract_ids.iter().collect();
        if !request
            .contract_ids
            .iter()
            .all(|v| declared_contracts.contains(v))
        {
            return Err("扫描合约不在固定版本中".into());
        }
        if !matches!(manifest.format.as_str(), "partition_manifest" | "parquet") {
            return Err("不支持此扫描文件格式".into());
        }
        checksum(&manifest.checksum)?;
        let identity = if manifest.source == "local_file" {
            let value = manifest
                .import_options
                .as_ref()
                .and_then(|v| v.get("identity"))
                .ok_or("导入版本缺少合约身份")?;
            Identity::Import(ImportResolver::from_value(value.clone())?)
        } else {
            Identity::Source(SourceResolver::from_value(
                manifest
                    .contract_identity
                    .clone()
                    .ok_or("固定版本缺少合约身份")?,
            )?)
        };
        let contracts = request.contract_ids.iter().cloned().collect();
        Ok(Self {
            root: Some(root),
            request,
            manifest,
            rows: version.rows,
            identity,
            contracts,
            files: VecDeque::new(),
            initialized: false,
            current: None,
            cancelled,
            metrics: Metrics::default(),
        })
    }
    pub fn metrics(&self) -> Metrics {
        self.metrics.clone()
    }
    pub fn close(&mut self) {
        self.cancelled.store(true, Ordering::Release);
        self.current = None;
        self.files.clear();
        self.root = None;
        self.metrics.closed = true;
    }
    pub fn next_batch(&mut self) -> Result<Option<RecordBatch>> {
        if self.metrics.closed {
            return Ok(None);
        }
        let result = self.next_inner();
        if !matches!(result, Ok(Some(_))) {
            self.close();
        }
        result
    }
    fn check(&self) -> Result<()> {
        if self.cancelled.load(Ordering::Acquire) {
            Err("数据扫描已关闭".into())
        } else {
            Ok(())
        }
    }
    fn initialize(&mut self) -> Result<()> {
        if self.manifest.format == "parquet" {
            self.files.push_back(FileSpec {
                name: self.manifest.path.clone(),
                checksum: self.manifest.checksum.clone(),
                rows: self.rows,
                bytes: None,
            });
        } else {
            let expected = self
                .manifest
                .partitions
                .as_ref()
                .ok_or("固定版本缺少分区清单")?;
            let file = self
                .root
                .as_ref()
                .ok_or("数据扫描已关闭")?
                .open_file(&self.manifest.path)
                .map_err(file_error)?;
            if file.len() > INDEX_BYTES {
                return Err("扫描分区清单超出预算".into());
            }
            file.verify_sha256(&self.manifest.checksum, &self.cancelled)
                .map_err(|_| "扫描清单校验和不一致或文件发生变化")?;
            let source = Source {
                file,
                cancelled: self.cancelled.clone(),
            };
            let mut content = vec![0; source.file.len() as usize];
            source.read_exact(0, &mut content)?;
            let partitions = parse_index(&content, expected).map_err(|e| format!("扫描{e}"))?;
            self.metrics.verified_bytes += source.file.len();
            let mut rows = 0_i64;
            for p in partitions {
                let date = |value: &str| {
                    NaiveDate::parse_from_str(value, "%Y-%m-%d")
                        .map_err(|_| "扫描日线分区日期不合法".to_string())
                };
                let (first, last) = (date(&p.first)?, date(&p.last)?);
                rows = rows.checked_add(p.rows).ok_or("扫描分区行数溢出")?;
                if first <= self.request.end && last >= self.request.start {
                    self.files.push_back(FileSpec {
                        name: p.artifact_name(),
                        checksum: p.checksum,
                        rows: p.rows,
                        bytes: Some(p.bytes),
                    });
                }
            }
            if rows != self.rows {
                return Err("扫描清单总行数不一致".into());
            }
        }
        self.initialized = true;
        Ok(())
    }
    fn open(&mut self, spec: FileSpec) -> Result<OpenFile> {
        self.check()?;
        let file = self
            .root
            .as_ref()
            .ok_or("数据扫描已关闭")?
            .open_file(&spec.name)
            .map_err(file_error)?;
        self.metrics.files_opened += 1;
        if spec.bytes.is_some_and(|bytes| bytes != file.len()) {
            return Err("扫描文件字节数或校验和不一致".into());
        }
        file.verify_sha256(&spec.checksum, &self.cancelled)
            .map_err(|_| "扫描文件校验和不一致或文件发生变化")?;
        self.metrics.verified_bytes += file.len();
        let source = Source {
            file,
            cancelled: self.cancelled.clone(),
        };
        source.footer()?;
        let metadata = ArrowReaderMetadata::load(&source, Default::default())
            .map_err(|_| "Parquet 文件清单解析失败")?;
        let parquet = metadata.metadata();
        let schema = metadata.schema();
        if parquet.file_metadata().num_rows() != spec.rows {
            return Err("扫描文件行数不一致".into());
        }
        if parquet.num_row_groups() > MAX_ROW_GROUPS || schema.fields().len() > MAX_COLUMNS {
            return Err("Parquet 行组或字段数超出预算".into());
        }
        let mut names = BTreeSet::new();
        for field in schema.fields() {
            if !names.insert(field.name())
                || field.name().len() > 256
                || !matches!(
                    field.data_type(),
                    DataType::Null
                        | DataType::Boolean
                        | DataType::Int8
                        | DataType::Int16
                        | DataType::Int32
                        | DataType::Int64
                        | DataType::UInt8
                        | DataType::UInt16
                        | DataType::UInt32
                        | DataType::UInt64
                        | DataType::Float32
                        | DataType::Float64
                        | DataType::Utf8
                        | DataType::LargeUtf8
                        | DataType::Utf8View
                        | DataType::Date32
                        | DataType::Decimal128(_, _)
                        | DataType::Decimal256(_, _)
                )
            {
                return Err("固定日线必须使用唯一的平面标量字段".into());
            }
        }
        let mut required: BTreeSet<&str> =
            self.request.columns.iter().map(String::as_str).collect();
        required.extend(["contract", "trading_day"]);
        if self.manifest.format == "partition_manifest" {
            required.extend(["_observed_at", "_raw_version_id"]);
        }
        if required.iter().any(|name| schema.index_of(name).is_err()) {
            return Err("固定日线缺少请求字段或来源证据".into());
        }
        let projected: Vec<usize> = required
            .into_iter()
            .map(|name| schema.index_of(name).expect("checked field"))
            .collect();
        let projection = ProjectionMask::roots(parquet.file_metadata().schema_descr(), projected);
        let day = schema.index_of("trading_day").expect("checked field");
        let mut groups = VecDeque::new();
        let mut rows = 0_i64;
        for (index, group) in parquet.row_groups().iter().enumerate() {
            group_limits(group, source.file.len())?;
            rows = rows
                .checked_add(group.num_rows())
                .ok_or("Parquet 行组行数溢出")?;
            if may_intersect(
                group.column(day).statistics(),
                self.request.start,
                self.request.end,
            ) {
                groups.push_back(index);
            }
        }
        if rows != spec.rows {
            return Err("扫描文件行组行数不一致".into());
        }
        source.check()?;
        Ok(OpenFile {
            source,
            metadata,
            projection,
            groups,
            reader: None,
            counted: false,
        })
    }
    fn next_inner(&mut self) -> Result<Option<RecordBatch>> {
        self.check()?;
        if !self.initialized {
            self.initialize()?;
        }
        loop {
            self.check()?;
            if self.current.is_none() {
                let Some(spec) = self.files.pop_front() else {
                    return Ok(None);
                };
                self.current = Some(self.open(spec)?);
            }
            let current = self.current.as_mut().expect("current file");
            current.source.check()?;
            if current.reader.is_none() {
                let Some(group) = current.groups.pop_front() else {
                    self.current = None;
                    continue;
                };
                current.reader = Some(
                    ParquetRecordBatchReaderBuilder::new_with_metadata(
                        current.source.clone(),
                        current.metadata.clone(),
                    )
                    .with_projection(current.projection.clone())
                    .with_row_groups(vec![group])
                    .with_batch_size(self.request.batch_rows)
                    .build()
                    .map_err(|_| "Parquet 批次读取初始化失败")?,
                );
                current.counted = false;
            }
            let next = current.reader.as_mut().expect("active row group").next();
            let Some(batch) = next else {
                current.reader = None;
                continue;
            };
            let batch = batch.map_err(|_| "Parquet 批次解码失败")?;
            current.source.check()?;
            if !current.counted {
                self.metrics.row_groups_decoded += 1;
                current.counted = true;
            }
            self.metrics.rows_decoded += batch.num_rows() as u64;
            let filtered = self.filter(batch)?;
            if let Some(batch) = filtered {
                return Ok(Some(batch));
            }
        }
    }
    fn filter(&self, batch: RecordBatch) -> Result<Option<RecordBatch>> {
        if batch.get_array_memory_size() > TRANSFER_BYTES
            || batch.num_rows() > self.request.batch_rows
        {
            return Err("扫描批次超出内存预算".into());
        }
        let contracts = batch.column_by_name("contract").ok_or("扫描缺少合约字段")?;
        let days = batch
            .column_by_name("trading_day")
            .ok_or("扫描缺少交易日字段")?;
        // The cache is bounded by the batch, with no persistent growth across history.
        let mut resolved = std::collections::BTreeMap::new();
        let mut selected = Vec::new();
        let mut ids = Vec::new();
        for row in 0..batch.num_rows() {
            if row % 128 == 0 {
                self.check()?;
            }
            for column in batch.columns() {
                check_value(column, row)?;
            }
            let day = day_value(days, row)?;
            if day < self.request.start || day > self.request.end {
                continue;
            }
            let code = text_value(contracts, row)?;
            let key = (code, day);
            let actual = if let Some(actual) = resolved.get(&key) {
                actual
            } else {
                let actual = match &self.identity {
                    Identity::Source(identity) => identity.resolve(day)?,
                    Identity::Import(identity) => identity.resolve(code, day)?,
                };
                resolved.entry(key).or_insert(actual)
            };
            if !self.contracts.contains(&actual.id) {
                continue;
            }
            validate_market_code(code, actual)?;
            selected.push(row as u32);
            ids.push(actual.id.clone());
        }
        if selected.is_empty() {
            return Ok(None);
        }
        let indices = UInt32Array::from(selected);
        let mut fields = Vec::new();
        let mut columns = Vec::new();
        for name in &self.request.columns {
            let index = batch
                .schema()
                .index_of(name)
                .map_err(|_| "扫描缺少请求字段")?;
            fields.push(batch.schema().field(index).clone());
            columns.push(
                take(batch.column(index).as_ref(), &indices, None).map_err(|_| "扫描列筛选失败")?,
            );
        }
        fields.push(Field::new("_contract_id", DataType::Utf8, false));
        columns.push(Arc::new(StringArray::from(ids)) as ArrayRef);
        for name in ["_observed_at", "_raw_version_id"] {
            if self.manifest.format == "partition_manifest" {
                let index = batch
                    .schema()
                    .index_of(name)
                    .map_err(|_| "累积分区缺少来源证据")?;
                for row in indices.values() {
                    text_value(batch.column(index), *row as usize)?;
                }
                fields.push(batch.schema().field(index).clone());
                columns.push(
                    take(batch.column(index).as_ref(), &indices, None)
                        .map_err(|_| "扫描来源筛选失败")?,
                );
            } else {
                fields.push(Field::new(name, DataType::Null, true));
                columns.push(new_null_array(&DataType::Null, indices.len()));
            }
        }
        let batch = RecordBatch::try_new(Arc::new(Schema::new(fields)), columns)
            .map_err(|_| "扫描批次构造失败")?;
        self.check()?;
        Ok(Some(batch))
    }
}
impl Drop for DailyScan {
    fn drop(&mut self) {
        self.close();
    }
}

fn group_limits(group: &RowGroupMetaData, file_size: u64) -> Result<()> {
    if !(0..=MAX_ROWS).contains(&group.num_rows())
        || !(0..=TRANSFER_BYTES as i64).contains(&group.total_byte_size())
    {
        return Err("Parquet 行组超出预算".into());
    }
    let mut compressed = 0_u64;
    let mut uncompressed = 0_u64;
    for column in group.columns() {
        if !matches!(
            column.compression(),
            Compression::UNCOMPRESSED | Compression::SNAPPY
        ) || !(0..=TRANSFER_BYTES as i64).contains(&column.compressed_size())
            || !(0..=TRANSFER_BYTES as i64).contains(&column.uncompressed_size())
            || column.data_page_offset() < 0
            || column.dictionary_page_offset().is_some_and(|v| v < 0)
            || column.num_values() < 0
        {
            return Err("Parquet 压缩方式不受支持或列块超出预算".into());
        }
        let (offset, length) = column.byte_range();
        if offset.checked_add(length).is_none_or(|end| end > file_size) {
            return Err("Parquet 列块超出文件范围".into());
        }
        compressed = compressed
            .checked_add(length)
            .ok_or("Parquet 列块大小溢出")?;
        uncompressed = uncompressed
            .checked_add(column.uncompressed_size() as u64)
            .ok_or("Parquet 列块大小溢出")?;
    }
    if compressed > TRANSFER_BYTES as u64 || uncompressed > TRANSFER_BYTES as u64 {
        return Err("Parquet 行组压缩字节超出预算".into());
    }
    Ok(())
}
fn may_intersect(stats: Option<&Statistics>, start: NaiveDate, end: NaiveDate) -> bool {
    let bounds = match stats {
        Some(Statistics::ByteArray(s)) => s.min_opt().zip(s.max_opt()).and_then(|(lo, hi)| {
            let lo = std::str::from_utf8(lo.data())
                .ok()?
                .parse::<NaiveDate>()
                .ok()?;
            let hi = std::str::from_utf8(hi.data())
                .ok()?
                .parse::<NaiveDate>()
                .ok()?;
            Some((lo, hi))
        }),
        _ => None,
    };
    bounds.is_none_or(|(lo, hi)| lo > hi || (lo <= end && hi >= start))
}
fn text_value(array: &ArrayRef, row: usize) -> Result<&str> {
    if array.is_null(row) {
        return Err("扫描身份或来源字段不能为空".into());
    }
    let value = if let Some(a) = array.as_any().downcast_ref::<StringArray>() {
        a.value(row)
    } else if let Some(a) = array.as_any().downcast_ref::<LargeStringArray>() {
        a.value(row)
    } else if let Some(a) = array.as_any().downcast_ref::<StringViewArray>() {
        a.value(row)
    } else {
        return Err("扫描身份或来源字段必须为字符串".into());
    };
    if value.is_empty() || value.len() > MAX_VALUE_BYTES {
        return Err("扫描身份或来源字段长度不合法".into());
    }
    Ok(value)
}
fn day_value(array: &ArrayRef, row: usize) -> Result<NaiveDate> {
    if let Some(days) = array.as_any().downcast_ref::<Date32Array>() {
        if days.is_null(row) {
            return Err("交易日不能为空".into());
        }
        return NaiveDate::from_ymd_opt(1970, 1, 1)
            .expect("epoch")
            .checked_add_signed(chrono::Duration::days(i64::from(days.value(row))))
            .ok_or("交易日超出有效范围".into());
    }
    text_value(array, row)?
        .parse()
        .map_err(|_| "交易日格式不合法".into())
}
fn check_value(array: &ArrayRef, row: usize) -> Result<()> {
    if array.is_null(row) {
        return Ok(());
    }
    let size = if let Some(a) = array.as_any().downcast_ref::<StringArray>() {
        a.value(row).len()
    } else if let Some(a) = array.as_any().downcast_ref::<LargeStringArray>() {
        a.value(row).len()
    } else if let Some(a) = array.as_any().downcast_ref::<StringViewArray>() {
        a.value(row).len()
    } else {
        0
    };
    if size > MAX_VALUE_BYTES {
        Err("扫描字段值超出长度预算".into())
    } else {
        Ok(())
    }
}
