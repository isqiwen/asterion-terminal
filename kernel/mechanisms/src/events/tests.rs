use super::*;
fn topic() -> Topic {
    Topic {
        id: "fixture.changed".into(),
        owner: "fixture.owner".into(),
        payload: "driver-type-1".into(),
        read_path: "/fixture".into(),
    }
}
fn registry() -> Arc<Registry> {
    Arc::new(Registry::new(vec![topic()]).unwrap())
}
fn trace() -> Value {
    json!({"version":1,"request_id":"a".repeat(32),"correlation_id":"b".repeat(32),"causation_id":null,"deadline_ms":1})
}
fn row(registry: &Registry, topic: &Topic, sequence: i64) -> Row {
    let message = registry
        .prepare(topic, "entity", json!({"value":1}), trace())
        .unwrap()
        .finish(Some(sequence))
        .unwrap();
    Row {
        id: message["id"].as_str().unwrap().into(),
        topic: topic.id.clone(),
        sequence,
        stream: "entity".into(),
        message,
    }
}

#[test]
fn topic_registration_and_exact_publication_grants_are_kernel_owned() {
    assert!(Registry::new(vec![topic(), topic()]).is_err());
    for (field, value) in [
        ("id", "unnamespaced"),
        ("owner", "BAD.owner"),
        ("read_path", "/a//b"),
        ("payload", ""),
    ] {
        let mut body = serde_json::to_value(topic()).unwrap();
        body[field] = json!(value);
        assert!(
            serde_json::from_value::<Topic>(body)
                .unwrap()
                .validate()
                .is_err()
        );
    }
    let registry = registry();
    assert!(registry.publisher("another.owner", &[topic()]).is_err());
    let grant = registry.publisher("fixture.owner", &[topic()]).unwrap();
    grant.authorize(&topic(), true).unwrap();
    assert!(grant.authorize(&topic(), false).is_err());
    let mut substituted = topic();
    substituted.payload = "driver-type-2".into();
    assert!(grant.authorize(&substituted, true).is_err());
    assert_eq!(
        registry.read_path("fixture.changed", "GET").unwrap(),
        "/fixture"
    );
    assert!(registry.read_path("fixture.changed", "POST").is_err());
    assert!(registry.read_path("unknown.changed", "GET").is_err());
}

#[test]
fn event_construction_owns_identity_causality_payload_and_bigint_range() {
    let registry = registry();
    let draft = registry
        .prepare(&topic(), "stream", json!({"value":1}), trace())
        .unwrap();
    let event = draft.finish(Some(MAX_SEQUENCE)).unwrap();
    assert_eq!(event["correlation_id"], "b".repeat(32));
    assert_eq!(event["causation_id"], "a".repeat(32));
    assert_eq!(event["sequence"], MAX_SEQUENCE.to_string());
    assert_eq!(draft.head_bounds(), (0, MAX_SEQUENCE - 1));
    for number in [None, Some(0), Some(-1)] {
        assert!(draft.finish(number).is_err());
    }
    let another = registry
        .prepare(&topic(), "stream", json!({}), trace())
        .unwrap()
        .finish(Some(1))
        .unwrap();
    assert_ne!(event["id"], another["id"]);
    assert!(registry.prepare(&topic(), "", json!({}), trace()).is_err());
    assert!(registry.prepare(&topic(), "s", json!([]), trace()).is_err());
    assert!(
        registry
            .prepare(
                &topic(),
                "s",
                json!({"value":9_007_199_254_740_992_u64}),
                trace()
            )
            .is_err()
    );
    assert!(
        registry
            .prepare(&topic(), "s", json!({}), json!({}))
            .is_err()
    );
}

#[test]
fn payload_budget_counts_utf8_and_json_separators_at_the_boundary() {
    let registry = registry();
    registry
        .prepare(
            &topic(),
            "s",
            json!({"text":"x".repeat(PAYLOAD_BYTES-12)}),
            trace(),
        )
        .unwrap();
    assert!(
        registry
            .prepare(
                &topic(),
                "s",
                json!({"text":"x".repeat(PAYLOAD_BYTES-11)}),
                trace()
            )
            .unwrap_err()
            .contains("64 KiB")
    );
    registry
        .prepare(&topic(), "s", json!({"text":"期".repeat(21841)}), trace())
        .unwrap();
    assert!(
        registry
            .prepare(&topic(), "s", json!({"text":"期".repeat(21842)}), trace())
            .is_err()
    );
    let value = json!({"a": [1, 2], "b": {"中文": true}});
    let mut bytes = Vec::new();
    value
        .serialize(&mut serde_json::Serializer::with_formatter(
            &mut bytes, Spaced,
        ))
        .unwrap();
    assert_eq!(
        String::from_utf8(bytes).unwrap(),
        r#"{"a": [1, 2], "b": {"中文": true}}"#
    );
}

