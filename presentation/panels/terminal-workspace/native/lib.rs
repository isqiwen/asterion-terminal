use asterion_desktop_bridge::workspace::WindowProfile;
use serde_json::{Value, json};

pub fn default_record() -> Value {
    json!({"version":4,"view":"总览","inspector":false,"tasks":false,"taskHeight":180,"marketRatio":60,"snapshot":"","contract":"","section":"工作概览","linkGroup":"A","locked":false,"viewport":null,"dock":"left","kind":"workspace","owner":"","ready":false,"open":true})
}
fn validate(patch: &Value) -> Result<(), String> {
    let values = patch.as_object().ok_or("Invalid workspace patch")?;
    let allowed = [
        "version",
        "view",
        "inspector",
        "tasks",
        "taskHeight",
        "marketRatio",
        "snapshot",
        "contract",
        "section",
        "linkGroup",
        "locked",
        "viewport",
        "dock",
    ];
    if values.keys().any(|k| !allowed.contains(&k.as_str())) {
        return Err("Unknown workspace field".into());
    }
    for (key, value) in values {
        let valid = match key.as_str() {
            "version" => value == 4,
            "dock" => value
                .as_str()
                .is_some_and(|v| ["left", "right", "top", "bottom"].contains(&v)),
            "viewport" => {
                value.is_null()
                    || (value["snapshot"].as_str().is_some_and(|v| v.len() <= 512)
                        && value["contract"].as_str().is_some_and(|v| v.len() <= 512)
                        && value["from"]
                            .as_f64()
                            .zip(value["to"].as_f64())
                            .is_some_and(|(a, b)| {
                                a.is_finite()
                                    && b.is_finite()
                                    && a < b
                                    && a.abs() < 1e9
                                    && b.abs() < 1e9
                            }))
            }
            "inspector" | "tasks" | "locked" => value.is_boolean(),
            "marketRatio" => value.as_f64().is_some_and(|n| (30.0..=75.0).contains(&n)),
            "taskHeight" => value.as_f64().is_some_and(|n| (120.0..=400.0).contains(&n)),
            "view" => value
                .as_str()
                .is_some_and(|v| !v.is_empty() && v.len() <= 240),
            "linkGroup" => value
                .as_str()
                .is_some_and(|v| ["", "A", "B", "C"].contains(&v)),
            _ => value.as_str().is_some_and(|v| v.len() <= 512),
        };
        if !valid {
            return Err(format!("Invalid workspace field: {key}"));
        }
    }
    Ok(())
}
fn prepare_child(record: &mut Value) {
    record["view"] = json!("市场");
    record["section"] = json!("历史图表");
}
fn title(record: &Value) -> &'static str {
    if record["kind"] == "chart" {
        "历史图表 · Asterion"
    } else {
        "工作台 · Asterion"
    }
}
pub static PROFILE: WindowProfile = WindowProfile {
    default_record,
    validate,
    linked_fields: &["snapshot", "contract"],
    merge_fields: &["snapshot", "contract", "viewport"],
    detached_kind: "chart",
    duplicate_detached_error: "图表窗口已打开",
    missing_detached_error: "没有拆出的图表",
    owner_closed_error: "原窗口已关闭，请保留当前图表窗口",
    prepare_child,
    title,
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn current_workspace_profile_checks_fields_and_chart_viewport() {
        let mut record = default_record();
        for key in ["kind", "owner", "ready", "open"] {
            record.as_object_mut().unwrap().remove(key);
        }
        validate(&record).unwrap();
        for patch in [
            json!({"unknown":true}),
            json!({"version":null}),
            json!({"marketRatio":99}),
            json!({"viewport":{"snapshot":"s","contract":"c","from":10,"to":5}}),
        ] {
            assert!(validate(&patch).is_err());
        }
        validate(&json!({"marketRatio":45,"view":"数据","viewport":{"snapshot":"s","contract":"c","from":-5,"to":80}})).unwrap();
    }

    #[test]
    fn chart_profile_owns_child_presentation_and_linked_state() {
        let mut record = default_record();
        record["snapshot"] = json!("snapshot-1");
        record["contract"] = json!("SHFE.rb2610");
        record["kind"] = json!(PROFILE.detached_kind);
        (PROFILE.prepare_child)(&mut record);
        assert_eq!(record["view"], "市场");
        assert_eq!(record["section"], "历史图表");
        assert_eq!(record["snapshot"], "snapshot-1");
        assert_eq!(record["contract"], "SHFE.rb2610");
        assert_eq!(PROFILE.linked_fields, &["snapshot", "contract"]);
        assert_eq!(PROFILE.merge_fields, &["snapshot", "contract", "viewport"]);
        assert_eq!((PROFILE.title)(&record), "历史图表 · Asterion");
    }
}
