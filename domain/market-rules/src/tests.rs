use super::*;
use serde_json::json;
fn fixture() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/rule.json")).unwrap()
}
fn basis_fixture() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/basis.json")).unwrap()
}
fn decimal(value: &str) -> Decimal {
    value.parse().unwrap()
}
fn day(value: &str) -> NaiveDate {
    value.parse().unwrap()
}
#[test]
fn rule_snapshots_match_python_golden_including_scale_exponents_and_whitespace() {
    let golden: Vec<Value> =
        serde_json::from_str(include_str!("../tests/fixtures/rule_golden.json")).unwrap();
    for case in golden {
        let mut input = fixture();
        for (key, value) in case["updates"].as_object().unwrap() {
            input[key] = value.clone();
        }
        let rules = Rules::from_value(input).unwrap();
        assert_eq!(rules.id(), case["id"].as_str().unwrap(), "{}", case["name"]);
        let normalized = output(rules.spec()).unwrap();
        for (key, value) in case["normalized_updates"].as_object().unwrap() {
            assert_eq!(&normalized[key], value, "{key}");
        }
    }
}
#[test]
fn rule_period_boundaries_coverage_and_identity_are_exact() {
    let mut value = fixture();
    value["periods"][0]["end"] = json!("2024-01-03");
    let mut second = value["periods"][0].clone();
    second["start"] = json!("2024-01-04");
    second["end"] = json!("2024-12-31");
    second["open_fee"] = json!("3");
    value["periods"].as_array_mut().unwrap().push(second);
    let rules = Rules::from_value(value).unwrap();
    assert_eq!(rules.at(day("2024-01-03")).unwrap().open_fee, decimal("2"));
    assert_eq!(rules.at(day("2024-01-04")).unwrap().open_fee, decimal("3"));
    assert!(rules.at(day("2023-12-31")).is_err());
    assert!(rules.at(day("2025-01-01")).is_err());
    rules
        .cover(
            &rules.spec().contract.id,
            day("2024-01-01"),
            day("2024-05-15"),
        )
        .unwrap();
    assert!(
        rules
            .cover(
                "SHFE.RB.212405.21230516",
                day("2024-01-01"),
                day("2024-01-02")
            )
            .is_err()
    );
    assert!(
        rules
            .cover(
                &rules.spec().contract.id,
                day("2024-05-15"),
                day("2024-05-16")
            )
            .is_err()
    );
    assert!(
        rules
            .cover(
                &rules.spec().contract.id,
                day("2024-01-02"),
                day("2024-01-01")
            )
            .is_err()
    );
    assert_eq!(
        rules
            .fee(day("2024-01-04"), &decimal("200"), 2, true, 40)
            .unwrap(),
        decimal("6")
    );
}
#[test]
fn rule_periods_reject_gaps_overlap_invalid_rates_and_bad_fingerprints() {
    for start in ["2024-01-10", "2024-01-12", "2024-01-01"] {
        let mut value = fixture();
        value["periods"][0]["end"] = json!("2024-01-10");
        let mut p = value["periods"][0].clone();
        p["start"] = json!(start);
        p["end"] = json!("2024-01-20");
        value["periods"].as_array_mut().unwrap().push(p);
        assert!(Rules::from_value(value).is_err(), "{start}");
    }
    for (pointer, value) in [
        ("/periods/0/margin_rate", json!("0")),
        ("/periods/0/margin_rate", json!("1.00000001")),
        ("/periods/0/open_fee", json!("-0.1")),
        ("/periods/0/close_fee", json!("1000001")),
        ("/multiplier", json!("0")),
        ("/tick_size", json!("NaN")),
        ("/tick_size", json!("1e-9")),
        ("/title", json!(" ")),
    ] {
        let mut v = fixture();
        *v.pointer_mut(pointer).unwrap() = value;
        assert!(Rules::from_value(v).is_err(), "{pointer}");
    }
    let mut value = fixture();
    value["periods"][0]["fee_mode"] = json!("notional");
    assert!(Rules::from_value(value).is_err());
    let mut snapshot = invoke("snapshot", json!({"spec":fixture()})).unwrap();
    snapshot["spec"]["source"] = json!("changed");
    assert!(invoke("validate", json!({"model":"RuleVersion","value":snapshot})).is_err());
}
#[test]
fn nullable_fields_are_required_and_unknown_fields_rejected() {
    for pointer in [
        "/basis",
        "/periods/0/settlement_basis",
        "/contract/last_delivery_on",
    ] {
        let mut value = fixture();
        let (parent, key) = pointer.rsplit_once('/').unwrap();
        value
            .pointer_mut(parent)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .remove(key);
        assert!(Rules::from_value(value).is_err(), "{pointer}");
    }
    let mut value = fixture();
    value["periods"][0]["unknown"] = json!(0);
    assert!(Rules::from_value(value).is_err());
    for pointer in [
        "/evidence/connection_id",
        "/evidence/row/settle",
        "/evidence/row/offset_today_fee",
    ] {
        let mut value = basis_fixture();
        let (parent, key) = pointer.rsplit_once('/').unwrap();
        value
            .pointer_mut(parent)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .remove(key);
        assert!(parsed::<SettlementBasis>(value).is_err(), "{pointer}");
    }
}
#[test]
fn settlement_values_preserve_python_decimal_rounding_and_normalized_evidence() {
    let golden: Value =
        serde_json::from_str(include_str!("../tests/fixtures/decimal_golden.json")).unwrap();
    for case in golden["settlement"].as_array().unwrap() {
        let basis: SettlementBasis = parsed(case["input"].clone()).unwrap();
        assert_eq!(output(&basis).unwrap(), case["normalized"]);
        assert_eq!(output(basis.values(28).unwrap()).unwrap(), case["values"]);
    }
}
#[test]
fn fees_match_python_at_28_and_40_digit_half_even_with_each_operation_rounded() {
    let golden: Value =
        serde_json::from_str(include_str!("../tests/fixtures/decimal_golden.json")).unwrap();
    for case in golden["fees"].as_array().unwrap() {
        let mut value = fixture()["periods"][0].clone();
        value["fee_mode"] = json!("notional");
        value["open_fee"] = case["rate"].clone();
        value["close_fee"] = case["rate"].clone();
        let period: RulePeriod = parsed(value).unwrap();
        let fee = period
            .fee(
                &decimal(case["price"].as_str().unwrap()),
                &decimal(case["multiplier"].as_str().unwrap()),
                case["lots"].as_i64().unwrap(),
                true,
                case["precision"].as_u64().unwrap() as u32,
            )
            .unwrap();
        assert_eq!(fee.to_string(), case["fee"].as_str().unwrap(), "{case}");
    }
}
#[test]
fn settlement_confirmation_requires_units_parameters_causality_and_known_values() {
    let basis: SettlementBasis = parsed(basis_fixture()).unwrap();
    let period = basis
        .period(day("2024-01-01"), day("2024-01-31"), 28)
        .unwrap();
    assert_eq!(period.open_fee, decimal("0.0005"));
    assert_eq!(period.margin_rate, decimal("0.12"));
    assert!(
        basis
            .period(day("2023-12-29"), day("2024-01-31"), 28)
            .is_err()
    );
    assert!(
        basis
            .period(day("2023-12-28"), day("2024-01-31"), 28)
            .is_err()
    );
    for (pointer, change) in [
        ("/fee_unit", json!("yuan_per_lot")),
        ("/margin_unit", json!("ratio")),
        ("/interpretation", json!(" ")),
        ("/evidence/row/long_margin_rate", Value::Null),
        ("/evidence/row/trading_fee_rate", Value::Null),
        ("/evidence/row/trading_fee", json!("-1")),
        ("/evidence/row/contract", json!("DCE.m2405")),
        ("/evidence/row/exchange", json!("DCE")),
        ("/evidence/row/trading_day", json!("2024-05-16")),
    ] {
        let mut value = basis_fixture();
        *value.pointer_mut(pointer).unwrap() = change;
        assert!(parsed::<SettlementBasis>(value).is_err(), "{pointer}");
    }
    let mut changed = period;
    changed.open_fee = decimal("0.01");
    assert!(changed.validate().is_err());
}
#[test]
fn source_basis_binds_actual_lifecycle_without_source_lookup() {
    let mut value = fixture();
    let c = value["contract"].clone();
    let basis = json!({"provider":"independent","contract_id":c["id"],"version_id":"fixed-source","checksum":"a".repeat(64),"connection_id":null,"symbol":"opaque:42","exchange":"SHFE","name":"supplied title","listed":c["listed_on"],"delisted":c["last_trade_on"],"trade_unit":null,"per_unit":null,"multiplier":null,"quote_unit":null,"quote_unit_desc":null});
    value["basis"] = basis;
    Rules::from_value(value.clone()).unwrap();
    for (pointer, change) in [
        ("/basis/contract_id", json!("SHFE.RB.202405.20230517")),
        ("/basis/listed", json!("2023-05-17")),
        ("/basis/delisted", Value::Null),
        ("/basis/exchange", json!("DCE")),
        ("/basis/connection_id", json!("invalid")),
    ] {
        let mut changed = value.clone();
        *changed.pointer_mut(pointer).unwrap() = change;
        assert!(Rules::from_value(changed).is_err(), "{pointer}");
    }
    let mut invalid = fixture();
    invalid["contract"]["product_id"] = json!("DCE.M");
    invalid["contract"]["id"] = json!("DCE.M.202405.20230516");
    assert!(Rules::from_value(invalid).is_err());
}
#[test]
fn signed_zero_tail_zeros_and_scientific_notation_are_preserved() {
    for (input, expected) in [
        ("-0.00", "-0.00"),
        ("0E+3", "0E+3"),
        ("0.0000000", "0E-7"),
        ("0.00000010", "1.0E-7"),
        ("1E-6", "0.000001"),
        ("1E-7", "1E-7"),
        ("1E+2", "1E+2"),
        ("001.2300", "1.2300"),
        ("+1.00", "1.00"),
    ] {
        assert_eq!(decimal(input).to_string(), expected);
    }
    assert_eq!(
        decimal("0.050")
            .divide_power_ten(2, 28)
            .unwrap()
            .to_string(),
        "0.0005"
    );
    assert_eq!(
        decimal("5.000")
            .divide_power_ten(2, 28)
            .unwrap()
            .to_string(),
        "0.050"
    );
    assert_eq!(
        decimal("1E+2").divide_power_ten(2, 28).unwrap().to_string(),
        "1"
    );
    assert_eq!(
        decimal("-0.00")
            .divide_power_ten(2, 28)
            .unwrap()
            .to_string(),
        "-0.00"
    );
    assert_eq!(decimal("0.100000000").constraints(true, 1, 9, 8), Ok(()));
}
#[test]
fn nonfinite_overflow_precision_and_numeric_json_are_handled_without_floats() {
    for input in [
        "NaN",
        "sNaN",
        "Infinity",
        "-Infinity",
        "1E999999999999999999",
        "1E+1000000",
    ] {
        assert!(input.parse::<Decimal>().is_err(), "{input}");
    }
    for precision in [0, 1001] {
        assert!(decimal("1").multiply(&decimal("2"), precision).is_err());
    }
    assert!(decimal("1E+999999").multiply(&decimal("10"), 28).is_err());
    let value: Decimal = serde_json::from_str("1234567890123456789012345678.12345678").unwrap();
    assert_eq!(value.to_string(), "1234567890123456789012345678.12345678");
    assert!(serde_json::from_value::<Decimal>(json!(true)).is_err());
}
#[test]
fn schema_has_one_rule_period_and_reuses_foreign_contracts() {
    let schema = schema();
    let defs = &schema["$defs"];
    for name in [
        "RulePeriod",
        "ContractBasis",
        "RuleSpec",
        "RuleVersion",
        "SettlementRow",
        "SettlementEvidence",
        "SettlementBasis",
    ] {
        assert_eq!(defs[name]["additionalProperties"], false, "{name}");
    }
    assert_eq!(
        defs["RuleSpec"]["properties"]["multiplier"]["format"],
        "decimal"
    );
    assert!(
        defs["RuleSpec"]["required"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == "basis")
    );
    assert!(
        defs["Contract"]["required"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == "last_delivery_on")
    );
    assert!(
        defs["SettlementRow"]["required"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == "settle")
    );
    assert!(defs.get("TimeVersion").is_some());
    assert!(defs.get("Period").is_some());
}

