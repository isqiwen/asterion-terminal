use arrow_array::{Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use asterion_data_store::{DailyScan, invoke};
use asterion_instrument_catalog::Catalog;
use asterion_kernel::files::ReadRoot;
use parquet::{arrow::ArrowWriter, basic::Compression, file::properties::WriterProperties};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    fs,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

fn catalog() -> Value {
    serde_json::from_str(include_str!(
        "../../instrument-catalog/tests/fixtures/valid.json"
    ))
    .unwrap()
}
fn identity() -> Value {
    let catalog = catalog();
    let id = Catalog::from_value(catalog.clone())
        .unwrap()
        .id()
        .to_owned();
    json!({"catalog_id":id,"catalog":catalog,"information_at":"2026-09-20T00:00:00Z","bindings":[{"contract":"SHFE.rb2610","source":"tushare","symbol":"RB2610.SHF"}]})
}
fn request(start: &str, end: &str, batch: usize) -> Value {
    json!({"version_id":"fixed","contract_ids":["SHFE.RB.202610.20251001"],"start":start,"end":end,"columns":["close"],"batch_rows":batch})
}
fn parquet(days: &[String], provenance: bool, rows_per_group: usize) -> Vec<u8> {
    let mut fields = vec![
        Field::new("contract", DataType::Utf8, false),
        Field::new("trading_day", DataType::Utf8, false),
        Field::new("close", DataType::Utf8, false),
    ];
    let mut columns: Vec<Arc<dyn Array>> = vec![
        Arc::new(StringArray::from(vec!["SHFE.rb2610"; days.len()])),
        Arc::new(StringArray::from(days.to_vec())),
        Arc::new(StringArray::from(vec!["3200.00100"; days.len()])),
    ];
    if provenance {
        fields.extend([
            Field::new("_observed_at", DataType::Utf8, false),
            Field::new("_raw_version_id", DataType::Utf8, false),
        ]);
        columns.extend([
            Arc::new(StringArray::from(vec!["2026-09-01T00:00:00Z"; days.len()])) as Arc<dyn Array>,
            Arc::new(StringArray::from(vec!["raw-immutable"; days.len()])),
        ]);
    }
    let batch = RecordBatch::try_new(Arc::new(Schema::new(fields)), columns).unwrap();
    let props = WriterProperties::builder()
        .set_compression(Compression::SNAPPY)
        .set_max_row_group_row_count(Some(rows_per_group))
        .build();
    let mut bytes = Vec::new();
    let mut writer = ArrowWriter::try_new(&mut bytes, batch.schema(), Some(props)).unwrap();
    writer.write(&batch).unwrap();
    writer.close().unwrap();
    bytes
}
fn sha(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}
fn version(bytes: &[u8], rows: usize) -> Value {
    json!({"id":"fixed","rows":rows,"manifest":{"layer":"STANDARD","type":{"id":"futures.daily","fields":[{"name":"close"},{"name":"contract"},{"name":"trading_day"}]},"scope":{"contract_ids":["SHFE.RB.202610.20251001"]},"source":"local_file","format":"parquet","path":"data.parquet","checksum":sha(bytes),"import_options":{"identity":identity()}}})
}
fn open(root: &std::path::Path, version: &Value, query: &Value) -> DailyScan {
    DailyScan::new(
        Arc::new(ReadRoot::new(root, 1024 * 1024 * 1024, 1000).unwrap()),
        &version.to_string(),
        &query.to_string(),
        Arc::new(AtomicBool::new(false)),
    )
    .unwrap()
}
#[test]
fn native_batches_prune_row_groups_preserve_decimal_text_and_close() {
    let temp = tempfile::tempdir().unwrap();
    let first = "2025-10-01".parse::<chrono::NaiveDate>().unwrap();
    let days: Vec<_> = (0..300)
        .map(|day| (first + chrono::Duration::days(day)).to_string())
        .collect();
    let bytes = parquet(&days, false, 100);
    fs::write(temp.path().join("data.parquet"), &bytes).unwrap();
    let mut scan = open(
        temp.path(),
        &version(&bytes, 300),
        &request("2026-01-01", "2026-01-02", 16),
    );
    assert_eq!(scan.metrics().files_opened, 0);
    let mut seen = 0;
    while let Some(batch) = scan.next_batch().unwrap() {
        assert_eq!(
            batch
                .schema()
                .fields()
                .iter()
                .map(|v| v.name().as_str())
                .collect::<Vec<_>>(),
            ["close", "_contract_id", "_observed_at", "_raw_version_id"]
        );
        let close = batch
            .column(0)
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        assert!(close.iter().all(|v| v == Some("3200.00100")));
        assert_eq!(batch.column(2).logical_null_count(), batch.num_rows());
        seen += batch.num_rows();
    }
    assert_eq!(seen, 2);
    let stats = scan.metrics();
    assert_eq!(stats.files_opened, 1);
    assert_eq!(stats.row_groups_decoded, 1);
    assert_eq!(stats.rows_decoded, 100);
    assert_eq!(stats.verified_bytes, bytes.len() as u64);
    assert!(stats.closed);
    scan.close();
    assert!(scan.next_batch().unwrap().is_none());
}
#[test]
fn bad_unselected_partition_is_never_opened_and_evidence_is_preserved() {
    let temp = tempfile::tempdir().unwrap();
    fs::create_dir(temp.path().join("artifacts")).unwrap();
    let mut parts = Vec::new();
    for day in ["2026-01-01", "2026-02-01"] {
        let bytes = parquet(&[day.into()], true, 1);
        let hash = sha(&bytes);
        fs::write(
            temp.path().join(format!("artifacts/{hash}.parquet")),
            if day.ends_with("01-01") {
                b"corrupt".as_slice()
            } else {
                &bytes
            },
        )
        .unwrap();
        parts.push(json!({"key":day,"checksum":hash,"rows":1,"bytes":bytes.len(),"first":day,"last":day,"inputs":["raw-immutable"],"replaces":null}));
    }
    let index = json!({"schema_version":1,"partitions":parts}).to_string();
    fs::write(temp.path().join("index.json"), &index).unwrap();
    let mut v = version(b"", 2);
    v["manifest"]["format"] = json!("partition_manifest");
    v["manifest"]["path"] = json!("index.json");
    v["manifest"]["checksum"] = json!(sha(index.as_bytes()));
    v["manifest"]["partitions"] = json!(parts);
    let mut scan = open(temp.path(), &v, &request("2026-02-01", "2026-02-01", 1));
    let batch = scan.next_batch().unwrap().unwrap();
    assert_eq!(
        batch
            .column(3)
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap()
            .value(0),
        "raw-immutable"
    );
    assert_eq!(scan.metrics().files_opened, 1);
    scan.close();
    assert!(scan.metrics().closed);
}
#[test]
fn corrupt_selected_file_and_in_place_mutation_fail_and_release_owner() {
    let temp = tempfile::tempdir().unwrap();
    let bytes = parquet(&["2026-01-01".into(), "2026-01-02".into()], false, 2);
    let path = temp.path().join("data.parquet");
    fs::write(&path, &bytes).unwrap();
    let v = version(&bytes, 2);
    let q = request("2026-01-01", "2026-01-02", 1);
    let mut scan = open(temp.path(), &v, &q);
    scan.next_batch().unwrap().unwrap();
    fs::write(&path, b"changed").unwrap();
    assert!(scan.next_batch().is_err());
    assert!(scan.metrics().closed);
    let mut scan = open(temp.path(), &v, &q);
    assert!(scan.next_batch().unwrap_err().contains("校验和"));
    assert!(scan.metrics().closed);
}
#[test]
fn strict_request_and_precise_fixed_version_authorization() {
    let temp = tempfile::tempdir().unwrap();
    let bytes = parquet(&["2026-01-01".into()], false, 1);
    let v = version(&bytes, 1);
    for mutation in [
        json!({"columns":["password"]}),
        json!({"contract_ids":["other"]}),
        json!({"version_id":"other"}),
        json!({"batch_rows":4097}),
        json!({"columns":["close","close"]}),
    ] {
        let mut q = request("2026-01-01", "2026-01-01", 1);
        q.as_object_mut()
            .unwrap()
            .extend(mutation.as_object().unwrap().clone());
        assert!(
            DailyScan::new(
                Arc::new(ReadRoot::new(temp.path(), 1024, 100).unwrap()),
                &v.to_string(),
                &q.to_string(),
                Arc::new(AtomicBool::new(false))
            )
            .is_err()
        );
    }
    assert!(
        invoke(
            "validate",
            json!({"model":"ScanRequest","value":{"unexpected":true}})
        )
        .is_err()
    );
    assert!(invoke("schema", json!({})).unwrap()["$defs"]["ScanRequest"].is_object());
}
#[test]
fn cancellation_is_checked_before_file_work_and_close_is_idempotent() {
    let temp = tempfile::tempdir().unwrap();
    let bytes = parquet(&["2026-01-01".into()], false, 1);
    fs::write(temp.path().join("data.parquet"), &bytes).unwrap();
    let cancel = Arc::new(AtomicBool::new(false));
    let mut scan = DailyScan::new(
        Arc::new(ReadRoot::new(temp.path(), 1024 * 1024, 100).unwrap()),
        &version(&bytes, 1).to_string(),
        &request("2026-01-01", "2026-01-01", 1).to_string(),
        cancel.clone(),
    )
    .unwrap();
    cancel.store(true, Ordering::Release);
    assert!(scan.next_batch().is_err());
    assert_eq!(scan.metrics().files_opened, 0);
    scan.close();
    scan.close();
    assert!(scan.metrics().closed);
}

#[test]
fn footer_and_index_budgets_fail_before_decoding() {
    let temp = tempfile::tempdir().unwrap();
    let mut bytes = b"PAR1".to_vec();
    bytes.extend_from_slice(&(16_u32 * 1024 * 1024).to_le_bytes());
    bytes.extend_from_slice(b"PAR1");
    fs::write(temp.path().join("data.parquet"), &bytes).unwrap();
    let mut scan = open(
        temp.path(),
        &version(&bytes, 1),
        &request("2026-01-01", "2026-01-01", 1),
    );
    assert!(scan.next_batch().unwrap_err().contains("预算"));
    assert_eq!(scan.metrics().row_groups_decoded, 0);
    assert!(scan.metrics().closed);
}

#[test]
#[cfg(target_os = "linux")]
fn close_releases_actual_root_and_data_descriptors_even_before_first_read() {
    let temp = tempfile::tempdir().unwrap();
    let bytes = parquet(&["2026-01-01".into(), "2026-01-02".into()], false, 2);
    fs::write(temp.path().join("data.parquet"), &bytes).unwrap();
    let descriptors = || {
        fs::read_dir("/proc/self/fd")
            .unwrap()
            .filter_map(|entry| entry.ok())
            .filter_map(|entry| fs::read_link(entry.path()).ok())
            .filter(|target| target.starts_with(temp.path()))
            .count()
    };
    assert_eq!(descriptors(), 0);
    let mut scan = open(
        temp.path(),
        &version(&bytes, 2),
        &request("2026-01-01", "2026-01-02", 1),
    );
    assert_eq!(descriptors(), 1);
    scan.close();
    assert_eq!(descriptors(), 0);
    let mut scan = open(
        temp.path(),
        &version(&bytes, 2),
        &request("2026-01-01", "2026-01-02", 1),
    );
    scan.next_batch().unwrap().unwrap();
    assert!(descriptors() > 1);
    scan.close();
    assert_eq!(descriptors(), 0);
}

#[test]
fn self_inconsistent_footer_fails_even_when_manifest_and_checksum_match() {
    use bytes::Bytes;
    use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

    let temp = tempfile::tempdir().unwrap();
    let original = parquet(&["2026-01-01".into(), "2026-01-02".into()], false, 2);
    let size = original.len();
    let footer_size = u32::from_le_bytes(original[size - 8..size - 4].try_into().unwrap()) as usize;
    // A two-row footer uses the compact-protocol integer 4 for num_rows.
    // Find that field using the independent Parquet parser, then change only
    // its value to three (6). Page data and row-group declarations stay intact.
    let malformed = (size - 8 - footer_size..size - 8)
        .filter(|i| original[*i] == 4)
        .find_map(|i| {
            let mut candidate = original.clone();
            candidate[i] = 6;
            let reader =
                ParquetRecordBatchReaderBuilder::try_new(Bytes::from(candidate.clone())).ok()?;
            let meta = reader.metadata();
            (meta.file_metadata().num_rows() == 3
                && meta.row_groups().iter().map(|g| g.num_rows()).sum::<i64>() == 2)
                .then_some(candidate)
        })
        .expect("fixture contains a num_rows footer field");
    fs::write(temp.path().join("data.parquet"), &malformed).unwrap();
    let mut scan = open(
        temp.path(),
        &version(&malformed, 3),
        &request("2026-01-01", "2026-01-02", 1),
    );
    assert!(scan.next_batch().unwrap_err().contains("行组行数不一致"));
    assert_eq!(scan.metrics().rows_decoded, 0);
    assert!(scan.metrics().closed);
}
