use super::*;
use serde_json::json;
fn fixture() -> Value {
    serde_json::from_str(include_str!("../tests/fixtures/valid.json")).unwrap()
}
fn day(value: &str) -> NaiveDate {
    value.parse().unwrap()
}
fn query(day: &str) -> ResolutionRequest {
    parsed(json!({"source":"tushare","symbol":"RB2610.SHF","trading_day":day,"information_at":"2026-09-20T00:00:00Z"})).unwrap()
}
fn identity() -> Value {
    let mut catalog = fixture();
    catalog["inputs"] =
        json!([{"version_id":"source-fixed","checksum":"a".repeat(64),"source":"tushare"}]);
    let id = Catalog::from_value(catalog.clone())
        .unwrap()
        .id()
        .to_owned();
    json!({"catalog_id":id,"catalog":catalog,"source":"tushare","symbol":"RB2610.SHF","information_at":"2026-09-20T00:00:00Z"})
}
fn import_identity() -> Value {
    let catalog = fixture();
    let id = Catalog::from_value(catalog.clone())
        .unwrap()
        .id()
        .to_owned();
    json!({"catalog_id":id,"catalog":catalog,"information_at":"2026-09-20T00:00:00Z","bindings":[{"contract":"SHFE.rb2610","source":"tushare","symbol":"RB2610.SHF"}]})
}
#[test]
fn normalized_roundtrip_and_python_fingerprint() {
    let catalog = Catalog::from_value(fixture()).unwrap();
    assert_eq!(output(catalog.model()).unwrap(), fixture());
    assert_eq!(
        catalog.id(),
        "d78ed94c3dc6e6d20f156c16a5d05cc2c8246a453b57594fc48531716d203ca0"
    );
    let mut equivalent = fixture();
    equivalent["products"][0]["provenance"]["observed_at"] =
        json!("2026-01-01T00:00:00.000000+00:00");
    assert_eq!(Catalog::from_value(equivalent).unwrap().id(), catalog.id());
}
#[test]
fn required_nullable_and_unknown_fields_are_strict() {
    for pointer in [
        "/schema_version",
        "/inputs",
        "/contracts/0/last_delivery_on",
    ] {
        let mut value = fixture();
        let (parent, key) = pointer.rsplit_once('/').unwrap();
        value
            .pointer_mut(parent)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .remove(key);
        assert!(Catalog::from_value(value).is_err(), "{pointer}");
    }
    let mut value = fixture();
    value["contracts"][0]["extra"] = json!(true);
    assert!(Catalog::from_value(value).is_err());
    for version in [json!(1), json!(3), json!(true), json!(2.0), json!("2")] {
        let mut value = fixture();
        value["schema_version"] = version;
        assert!(Catalog::from_value(value).is_err());
    }
}
#[test]
fn all_lifecycle_and_reference_invariants_are_enforced() {
    let changes = [
        ("/contracts/0/id", json!("SHFE.RB.202610")),
        ("/contracts/0/product_id", json!("DCE.M")),
        ("/contracts/0/delivery_month", json!("0000-10")),
        ("/contracts/0/last_trade_on", json!("2025-09-30")),
        ("/contracts/0/last_delivery_on", json!("2026-10-14")),
        ("/symbols/0/contract_id", json!("missing")),
        ("/symbols/0/valid_from", json!("2025-09-30")),
        ("/symbols/0/valid_until", json!("2026-10-16")),
        ("/products/0/exchange", json!("DCE")),
        (
            "/products/0/provenance/available_at",
            json!("2025-01-01T00:00:00Z"),
        ),
    ];
    for (pointer, change) in changes {
        let mut value = fixture();
        *value.pointer_mut(pointer).unwrap() = change;
        assert!(Catalog::from_value(value).is_err(), "{pointer}");
    }
    for key in ["products", "contracts", "symbols"] {
        let mut value = fixture();
        let item = value[key][0].clone();
        value[key].as_array_mut().unwrap().push(item);
        assert!(Catalog::from_value(value).is_err(), "{key}");
    }
}
#[test]
fn resolution_uses_inclusive_intervals_and_all_three_knowledge_times() {
    let catalog = Catalog::from_value(fixture()).unwrap();
    for day in ["2025-10-01", "2026-04-22", "2026-10-15"] {
        assert_eq!(
            catalog.resolve(&query(day)).unwrap().contract.id,
            "SHFE.RB.202610.20251001"
        );
    }
    for day in ["2025-09-30", "2026-10-16"] {
        assert!(catalog.resolve(&query(day)).is_err());
    }
    for key in ["products", "contracts", "symbols"] {
        let mut value = fixture();
        value[key][0]["provenance"]["available_at"] = json!("2026-09-20T00:00:00.000001Z");
        assert!(
            Catalog::from_value(value)
                .unwrap()
                .resolve(&query("2026-04-22"))
                .is_err(),
            "{key}"
        );
    }
    let mut wrong = query("2026-04-22");
    wrong.symbol = "rb2610.SHF".into();
    assert!(catalog.resolve(&wrong).is_err());
}
#[test]
fn short_codes_do_not_infer_century_and_reused_symbols_keep_lifecycles() {
    let mut value = fixture();
    value["products"][0]["id"] = json!("CZCE.MA");
    value["products"][0]["exchange"] = json!("CZCE");
    let base = value["contracts"][0].clone();
    let mapping = value["symbols"][0].clone();
    value["contracts"] = json!([]);
    value["symbols"] = json!([]);
    for year in [2026, 2036] {
        let id = format!("CZCE.MA.{year}05.{}0101", year - 1);
        let mut c = base.clone();
        c["id"] = json!(id);
        c["product_id"] = json!("CZCE.MA");
        c["delivery_month"] = json!(format!("{year}-05"));
        c["listed_on"] = json!(format!("{}-01-01", year - 1));
        c["last_trade_on"] = json!(format!("{year}-05-15"));
        let mut m = mapping.clone();
        m["contract_id"] = json!(id);
        m["symbol"] = json!("MA605.ZCE");
        m["valid_from"] = c["listed_on"].clone();
        m["valid_until"] = c["last_trade_on"].clone();
        value["contracts"].as_array_mut().unwrap().push(c);
        value["symbols"].as_array_mut().unwrap().push(m);
    }
    let catalog = Catalog::from_value(value).unwrap();
    for year in [2026, 2036] {
        let mut q = query(&format!("{year}-03-01"));
        q.symbol = "MA605.ZCE".into();
        let actual = catalog.resolve(&q).unwrap().contract;
        assert_eq!(actual.delivery_month, format!("{year}-05"));
        validate_market_code("CZCE.ma605", &actual).unwrap();
        assert!(validate_market_code("CZCE.ma604", &actual).is_err());
    }
    let mut q = query("2030-03-01");
    q.symbol = "MA605.ZCE".into();
    assert!(catalog.resolve(&q).is_err());
}
#[test]
fn source_identity_pins_source_fingerprint_and_single_lifecycle() {
    let resolver = SourceResolver::from_value(identity()).unwrap();
    let row = json!({"symbol":"RB2610.SHF","exchange":"SHFE","contract":"SHFE.rb2610","trading_day":"2026-04-22","close":3900.5});
    assert_eq!(
        resolver
            .validate_rows(std::slice::from_ref(&row), "SHFE")
            .unwrap(),
        vec!["SHFE.RB.202610.20251001"]
    );
    assert!(resolver.validate_rows(&[], "SHFE").is_err());
    assert!(
        resolver
            .validate_rows(std::slice::from_ref(&row), "DCE")
            .is_err()
    );
    for (key, change) in [
        ("symbol", json!("rb2610.SHF")),
        ("contract", json!("SHFE.rb2609")),
        ("trading_day", json!("2027-04-22")),
    ] {
        let mut row = row.clone();
        row[key] = change;
        assert!(resolver.validate_rows(&[row], "SHFE").is_err());
    }
    let mut value = identity();
    value["catalog"]["contracts"][0]["last_delivery_on"] = json!("2026-10-20");
    assert!(SourceResolver::from_value(value).is_err());
    let mut value = identity();
    value["source"] = json!("other");
    assert!(SourceResolver::from_value(value).is_err());
}
#[test]
fn import_mapping_is_exact_and_batch_set_must_match() {
    let resolver = ImportResolver::from_value(import_identity()).unwrap();
    assert!(resolver.resolve("SHFE.rb2610", day("2026-04-22")).is_ok());
    assert!(resolver.resolve("SHFE.RB2610", day("2026-04-22")).is_err());
    assert!(resolver.resolve("SHFE.rb2610", day("2027-04-22")).is_err());
    assert!(resolver.validate_rows(&[]).is_err());
    resolver
        .validate_rows(&[json!({"contract":"SHFE.rb2610","trading_day":"2026-04-22"})])
        .unwrap();
    assert!(
        resolver
            .validate_rows(&[
                json!({"contract":"SHFE.rb2610","trading_day":"2026-04-22"}),
                json!({"contract":"SHFE.rb2609","trading_day":"2026-04-22"})
            ])
            .is_err()
    );
    let mut value = import_identity();
    let binding = value["bindings"][0].clone();
    value["bindings"].as_array_mut().unwrap().push(binding);
    assert!(ImportResolver::from_value(value).is_err());
}
#[test]
fn release_rejects_changed_content_and_normalizes_datetimes() {
    let release = invoke(
        "snapshot",
        json!({"catalog":fixture(),"published_at":123.125}),
    )
    .unwrap();
    invoke(
        "validate",
        json!({"model":"ReferenceRelease","value":release.clone()}),
    )
    .unwrap();
    let mut damaged = release;
    damaged["catalog"]["products"][0]["name"] = json!("changed");
    assert!(
        invoke(
            "validate",
            json!({"model":"ReferenceRelease","value":damaged})
        )
        .is_err()
    );
    let provenance = json!({"source":"x","source_version":"y","observed_at":"2026-01-01T08:00:00.001+08:00","available_at":"2026-01-01T00:00:00.1234567Z"});
    let result = invoke("validate", json!({"model":"Provenance","value":provenance})).unwrap();
    assert_eq!(result["observed_at"], "2026-01-01T08:00:00.001000+08:00");
    assert_eq!(result["available_at"], "2026-01-01T00:00:00.123456Z");
    assert!(invoke("snapshot", json!({"catalog":fixture()})).is_err());
}
#[test]
fn schema_covers_public_models_and_required_nullable() {
    let schema = schema();
    let contract = &schema["$defs"]["Contract"];
    assert!(
        contract["required"]
            .as_array()
            .unwrap()
            .iter()
            .any(|s| s == "last_delivery_on")
    );
    assert_eq!(contract["additionalProperties"], false);
    assert!(
        contract["properties"]["last_delivery_on"]["type"]
            .as_array()
            .unwrap()
            .iter()
            .any(|t| t == "null")
    );
    for name in [
        "ReferenceCatalog",
        "SourceIdentity",
        "ImportIdentity",
        "ReferenceRelease",
    ] {
        assert!(schema["$defs"].get(name).is_some());
    }
}

#[test]
fn typed_market_code_cannot_accept_forged_contract() {
    let mut actual = Catalog::from_value(fixture()).unwrap().model().contracts[0].clone();
    actual.id = "forged".into();
    assert!(validate_market_code("SHFE.rb2610", &actual).is_err());
}
