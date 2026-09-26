use asterion_data_store::{
    Cell, MergeOutcome, MergeRequest, ParentVersion, Partition, PartitionedVersion, merge,
    read_index, read_partition, verify_version,
};
use asterion_kernel::artifacts::ArtifactStore;
use serde_json::{Value, json};
use std::fs;

const T1: &str = "2026-01-01T00:00:00+00:00";
const T2: &str = "2026-01-02T00:00:00+00:00";

fn text(value: &str) -> Cell {
    Cell::Text(value.into())
}
fn daily(day: &str, close: &str) -> Vec<Cell> {
    vec![text("SHFE.RB2610"), text(day), text(close), Cell::Null]
}
fn request(
    job: &str,
    rows: Vec<Vec<Cell>>,
    observed: &str,
    parent: Option<ParentVersion>,
) -> MergeRequest {
    MergeRequest {
        job_id: job.into(),
        type_id: "futures.daily".into(),
        time_field: "trading_day".into(),
        primary_key: vec!["contract".into(), "trading_day".into()],
        raw_version_id: format!("raw-{job}"),
        parent,
        columns: vec![
            "contract".into(),
            "trading_day".into(),
            "close".into(),
            "oi".into(),
        ],
        observed_at: vec![observed.into(); rows.len()],
        rows,
    }
}
fn parent(id: &str, outcome: &MergeOutcome) -> ParentVersion {
    let detail = &outcome.detail;
    ParentVersion {
        id: id.into(),
        revision: detail["revision"].as_i64().unwrap(),
        observed_at: detail["observed_at"].as_str().unwrap().into(),
        coverage_gaps: serde_json::from_value(detail["coverage_gaps"].clone()).unwrap(),
        first: detail["first"].as_str().unwrap().into(),
        last: detail["last"].as_str().unwrap().into(),
        index: PartitionedVersion {
            path: outcome.index.name.clone(),
            checksum: outcome.index.sha256.clone(),
            bytes: outcome.index.bytes,
            partitions: partitions(outcome),
        },
    }
}
fn partitions(outcome: &MergeOutcome) -> Vec<Partition> {
    serde_json::from_value(outcome.detail["partitions"].clone()).unwrap()
}
fn store() -> (tempfile::TempDir, ArtifactStore) {
    let temp = tempfile::tempdir().unwrap();
    let store = ArtifactStore::new(temp.path(), 1 << 30).unwrap();
    (temp, store)
}
fn closes(store: &ArtifactStore, part: &Partition) -> Vec<Value> {
    let batch = read_partition(store, part).unwrap();
    let close = batch.column_by_name("close").unwrap();
    let close = close
        .as_any()
        .downcast_ref::<arrow_array::StringArray>()
        .unwrap();
    close.iter().map(|v| json!(v)).collect()
}

#[test]
fn first_publication_partitions_by_month_with_provenance_and_verified_index() {
    let (_temp, store) = store();
    let rows = vec![
        daily("2024-02-01", "3"),
        daily("2024-01-03", "2"),
        daily("2024-01-02", "1"),
    ];
    let outcome = merge(&store, &request("job-1", rows, T1, None)).unwrap();
    let parts = partitions(&outcome);
    assert_eq!(outcome.series, "monthly-observed-v1");
    assert_eq!(outcome.rows, 3);
    assert_eq!(
        parts
            .iter()
            .map(|p| (p.key.as_str(), p.rows, p.first.as_str(), p.last.as_str()))
            .collect::<Vec<_>>(),
        [
            ("2024-01", 2, "2024-01-02", "2024-01-03"),
            ("2024-02", 1, "2024-02-01", "2024-02-01")
        ]
    );
    assert!(
        parts
            .iter()
            .all(|p| p.inputs == ["raw-job-1"] && p.replaces.is_none())
    );
    assert!(outcome.index.name.starts_with("datasets/job-1/partitions/"));
    assert_eq!(outcome.detail["revision"], 1);
    assert_eq!(outcome.detail["observed_at"], T1);
    assert_eq!(outcome.detail["changes"]["added"], 3);
    assert_eq!(outcome.detail["coverage"], "RETURNED_ROWS_ONLY");
    assert_eq!(outcome.detail["coverage_gaps"], Value::Null);
    assert_eq!(outcome.touched.len(), 3);
    assert_eq!(outcome.touched[0]["trading_day"], text("2024-01-02"));
    assert_eq!(outcome.touched[0]["_observed_at"], text(T1));
    assert_eq!(outcome.touched[0]["_raw_version_id"], text("raw-job-1"));
    let version = parent("v1", &outcome).index;
    assert_eq!(read_index(&store, &version).unwrap(), parts);
    let batch = read_partition(&store, &parts[0]).unwrap();
    let schema = batch.schema();
    assert_eq!(
        schema
            .fields()
            .iter()
            .map(|f| (f.name().as_str(), f.data_type().clone()))
            .collect::<Vec<_>>(),
        [
            ("contract", arrow_schema::DataType::Utf8),
            ("trading_day", arrow_schema::DataType::Utf8),
            ("close", arrow_schema::DataType::Utf8),
            ("oi", arrow_schema::DataType::Null),
            ("_observed_at", arrow_schema::DataType::Utf8),
            ("_raw_version_id", arrow_schema::DataType::Utf8),
        ]
    );
    assert_eq!(closes(&store, &parts[0]), [json!("1"), json!("2")]);
    // An identical retry writes the same content-addressed bytes.
    let again = merge(
        &store,
        &request(
            "job-1",
            vec![
                daily("2024-02-01", "3"),
                daily("2024-01-03", "2"),
                daily("2024-01-02", "1"),
            ],
            T1,
            None,
        ),
    )
    .unwrap();
    assert_eq!(again.index, outcome.index);
}

