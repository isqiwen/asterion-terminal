//! Fixed, deterministic primitives. No plugins, I/O, clocks, or business models.

pub mod communication;
pub mod decimal;
#[rustfmt::skip]
pub mod wire_generated;

use serde_json::Value;
use sha2::{Digest, Sha256};

/// UTF-8 JSON, recursively sorted keys, compact separators and exact integers.
/// Floating-point identities require a separately specified numeric contract.
pub fn canonical(value: &Value) -> Result<Vec<u8>, String> {
    fn ordered(value: &Value) -> Result<Value, String> {
        Ok(match value {
            Value::Number(n) if n.is_f64() => {
                return Err("canonical identities do not accept floating-point values".into());
            }
            Value::Array(items) => {
                Value::Array(items.iter().map(ordered).collect::<Result<_, _>>()?)
            }
            Value::Object(fields) => {
                let sorted = fields.iter().collect::<std::collections::BTreeMap<_, _>>();
                Value::Object(
                    sorted
                        .into_iter()
                        .map(|(k, v)| Ok((k.clone(), ordered(v)?)))
                        .collect::<Result<_, String>>()?,
                )
            }
            _ => value.clone(),
        })
    }
    serde_json::to_vec(&ordered(value)?).map_err(|e| e.to_string())
}

pub fn digest(value: &Value) -> Result<String, String> {
    Ok(hex::encode(Sha256::digest(canonical(value)?)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn canonical_is_recursive_and_preserves_unicode_and_integer_precision() {
        let value = json!({"z": [true, null, 9007199254740993_u64], "a": {"汉": "\n", "b": 1}});
        assert_eq!(
            String::from_utf8(canonical(&value).unwrap()).unwrap(),
            "{\"a\":{\"b\":1,\"汉\":\"\\n\"},\"z\":[true,null,9007199254740993]}"
        );
        assert_eq!(
            digest(&json!({"b": 2, "a": 1})).unwrap(),
            "43258cff783fe7036d8a43033f830adfc60ec037382473548ac742b888292777"
        );
    }

    #[test]
    fn floating_point_never_silently_changes_identity() {
        assert!(canonical(&json!({"nested": [1.0]})).is_err());
    }
}
