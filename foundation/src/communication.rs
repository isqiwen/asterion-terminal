//! Deterministic communication value validation. No clocks, identities, I/O or
//! process context live here; callers own those runtime concerns.

use crate::wire_generated::SCHEMA;
use serde::{
    Deserialize, Deserializer,
    de::{self, MapAccess, Visitor},
};
use serde_json::{Map, Value};
use std::{fmt, sync::OnceLock};

pub const MAX_SAFE_INTEGER: u64 = 9_007_199_254_740_991;
static SCHEMA_VALUE: OnceLock<Value> = OnceLock::new();

fn invalid() -> String {
    "Communication value does not match the current contract".into()
}

pub fn validate(name: &str, value: &Value) -> Result<(), String> {
    let schema = SCHEMA_VALUE
        .get_or_init(|| serde_json::from_str(SCHEMA).expect("generated communication schema"));
    let spec = schema["$defs"]
        .get(name)
        .ok_or("Unknown communication contract")?;
    visit(schema, spec, value)
}

fn visit(schema: &Value, spec: &Value, value: &Value) -> Result<(), String> {
    if let Some(reference) = spec.get("$ref").and_then(Value::as_str) {
        let spec = schema
            .pointer(reference.strip_prefix('#').ok_or_else(invalid)?)
            .ok_or_else(invalid)?;
        return visit(schema, spec, value);
    }
    if let Some(branches) = spec.get("anyOf").and_then(Value::as_array) {
        return if branches
            .iter()
            .any(|branch| visit(schema, branch, value).is_ok())
        {
            Ok(())
        } else {
            Err(invalid())
        };
    }
    let actual = match value {
        Value::Null => "null",
        Value::Bool(_) => "boolean",
        Value::String(_) => "string",
        Value::Array(_) => "array",
        Value::Object(_) => "object",
        Value::Number(number) => {
            if number.is_i64()
                || number.is_u64()
                || number.as_f64().is_some_and(|v| v.fract() == 0.0)
            {
                "integer"
            } else {
                "number"
            }
        }
    };
    if let Some(types) = spec.get("type") {
        let valid = types
            .as_str()
            .map(|kind| kind == actual)
            .unwrap_or_else(|| {
                types
                    .as_array()
                    .is_some_and(|kinds| kinds.iter().any(|kind| kind.as_str() == Some(actual)))
            });
        if !valid {
            return Err(invalid());
        }
    }
    if let Some(number) = value.as_number() {
        let number = number.as_f64().ok_or_else(invalid)?;
        if !number.is_finite()
            || (actual == "integer" && number.abs() > MAX_SAFE_INTEGER as f64)
            || number < spec["minimum"].as_f64().unwrap_or(f64::NEG_INFINITY)
            || number > spec["maximum"].as_f64().unwrap_or(f64::INFINITY)
        {
            return Err(invalid());
        }
    }
    if spec
        .get("enum")
        .and_then(Value::as_array)
        .is_some_and(|allowed| {
            !allowed.iter().any(|item| {
                item == value
                    || (item.is_number() && value.is_number() && item.as_f64() == value.as_f64())
            })
        })
    {
        return Err(invalid());
    }
    if let Some(text) = value.as_str() {
        let size = text.chars().count() as u64;
        if size < spec["minLength"].as_u64().unwrap_or(0)
            || size > spec["maxLength"].as_u64().unwrap_or(u64::MAX)
        {
            return Err(invalid());
        }
    }
    if let Some(map) = value.as_object() {
        if spec
            .get("required")
            .and_then(Value::as_array)
            .is_some_and(|keys| {
                keys.iter()
                    .any(|key| !map.contains_key(key.as_str().unwrap_or("")))
            })
        {
            return Err(invalid());
        }
        let props = spec.get("properties");
        for (key, child) in map {
            let child_spec = props.and_then(|properties| properties.get(key));
            if child_spec.is_none() && spec["additionalProperties"] == false {
                return Err(invalid());
            }
            visit(schema, child_spec.unwrap_or(&Value::Null), child)?;
        }
    }
    if let Some(items) = value.as_array() {
        for item in items {
            visit(schema, spec.get("items").unwrap_or(&Value::Null), item)?;
        }
    }
    Ok(())
}

/// JSON Schema integers include integral numeric representations such as 1.0.
/// Generated unsigned fields use the same numeric admission as validate().
pub fn deserialize_unsigned<'de, D: Deserializer<'de>>(deserializer: D) -> Result<u64, D::Error> {
    let value = Value::deserialize(deserializer)?;
    let number = value
        .as_f64()
        .ok_or_else(|| de::Error::custom("Invalid communication integer"))?;
    if !number.is_finite()
        || number.fract() != 0.0
        || !(0.0..=MAX_SAFE_INTEGER as f64).contains(&number)
    {
        return Err(de::Error::custom("Unsafe communication integer"));
    }
    Ok(number as u64)
}