#[test]
fn untouched_months_are_reused_and_observation_order_decides_rows() {
    let (_temp, store) = store();
    let first = merge(
        &store,
        &request(
            "job-1",
            vec![daily("2024-01-02", "1"), daily("2024-02-01", "5")],
            T1,
            None,
        ),
    )
    .unwrap();
    let january = partitions(&first)[0].clone();
    let second = merge(
        &store,
        &request(
            "job-2",
            vec![daily("2024-02-01", "6"), daily("2024-03-01", "7")],
            T2,
            Some(parent("v1", &first)),
        ),
    )
    .unwrap();
    let parts = partitions(&second);
    assert_eq!(parts[0], january);
    assert_eq!(
        parts[1].replaces.as_deref(),
        Some(partitions(&first)[1].checksum.as_str())
    );
    assert_eq!(parts[1].inputs, ["raw-job-2"]);
    assert_eq!(second.detail["revision"], 2);
    assert_eq!(second.detail["parent_version_id"], "v1");
    assert_eq!(second.detail["changes"]["revised"], 1);
    assert_eq!(second.detail["changes"]["added"], 1);
    assert_eq!(second.detail["observed_at"], T2);
    assert_eq!(closes(&store, &parts[1]), [json!("6")]);
    // January was not touched: it is neither decoded nor returned.
    assert_eq!(second.touched.len(), 2);
    assert_eq!(second.metrics.partitions_read, ["2024-02"]);
    assert_eq!(second.metrics.bytes_read, partitions(&first)[1].bytes);
    assert!(first.metrics.partitions_read.is_empty());

    let stale = merge(
        &store,
        &request(
            "job-3",
            vec![daily("2024-02-01", "0")],
            T1,
            Some(parent("v2", &second)),
        ),
    )
    .unwrap();
    assert_eq!(partitions(&stale), parts);
    assert_eq!(stale.detail["changes"]["stale_ignored"], 1);
    assert_eq!(stale.detail["observed_at"], T2);

    let refreshed = merge(
        &store,
        &request(
            "job-4",
            vec![daily("2024-02-01", "6")],
            "2026-01-03T00:00:00+00:00",
            Some(parent("v2", &second)),
        ),
    )
    .unwrap();
    assert_eq!(refreshed.detail["changes"]["refreshed"], 1);
    assert_ne!(partitions(&refreshed)[1].checksum, parts[1].checksum);

    let unchanged = merge(
        &store,
        &request(
            "job-5",
            vec![daily("2024-02-01", "6")],
            T2,
            Some(parent("v2", &second)),
        ),
    )
    .unwrap();
    assert_eq!(unchanged.detail["changes"]["unchanged"], 1);
    assert_eq!(partitions(&unchanged), parts);

    let conflict = merge(
        &store,
        &request(
            "job-6",
            vec![daily("2024-02-01", "9")],
            T2,
            Some(parent("v2", &second)),
        ),
    );
    assert_eq!(
        conflict.err().unwrap(),
        "相同采集时间出现不同记录，拒绝自动裁决"
    );
}

