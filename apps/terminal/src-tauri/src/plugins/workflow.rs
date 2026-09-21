use serde_json::{json, Value};
use crate::workspace::WindowProfile;

pub fn default_record() -> Value { json!({"version":4,"view":"总览","inspector":false,"tasks":false,"taskHeight":180,"marketRatio":60,"snapshot":"","contract":"","section":"工作概览","linkGroup":"A","locked":false,"viewport":null,"dock":"left","kind":"workspace","owner":"","ready":false,"open":true}) }
fn validate(patch: &Value) -> Result<(), String> {
        let values = patch.as_object().ok_or("Invalid workspace patch")?;
        let allowed = ["version","view","inspector","tasks","taskHeight","marketRatio","snapshot","contract","section","linkGroup","locked","viewport","dock"];
        if values.keys().any(|k| !allowed.contains(&k.as_str())) { return Err("Unknown workspace field".into()); }
        for (key, value) in values {
            let valid = match key.as_str() {
                "version" => value == 4,
                "dock" => value.as_str().is_some_and(|v| ["left","right","top","bottom"].contains(&v)),
                "viewport" => value.is_null() || (value["snapshot"].as_str().is_some_and(|v|v.len()<=512) && value["contract"].as_str().is_some_and(|v|v.len()<=512) && value["from"].as_f64().zip(value["to"].as_f64()).is_some_and(|(a,b)| a.is_finite() && b.is_finite() && a<b && a.abs()<1e9 && b.abs()<1e9)),
                "inspector" | "tasks" | "locked" => value.is_boolean(),
                "marketRatio" => value.as_f64().is_some_and(|n| (30.0..=75.0).contains(&n)),
                "taskHeight" => value.as_f64().is_some_and(|n| (120.0..=400.0).contains(&n)),
                "view" => value.as_str().is_some_and(|v| !v.is_empty() && v.len() <= 240),
                "linkGroup" => value.as_str().is_some_and(|v| ["","A","B","C"].contains(&v)),
                _ => value.as_str().is_some_and(|v| v.len() <= 512),
            };
            if !valid { return Err(format!("Invalid workspace field: {key}")); }
        }
    Ok(())
}
fn prepare_child(record: &mut Value) { record["view"]=json!("市场"); record["section"]=json!("历史图表"); }
fn title(record: &Value) -> &'static str { if record["kind"] == "chart" { "历史图表 · Asterion" } else { "工作台 · Asterion" } }
pub static PROFILE: WindowProfile = WindowProfile {
    default_record, validate, linked_fields: &["snapshot", "contract"],
    merge_fields: &["snapshot", "contract", "viewport"], detached_kind: "chart", prepare_child, title,
};