/// Strict JSON parsing rejects duplicate keys at every nesting level. RawValue
/// preserves token kind even when another workspace crate enables serde_json's
/// arbitrary_precision feature; its private Number map must never become data.
pub fn parse_json(raw: &[u8]) -> Result<Value, String> {
    let value: &serde_json::value::RawValue =
        serde_json::from_slice(raw).map_err(|error| error.to_string())?;
    parse_raw(value)
}

fn parse_raw(raw: &serde_json::value::RawValue) -> Result<Value, String> {
    let text = raw.get();
    match text.as_bytes().first() {
        Some(b'{') => {
            struct Object(Value);
            impl<'de> Deserialize<'de> for Object {
                fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
                    struct ObjectVisitor;
                    impl<'de> Visitor<'de> for ObjectVisitor {
                        type Value = Object;
                        fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                            formatter.write_str("a JSON object without duplicate keys")
                        }
                        fn visit_map<A: MapAccess<'de>>(
                            self,
                            mut map: A,
                        ) -> Result<Self::Value, A::Error> {
                            let mut values = Map::new();
                            while let Some(key) = map.next_key::<String>()? {
                                if values.contains_key(&key) {
                                    return Err(de::Error::custom("Duplicate communication key"));
                                }
                                let child: &serde_json::value::RawValue = map.next_value()?;
                                values.insert(key, parse_raw(child).map_err(de::Error::custom)?);
                            }
                            Ok(Object(Value::Object(values)))
                        }
                    }
                    deserializer.deserialize_map(ObjectVisitor)
                }
            }
            serde_json::from_str::<Object>(text)
                .map(|value| value.0)
                .map_err(|error| error.to_string())
        }
        Some(b'[') => {
            let values: Vec<&serde_json::value::RawValue> =
                serde_json::from_str(text).map_err(|error| error.to_string())?;
            values
                .into_iter()
                .map(parse_raw)
                .collect::<Result<Vec<_>, _>>()
                .map(Value::Array)
        }
        _ => {
            let value: Value = serde_json::from_str(text).map_err(|error| error.to_string())?;
            if let Value::Number(number) = &value
                && number.as_f64().is_none_or(|value| !value.is_finite())
            {
                return Err("Non-finite communication number".into());
            }
            Ok(value)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire_generated::Context;
    use serde_json::json;

    #[test]
    fn all_shared_cases_use_the_schema_owned_by_contracts() {
        let cases: Value =
            serde_json::from_str(include_str!("../../contracts/communication-cases.json")).unwrap();
        for case in cases.as_array().unwrap() {
            assert_eq!(
                validate(case["type"].as_str().unwrap(), &case["value"]).is_ok(),
                case["valid"].as_bool().unwrap(),
                "{}",
                case["name"]
            );
        }
    }

    #[test]
    fn numbers_and_unicode_match_json_schema_semantics() {
        let context = json!({"version":1.0,"request_id":"😀".repeat(32),"correlation_id":"b".repeat(32),"causation_id":null,"deadline_ms":2000000000000.0});
        validate("Context", &context).unwrap();
        let typed: Context = serde_json::from_value(context).unwrap();
        assert_eq!(typed.version, 1);
        assert_eq!(typed.deadline_ms, 2000000000000);
        let mut bad = json!({"context":typed,"kind":"query","contract":"x","payload":{"nested":[9007199254740992_u64]}});
        assert!(validate("Call", &bad).is_err());
        bad["payload"]["nested"] = json!([1.25, -9007199254740991_i64, null]);
        validate("Call", &bad).unwrap();
        assert!(validate("Unknown", &bad).is_err());
    }

    #[test]
    fn strict_parser_does_not_erase_duplicate_keys_or_accept_non_finite_json() {
        for raw in [
            r#"{"a":1,"a":2}"#,
            r#"{"x":[{"key":1,"key":2}]}"#,
            "NaN",
            "Infinity",
            "1e999",
            "{}{}",
        ] {
            assert!(parse_json(raw.as_bytes()).is_err(), "{raw}");
        }
        assert_eq!(
            parse_json(br#"{"a":[true,null,-2,1.5]}"#).unwrap(),
            json!({"a":[true,null,-2,1.5]})
        );
        // This literal user key must remain an object, not the internal
        // arbitrary_precision Number transport used by serde_json.
        let object = parse_json(br#"{"$serde_json::private::Number":"1e999"}"#).unwrap();
        assert!(object.is_object());
        assert_eq!(object["$serde_json::private::Number"], "1e999");
    }
}