#[test]
fn typed_construction_normalizes_only_declared_text_fields() {
    let mut spec: RuleSpec = serde_json::from_value(fixture()).unwrap();
    spec.title = "  title  ".into();
    spec.source = " source ".into();
    spec.contract.provenance.source = " evidence ".into();
    let rules = Rules::new(spec).unwrap();
    assert_eq!(rules.spec().title, "title");
    assert_eq!(rules.spec().source, "source");
    assert_eq!(rules.spec().contract.provenance.source, " evidence ");
    let mut basis: SettlementBasis = parsed(basis_fixture()).unwrap();
    basis.interpretation = "  explicit  ".into();
    assert_eq!(
        SettlementBasis::new(basis).unwrap().interpretation,
        "explicit"
    );
    let schema = schema();
    assert_eq!(schema["$defs"]["RuleSpec"]["x-strip-whitespace"], true);
    assert_eq!(
        schema["$defs"]["SettlementBasis"]["x-strip-whitespace"],
        true
    );
    assert!(
        schema["$defs"]["SettlementEvidence"]
            .get("x-strip-whitespace")
            .is_none()
    );
}

#[test]
fn decimal_carry_subnormal_and_signed_zero_exponents_match_explicit_context() {
    for (precision, a, b, expected) in [
        (
            28,
            "9.9999999999999999999999999995",
            "1",
            "10.00000000000000000000000000",
        ),
        (
            40,
            "9.9999999999999999999999999995",
            "1",
            "9.9999999999999999999999999995",
        ),
        (28, "1E-999999", "1E-40", "0E-1000026"),
        (40, "1E-999999", "1E-40", "0E-1000038"),
        (28, "1E-1000027", "1", "0E-1000026"),
        (40, "1E-1000027", "1", "1E-1000027"),
        (28, "-0E-1000040", "1", "-0E-1000026"),
        (40, "-0E-1000040", "1", "-0E-1000038"),
        (28, "0E+999999", "1E+999999", "0E+999999"),
    ] {
        assert_eq!(
            decimal(a)
                .multiply(&decimal(b), precision)
                .unwrap()
                .to_string(),
            expected
        );
    }
}