#[test]
fn read_plans_validate_latest_limits_and_cursor_ranges() {
    let registry = registry();
    for after in [
        "",
        "-1",
        "+1",
        "１",
        "9223372036854775808",
        "00000000000000000000",
    ] {
        assert!(
            registry.read_plan("fixture.changed", after, 100).is_err(),
            "{after}"
        );
    }
    for after in ["0", "latest"] {
        for limit in [-1, 0, 501] {
            assert!(registry.read_plan("fixture.changed", after, limit).is_err());
        }
    }
    assert!(registry.read_plan("missing.topic", "0", 1).is_err());
    let latest = registry.read_plan("fixture.changed", "latest", 1).unwrap();
    assert_eq!(registry.latest(&latest, None).unwrap()["cursor"], "0");
    assert_eq!(
        registry.latest(&latest, Some(MAX_SEQUENCE)).unwrap()["cursor"],
        MAX_SEQUENCE.to_string()
    );
    assert!(registry.latest(&latest, Some(-1)).is_err());
    let plan = registry.read_plan("fixture.changed", "000", 1).unwrap();
    assert_eq!(registry.page(&plan, vec![]).unwrap()["cursor"], "000");
    assert!(registry.latest(&plan, None).is_err());
}

#[test]
fn read_pages_reject_wrong_topic_owner_sequence_and_denormalized_columns() {
    let registry = registry();
    let plan = registry.read_plan("fixture.changed", "0", 2).unwrap();
    let first = row(&registry, &topic(), 1);
    assert_eq!(
        registry
            .page(&plan, vec![first.clone(), row(&registry, &topic(), 2)])
            .unwrap()["cursor"],
        "2"
    );
    assert!(
        registry
            .page(&plan, vec![row(&registry, &topic(), 2)])
            .is_err()
    );
    for field in ["owner", "topic", "stream", "id", "sequence"] {
        let mut damaged = first.clone();
        damaged.message[field] = match field {
            "owner" => json!("another.owner"),
            "sequence" => json!("01"),
            _ => json!("mismatch"),
        };
        assert!(registry.page(&plan, vec![damaged]).is_err(), "{field}");
    }
    let one = registry.read_plan("fixture.changed", "0", 1).unwrap();
    assert!(
        registry
            .page(&one, vec![first, row(&registry, &topic(), 2)])
            .is_err()
    );
    let mut forged = plan;
    forged.offset = Some(1);
    assert!(registry.page(&forged, vec![]).is_err());
}

#[test]
fn restore_streams_rows_and_heads_without_retaining_journal_payloads() {
    let registry = registry();
    let mut validator = JournalValidator::default();
    let mut event = row(&registry, &topic(), 1);
    for sequence in 1..=10_000 {
        event.sequence = sequence;
        event.id = format!("{sequence:032x}");
        event.message["id"] = json!(event.id);
        event.message["sequence"] = json!(sequence.to_string());
        validator.row(event.clone()).unwrap();
        assert_eq!(validator.actual.len(), 1);
    }
    validator.head(&topic().id, 10_000).unwrap();
    assert_eq!(validator.finish().unwrap(), 10_000);
    assert!(validator.finish().is_err());
    assert_eq!(JournalValidator::default().finish().unwrap(), 0);
}

#[test]
fn restore_rejects_gaps_mismatched_heads_and_reuse_after_failure() {
    let registry = registry();
    for bad in [0, 2] {
        let mut validator = JournalValidator::default();
        validator.row(row(&registry, &topic(), 1)).unwrap();
        assert!(validator.head(&topic().id, bad).is_err());
        assert!(validator.head(&topic().id, 1).is_err());
        assert!(validator.finish().is_err());
    }
    let mut validator = JournalValidator::default();
    assert!(validator.row(row(&registry, &topic(), 2)).is_err());
    assert!(validator.row(row(&registry, &topic(), 1)).is_err());
    let mut validator = JournalValidator::default();
    validator.row(row(&registry, &topic(), 1)).unwrap();
    assert!(validator.finish().is_err());
    let mut validator = JournalValidator::default();
    validator.row(row(&registry, &topic(), 1)).unwrap();
    validator.head(&topic().id, 1).unwrap();
    assert!(validator.row(row(&registry, &topic(), 2)).is_err());
    assert!(JournalValidator::default().head(&topic().id, 0).is_err());
}

#[test]
fn restore_accepts_database_topic_collation_without_assuming_rust_sort_order() {
    let mut other = topic();
    other.id = "aaa.changed".into();
    let registry = Registry::new(vec![topic(), other.clone()]).unwrap();
    let mut validator = JournalValidator::default();
    validator.row(row(&registry, &topic(), 1)).unwrap();
    validator.row(row(&registry, &other, 1)).unwrap();
    validator.head(&other.id, 1).unwrap();
    validator.head(&topic().id, 1).unwrap();
    assert_eq!(validator.finish().unwrap(), 2);
    let mut validator = JournalValidator::default();
    validator.row(row(&registry, &topic(), 1)).unwrap();
    validator.row(row(&registry, &other, 1)).unwrap();
    assert!(validator.row(row(&registry, &topic(), 2)).is_err());
}