#[test]
fn calendar_gaps_extend_coverage_and_recollection_fills_them() {
    let (_temp, store) = store();
    let calendar = |job: &str, days: &[&str], parent| MergeRequest {
        job_id: job.into(),
        type_id: "futures.calendar".into(),
        time_field: "date".into(),
        primary_key: vec!["exchange".into(), "date".into()],
        raw_version_id: format!("raw-{job}"),
        parent,
        columns: vec!["exchange".into(), "date".into(), "is_open".into()],
        rows: days
            .iter()
            .map(|d| vec![text("SHFE"), text(d), Cell::Int(1)])
            .collect(),
        observed_at: vec![T1.into(); days.len()],
    };
    let first = merge(&store, &calendar("c1", &["2024-01-01", "2024-01-04"], None)).unwrap();
    assert_eq!(first.detail["coverage"], "CALENDAR_GAPS");
    assert_eq!(
        first.detail["coverage_gaps"],
        json!([{"start": "2024-01-02", "end": "2024-01-03"}])
    );
    let batch = read_partition(&store, &partitions(&first)[0]).unwrap();
    assert_eq!(
        *batch
            .schema()
            .field_with_name("is_open")
            .unwrap()
            .data_type(),
        arrow_schema::DataType::Int64
    );
    let second = merge(
        &store,
        &calendar(
            "c2",
            &["2024-01-02", "2024-01-03", "2024-01-06"],
            Some(parent("v1", &first)),
        ),
    )
    .unwrap();
    assert_eq!(
        second.detail["coverage_gaps"],
        json!([{"start": "2024-01-05", "end": "2024-01-05"}])
    );
    let third = merge(
        &store,
        &calendar("c3", &["2024-01-05"], Some(parent("v2", &second))),
    )
    .unwrap();
    assert_eq!(third.detail["coverage"], "CALENDAR_COMPLETE");
    assert_eq!(third.detail["coverage_gaps"], json!([]));
}

#[test]
fn parent_index_and_partition_damage_or_mismatch_is_rejected() {
    let (temp, store) = store();
    let first = merge(
        &store,
        &request("job-1", vec![daily("2024-01-02", "1")], T1, None),
    )
    .unwrap();
    let mut changed = parent("v1", &first);
    changed.index.partitions[0].rows = 2;
    assert!(
        merge(
            &store,
            &request("job-2", vec![daily("2024-01-03", "2")], T2, Some(changed))
        )
        .err()
        .unwrap()
        .contains("父版本分区清单与版本记录不一致")
    );
    let part = &partitions(&first)[0];
    fs::write(temp.path().join(part.artifact_name()), b"damaged").unwrap();
    assert_eq!(
        merge(
            &store,
            &request(
                "job-3",
                vec![daily("2024-01-03", "2")],
                T2,
                Some(parent("v1", &first))
            )
        )
        .err(),
        Some("分区文件校验和不一致".into())
    );
    fs::write(temp.path().join(&first.index.name), b"{}").unwrap();
    assert_eq!(
        merge(
            &store,
            &request(
                "job-4",
                vec![daily("2024-02-03", "2")],
                T2,
                Some(parent("v1", &first))
            )
        )
        .err(),
        Some("父版本分区清单校验和不一致".into())
    );
}

#[test]
fn field_sets_kinds_and_requests_follow_the_current_contract() {
    let (_temp, store) = store();
    let first = merge(
        &store,
        &request("job-1", vec![daily("2024-01-02", "1")], T1, None),
    )
    .unwrap();
    let mut wider = request(
        "job-2",
        vec![[daily("2024-01-03", "2"), vec![text("x")]].concat()],
        T2,
        Some(parent("v1", &first)),
    );
    wider.columns.push("extra".into());
    assert_eq!(
        merge(&store, &wider).err(),
        Some("父版本分区字段与本次采集不一致".into())
    );
    let mut mixed = request(
        "job-3",
        vec![daily("2024-03-02", "1"), daily("2024-03-03", "2")],
        T1,
        None,
    );
    mixed.rows[1][2] = Cell::Int(2);
    assert!(
        merge(&store, &mixed)
            .err()
            .unwrap()
            .contains("同时包含文本与整数")
    );
    let cases: Vec<fn(&mut MergeRequest)> = vec![
        |r| r.type_id = "futures.contracts".into(),
        |r| r.job_id = "../escape".into(),
        |r| r.columns[3] = "_hidden".into(),
        |r| r.primary_key.push("missing".into()),
        |r| r.observed_at[0] = "yesterday".into(),
        |r| r.rows[0].pop().map(|_| ()).unwrap(),
        |r| r.rows.clear(),
    ];
    for mutate in cases {
        let mut invalid = request("job-4", vec![daily("2024-04-02", "1")], T1, None);
        mutate(&mut invalid);
        assert!(merge(&store, &invalid).is_err());
    }
    assert!(serde_json::from_value::<Cell>(json!(true)).is_err());
    assert!(serde_json::from_value::<Cell>(json!(1.5)).is_err());
}

#[test]
fn version_verification_checks_every_partition_without_decoding() {
    let (temp, store) = store();
    let outcome = merge(
        &store,
        &request(
            "job-1",
            vec![daily("2024-01-02", "1"), daily("2024-02-01", "2")],
            T1,
            None,
        ),
    )
    .unwrap();
    let version = parent("v1", &outcome).index;
    assert_eq!(
        verify_version(&store, &version).unwrap(),
        partitions(&outcome)
    );
    let last = &partitions(&outcome)[1];
    fs::remove_file(temp.path().join(last.artifact_name())).unwrap();
    assert_eq!(
        verify_version(&store, &version).err(),
        Some("分区文件缺失".into())
    );
}
