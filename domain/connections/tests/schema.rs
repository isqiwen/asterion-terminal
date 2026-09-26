use asterion_connections::{MAX_TEXT_BYTES, invoke};
use serde_json::{Value, json};

fn validate(model: &str, value: Value) -> Result<Value, String> {
    invoke("validate", json!({"model":model,"value":value}))
}

#[test]
fn schema_exposes_current_value_types_defaults_and_typed_maps_only() {
    let schema = invoke("schema", json!({})).unwrap();
    let definitions = schema["$defs"].as_object().unwrap();
    for name in [
        "Subscription",
        "Exchange",
        "ConfigField",
        "ConnectorDescriptor",
        "ConnectionProfile",
        "Channel",
        "Feature",
        "ConnectionStatus",
        "ChannelState",
        "ReadRequest",
        "ReadBatch",
    ] {
        assert!(definitions.contains_key(name), "{name}");
    }
    for private in [
        "Saved",
        "Stored",
        "Save",
        "SecretChange",
        "Manager",
        "ReadTicket",
    ] {
        assert!(!definitions.contains_key(private));
    }
    assert_eq!(
        definitions["ConnectionProfile"]["properties"]["config"]["additionalProperties"]["type"],
        "string"
    );
    assert_eq!(
        definitions["ConfigField"]["properties"]["required"]["default"],
        true
    );
    assert_eq!(
        definitions["ConfigField"]["properties"]["secret"]["default"],
        false
    );
    assert_eq!(
        definitions["ConnectorDescriptor"]["properties"]["version"]["const"],
        1
    );
    assert_eq!(
        definitions["ReadBatch"]["properties"]["complete"]["const"],
        true
    );
    assert_eq!(
        definitions["ChannelState"]["properties"]["state"]["default"],
        "disconnected"
    );
    assert!(invoke("schema", json!({"unknown":true})).is_err());
}

#[test]
fn explicit_defaults_and_map_values_are_checked_by_the_same_rust_models() {
    let field = validate("ConfigField", json!({"key":"account","label":"账户"})).unwrap();
    assert_eq!(
        field,
        json!({"key":"account","label":"账户","required":true,"secret":false,"identity":false,"default":""})
    );
    for invalid in [
        json!({"key":"BAD","label":"账户"}),
        json!({"key":"valid","label":"账户","secret":true,"default":"secret"}),
        json!({"key":"valid","label":"账户","extra":true}),
    ] {
        assert!(validate("ConfigField", invalid).is_err());
    }
    assert!(
        validate(
            "ConfigField",
            json!({"key":"valid","label":"x".repeat(MAX_TEXT_BYTES+1)})
        )
        .is_err()
    );
    let profile = json!({"connection_id":"a".repeat(32),"connector_id":"test","name":"账户","config_revision":1,"config":{"account":"测试账户"}});
    assert_eq!(
        validate("ConnectionProfile", profile.clone()).unwrap(),
        profile
    );
    let mut bad = profile;
    bad["config"]["account"] = json!(null);
    assert!(validate("ConnectionProfile", bad).is_err());
    assert!(validate("Unknown", json!({})).is_err());
}

#[test]
fn read_envelopes_require_all_fields_and_valid_identity_time_and_completion() {
    let valid = json!({"connection_id":"a".repeat(32),"generation":1,"request_id":"b".repeat(32),"started_at":1.0,"observed_at":2.0,"complete":true});
    assert!(validate("ReadBatch", valid.clone()).is_ok());
    for (key, value) in [
        ("request_id", json!("foreign")),
        ("complete", json!(false)),
        ("observed_at", json!(0)),
        ("generation", json!(-1)),
        ("unknown", json!(true)),
    ] {
        let mut invalid = valid.clone();
        invalid[key] = value;
        assert!(validate("ReadBatch", invalid).is_err());
    }
    for key in valid.as_object().unwrap().keys() {
        let mut invalid = valid.clone();
        invalid.as_object_mut().unwrap().remove(key);
        assert!(validate("ReadBatch", invalid).is_err());
    }
}
