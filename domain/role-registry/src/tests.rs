use super::*;
use serde_json::json;
fn provider() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/provider.json")).unwrap()
}
fn query() -> RoleQuery {
    parsed(serde_json::from_str(include_str!("../tests/fixtures/query.json")).unwrap()).unwrap()
}
fn computed() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/computed.json")).unwrap()
}
fn dt(value: &str) -> DateTime<FixedOffset> {
    DateTime::parse_from_rfc3339(value).unwrap()
}
#[test]
fn provider_snapshot_and_resolution_match_current_python_goldens() {
    let version: RoleVersion = parsed(provider()).unwrap();
    let registry = RoleRegistry::new(version.spec).unwrap();
    assert_eq!(
        registry.id(),
        "dbe57073bf4541d62bb0aeddea6281f1044d1df6b7f9b9fac7ad8c565eefc3ef"
    );
    let expected: Value =
        serde_json::from_str(include_str!("../tests/fixtures/provider_resolution.json")).unwrap();
    assert_eq!(
        output(registry.resolve(&query()).unwrap()).unwrap(),
        expected
    );
    assert_eq!(
        registry
            .resolve(&query())
            .unwrap()
            .session
            .trading_day
            .to_string(),
        "2025-04-14"
    );
}
#[test]
fn reports_require_correct_identity_calendar_knowledge_and_uniqueness() {
    for (pointer, value) in [
        ("/product_id", json!("SHFE.RB")),
        ("/reports/0/contract_id", json!("missing")),
        ("/reports/0/trading_day", json!("2026-04-14")),
        ("/reports/0/trading_day", json!("2025-04-13")),
        ("/reports/0/available_at", json!("2026-04-14T00:00:00Z")),
        ("/origin", json!("computed")),
    ] {
        let mut spec = provider()["spec"].clone();
        *spec.pointer_mut(pointer).unwrap() = value;
        assert!(RoleRegistry::from_value(spec).is_err(), "{pointer}");
    }
    for role in ["main", "secondary"] {
        let mut spec = provider()["spec"].clone();
        let mut row = spec["reports"][0].clone();
        row["role"] = json!(role);
        spec["reports"].as_array_mut().unwrap().push(row);
        assert!(RoleRegistry::from_value(spec).is_err());
    }
}
#[test]
fn known_and_retrospective_queries_never_fill_missing_roles_or_future_evidence() {
    let registry = RoleRegistry::from_value(provider()["spec"].clone()).unwrap();
    let mut q = query();
    q.role = Role::Secondary;
    assert!(registry.resolve(&q).unwrap_err().contains("缺口"));
    q = query();
    q.timestamp = dt("2025-04-10T21:00:00+08:00");
    q.information_at = q.timestamp;
    assert!(registry.resolve(&q).unwrap_err().contains("缺口"));
    let mut spec = provider()["spec"].clone();
    spec["reports"][0]["available_at"] = Value::Null;
    let registry = RoleRegistry::from_value(spec).unwrap();
    let mut q = query();
    q.version_id = registry.id().into();
    assert!(registry.resolve(&q).unwrap_err().contains("可知"));
    q.mode = RoleMode::Retrospective;
    q.explanation = "Explicit historical view".into();
    assert!(registry.resolve(&q).unwrap_err().contains("观测"));
    q.information_at = dt("2025-04-16T00:00:00Z");
    assert!(!registry.resolve(&q).unwrap().execution_authorized);
    q.mode = RoleMode::AsKnown;
    assert!(q.validate().unwrap_err().contains("未来信息"));
    q.mode = RoleMode::Retrospective;
    q.explanation.clear();
    assert!(q.validate().is_err());
    for group in ["products", "contracts"] {
        let mut spec = provider()["spec"].clone();
        spec["catalog"][group][0]["provenance"]["available_at"] = json!("2026-01-01T00:00:00Z");
        let registry = RoleRegistry::from_value(spec).unwrap();
        let mut q = query();
        q.version_id = registry.id().into();
        assert!(registry.resolve(&q).unwrap_err().contains("身份资料"));
    }
}
#[test]
fn next_opening_uses_confirmed_day_openings_and_never_intraday_or_inferred_dates() {
    let version: RoleVersion = parsed(provider()).unwrap();
    let model = RoleCalendar {
        product_id: version.spec.product_id,
        trading_time: version.spec.trading_time,
    };
    let calendar = OpeningCalendar::new(model).unwrap();
    for (end, available, start, day) in [
        (
            "2025-04-03T15:00:00+08:00",
            "2025-04-03T15:05:00+08:00",
            "2025-04-07T09:00:00+08:00",
            "2025-04-07",
        ),
        (
            "2025-04-11T15:00:00+08:00",
            "2025-04-11T15:05:00+08:00",
            "2025-04-11T21:00:00+08:00",
            "2025-04-14",
        ),
        (
            "2025-04-07T15:00:00+08:00",
            "2025-04-07T21:01:00+08:00",
            "2025-04-08T21:00:00+08:00",
            "2025-04-09",
        ),
    ] {
        let actual = calendar.next_opening(dt(end), dt(available)).unwrap();
        assert_eq!(actual.start, dt(start));
        assert_eq!(actual.trading_day.to_string(), day);
    }
    for (end, available) in [
        ("2025-04-14T15:00:00+08:00", "2025-04-14T16:00:00+08:00"),
        ("2025-04-11T15:00:00+08:00", "2025-04-11T14:59:00+08:00"),
        ("2024-01-01T15:00:00+08:00", "2024-01-01T16:00:00+08:00"),
    ] {
        assert!(calendar.next_opening(dt(end), dt(available)).is_err());
    }
}
#[test]
fn computed_role_projection_resolves_without_knowing_ranking_algorithms() {
    let registry = ComputedRegistry::from_value(computed()).unwrap();
    let mut q = query();
    q.version_id = registry.model().version_id.clone();
    let result = registry.resolve(&q).unwrap();
    assert_eq!(result.record_index, 1);
    assert_eq!(result.contract.id, "SHFE.AU.202508.20240617");
    let mut result = output(result).unwrap();
    result.as_object_mut().unwrap().remove("record_index");
    let expected: Value =
        serde_json::from_str(include_str!("../tests/fixtures/computed_resolution.json")).unwrap();
    assert_eq!(result, expected);
    q.role = Role::Secondary;
    assert_eq!(
        registry.resolve(&q).unwrap().contract.id,
        "SHFE.AU.202506.20240617"
    );
}
#[test]
fn computed_roles_reject_auction_late_publication_gaps_and_unknown_identity() {
    let registry = ComputedRegistry::from_value(computed()).unwrap();
    let mut q = query();
    q.version_id = registry.model().version_id.clone();
    q.timestamp = dt("2025-04-11T20:56:00+08:00");
    q.information_at = q.timestamp;
    assert!(registry.resolve(&q).unwrap_err().contains("尚未生效"));
    q.timestamp = dt("2025-04-11T21:00:00+08:00");
    q.information_at = q.timestamp;
    let mut index = computed();
    index["published_at"] = json!("2026-04-11T20:00:00+08:00");
    assert!(
        ComputedRegistry::from_value(index)
            .unwrap()
            .resolve(&q)
            .unwrap_err()
            .contains("发布晚于")
    );
    let mut index = computed();
    index["records"].as_array_mut().unwrap().pop();
    assert!(
        ComputedRegistry::from_value(index)
            .unwrap()
            .resolve(&q)
            .unwrap_err()
            .contains("缺口")
    );
    q.mode = RoleMode::Retrospective;
    q.explanation = "retrospective".into();
    assert!(registry.resolve(&q).unwrap_err().contains("不提供"));
    for (pointer, value) in [
        ("/records/0/main", json!("missing")),
        (
            "/records/0/effective_start",
            json!("2025-04-11T09:01:00+08:00"),
        ),
        ("/records/0/available_at", json!("2026-04-11T00:00:00Z")),
    ] {
        let mut index = computed();
        *index.pointer_mut(pointer).unwrap() = value;
        assert!(ComputedRegistry::from_value(index).is_err(), "{pointer}");
    }
}
#[test]
fn strict_fields_fingerprints_and_non_authorizing_outputs_are_enforced() {
    let mut version = provider();
    version["spec"]["source"] = json!("changed");
    assert!(parsed::<RoleVersion>(version).is_err());
    for field in ["available_at", "evidence"] {
        let mut spec = provider()["spec"].clone();
        spec["reports"][0].as_object_mut().unwrap().remove(field);
        assert!(RoleRegistry::from_value(spec).is_err());
    }
    let mut spec = provider()["spec"].clone();
    spec["reports"][0]["unused"] = json!(1);
    assert!(RoleRegistry::from_value(spec).is_err());
    let mut resolution: Value =
        serde_json::from_str(include_str!("../tests/fixtures/provider_resolution.json")).unwrap();
    resolution["execution_authorized"] = json!(true);
    assert!(parsed::<RoleResolution>(resolution).is_err());
    let schema = schema();
    assert!(
        schema["$defs"]["RoleReport"]["required"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == "available_at")
    );
    assert_eq!(
        schema["$defs"]["RoleResolution"]["properties"]["execution_authorized"]["const"],
        false
    );
}
