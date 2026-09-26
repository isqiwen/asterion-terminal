//! Stable identifiers of collections and versions. They are UUIDv5 names of
//! the identity's canonical text, which is Python's
//! `json.dumps(value, sort_keys=True)`; existing identifiers were derived that
//! way, so the text must match it exactly.
use asterion_data_store::pyfmt::float_repr;
use serde_json::Value;
use std::fmt::Write;
use uuid::Uuid;

/// Python `json.dumps` options of the two canonical forms stored data uses.
#[derive(Clone, Copy)]
struct Style {
    ascii: bool,
    item: &'static str,
    key: &'static str,
}

fn string(text: &str, style: Style, output: &mut String) {
    output.push('"');
    for c in text.chars() {
        match c {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            '\u{08}' => output.push_str("\\b"),
            '\u{0c}' => output.push_str("\\f"),
            c if (c as u32) < 0x20 || (style.ascii && (c as u32) > 0x7e) => {
                let mut units = [0u16; 2];
                for unit in c.encode_utf16(&mut units) {
                    let _ = write!(output, "\\u{unit:04x}");
                }
            }
            c => output.push(c),
        }
    }
    output.push('"');
}

fn dump(value: &Value, style: Style, output: &mut String) {
    match value {
        Value::Null => output.push_str("null"),
        Value::Bool(flag) => output.push_str(if *flag { "true" } else { "false" }),
        Value::Number(number) => {
            if let Some(integer) = number.as_i64() {
                let _ = write!(output, "{integer}");
            } else if let Some(integer) = number.as_u64() {
                let _ = write!(output, "{integer}");
            } else {
                output.push_str(&float_repr(number.as_f64().unwrap_or_default()));
            }
        }
        Value::String(text) => string(text, style, output),
        Value::Array(items) => {
            output.push('[');
            for (index, item) in items.iter().enumerate() {
                if index > 0 {
                    output.push_str(style.item);
                }
                dump(item, style, output);
            }
            output.push(']');
        }
        Value::Object(fields) => {
            output.push('{');
            let mut keys: Vec<&String> = fields.keys().collect();
            keys.sort();
            for (index, key) in keys.into_iter().enumerate() {
                if index > 0 {
                    output.push_str(style.item);
                }
                string(key, style, output);
                output.push_str(style.key);
                dump(&fields[key], style, output);
            }
            output.push('}');
        }
    }
}

/// `json.dumps(value, sort_keys=True)` with Python's default separators.
pub fn python_json(value: &Value) -> String {
    let mut output = String::new();
    let style = Style {
        ascii: true,
        item: ", ",
        key: ": ",
    };
    dump(value, style, &mut output);
    output
}

/// `json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))`:
/// the canonical bytes that evidence digests are taken over.
pub fn canonical(value: &Value) -> Vec<u8> {
    let mut output = String::new();
    let style = Style {
        ascii: false,
        item: ",",
        key: ":",
    };
    dump(value, style, &mut output);
    output.into_bytes()
}

/// The stable identifier of an identity value.
pub fn stable_id(value: &Value) -> String {
    let name = format!("asterion:data:v1:{}", python_json(value));
    Uuid::new_v5(&Uuid::NAMESPACE_URL, name.as_bytes()).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn canonical_text_matches_python_json_dumps() {
        let value =
            json!({"b": [1, 2.5, null, true], "a": {"z": "螺纹\u{1F600}\"\n", "y": -0.0001}});
        let expected = concat!(
            r#"{"a": {"y": -0.0001, "z": ""#,
            "\\u87ba\\u7eb9\\ud83d\\ude00",
            r#"\"\n"}, "b": [1, 2.5, null, true]}"#
        );
        assert_eq!(python_json(&value), expected);
    }
}
