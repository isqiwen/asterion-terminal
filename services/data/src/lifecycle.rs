//! Reversible archiving of published versions and the references that keep
//! them. Nothing is deleted: every version keeps its files and fixed references.
//!
//! References are counted from the documents that pin a version: data lineage,
//! import and sync identities, coverage reports and history plans (data), and
//! reference releases, contract rules, contract roles and research inputs.
//! The latter owners are not yet Rust services; their counters live here with
//! the fixed catalogue until those owners move and take them over.
use asterion_store::{StoreError, Transaction};
use serde::Deserialize;
use serde_json::{Map, Value, json};
use std::collections::BTreeMap;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Debug)]
pub enum LifecycleError {
    NotFound,
    Conflict(String),
    /// A stored document does not satisfy the current contract.
    Refused(String),
    Store(StoreError),
}
impl From<StoreError> for LifecycleError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
type Result<T> = std::result::Result<T, LifecycleError>;

fn refused() -> LifecycleError {
    LifecycleError::Refused("版本引用记录不符合当前契约".into())
}

fn document(value: &Value) -> Result<Value> {
    match value {
        Value::String(text) => serde_json::from_str(text).map_err(|_| refused()),
        other => Ok(other.clone()),
    }
}

fn documents(tx: &Transaction, sql: &str, params: Vec<Value>) -> Result<Vec<Value>> {
    tx.rows(sql, params)?
        .iter()
        .map(|row| document(&row[0]))
        .collect()
}

fn is(value: &Value, version_id: &str) -> bool {
    value.as_str() == Some(version_id)
}

/// Version identifiers of a catalogue's `inputs`, which must be present.
fn catalog_inputs(catalog: &Value) -> Result<Vec<&Value>> {
    catalog["inputs"]
        .as_array()
        .map(|inputs| inputs.iter().map(|input| &input["version_id"]).collect())
        .ok_or_else(refused)
}

fn pins_catalog(catalog: &Value, version_id: &str) -> Result<bool> {
    Ok(catalog_inputs(catalog)?
        .into_iter()
        .any(|input| is(input, version_id)))
}

/// Versions a contract-rule specification is derived from.
fn rule_versions(spec: &Value) -> Vec<&Value> {
    let mut found = vec![
        &spec["basis"]["version_id"],
        &spec["contract"]["provenance"]["source_version"],
    ];
    for period in spec["periods"].as_array().into_iter().flatten() {
        found.push(&period["settlement_basis"]["evidence"]["version_id"]);
    }
    found
}

/// Versions a research run or package is fixed to.
fn research_inputs(payload: &Value) -> Vec<&Value> {
    let mut found = vec![
        &payload["request"]["version_id"],
        &payload["version"]["id"],
        &payload["coverage"]["daily_version_id"],
        &payload["coverage"]["calendar_version_id"],
        &payload["coverage"]["contracts_version_id"],
    ];
    found.extend(rule_versions(&payload["request"]["rules"]["spec"]));
    found.extend(
        payload["version"]["manifest"]["inputs"]
            .as_array()
            .into_iter()
            .flatten(),
    );
    found
}

fn count(values: Vec<&Value>, version_id: &str) -> i64 {
    i64::from(values.into_iter().any(|value| is(value, version_id)))
}

/// Reference counts by category. Data's own categories appear only when
/// non-zero; each other owner always reports its categories.
pub fn references(tx: &Transaction, version_id: &str) -> Result<BTreeMap<&'static str, i64>> {
    let mut counts: BTreeMap<&'static str, i64> = BTreeMap::new();
    let mut add = |key: &'static str, value: i64| {
        if value > 0 {
            *counts.entry(key).or_default() += value;
        }
    };
    for manifest in documents(tx, "SELECT manifest FROM data_versions", vec![])? {
        let inputs = manifest["inputs"].as_array().cloned().unwrap_or_default();
        if inputs.iter().any(|input| is(input, version_id))
            || is(&manifest["parent_version_id"], version_id)
        {
            add("data_lineage", 1);
        }
        if manifest["source"] == "local_file" {
            let catalog = &manifest["import_options"]["identity"]["catalog"];
            add(
                "import_identity",
                i64::from(pins_catalog(catalog, version_id)?),
            );
        }
        if !manifest["contract_identity"].is_null() {
            let catalog = &manifest["contract_identity"]["catalog"];
            add(
                "sync_identity",
                i64::from(pins_catalog(catalog, version_id)?),
            );
        }
    }
    for report in documents(tx, "SELECT report FROM data_coverage_reports", vec![])? {
        add(
            "coverage_reports",
            count(
                vec![
                    &report["daily_version_id"],
                    &report["calendar_version_id"],
                    &report["contracts_version_id"],
                ],
                version_id,
            ),
        );
    }
    for payload in documents(tx, "SELECT payload FROM jobs", vec![])? {
        if let Some(plan) = payload.get("history_plan") {
            let request = &plan["request"];
            if !request["contracts_version_id"].is_string() {
                return Err(refused());
            }
            add(
                "history_plans",
                count(
                    vec![
                        &request["contracts_version_id"],
                        &request["calendar_version_id"],
                    ],
                    version_id,
                ),
            );
        }
    }
    let mut owned = |key: &'static str, value: i64| {
        *counts.entry(key).or_default() += value;
    };
    let mut releases = 0;
    for catalog in documents(tx, "SELECT catalog FROM reference_releases", vec![])? {
        releases += i64::from(pins_catalog(&catalog, version_id)?);
    }
    owned("reference_catalogs", releases);
    let rules = documents(tx, "SELECT spec FROM contract_rule_versions", vec![])?
        .iter()
        .map(|spec| count(rule_versions(spec), version_id))
        .sum();
    owned("contract_rules", rules);
    owned("contract_roles", role_references(tx, version_id)?);
    let runs = documents(
        tx,
        "SELECT payload FROM jobs WHERE kind = ?",
        vec![json!("research.backtest")],
    )?
    .iter()
    .map(|payload| count(research_inputs(payload), version_id))
    .sum();
    owned("research_runs", runs);
    let mut drafts = 0;
    for content in documents(
        tx,
        "SELECT content FROM research_documents WHERE deleted = FALSE",
        vec![],
    )? {
        let config = &content["config"];
        let mut pinned = vec![&config["version_id"]];
        pinned.extend(rule_versions(&config["rules"]["spec"]));
        drafts += count(pinned, version_id);
    }
    owned("research_documents", drafts);
    let packages = documents(tx, "SELECT package FROM research_packages", vec![])?
        .iter()
        .map(|package| count(research_inputs(&package["content"]), version_id))
        .sum();
    owned("research_packages", packages);
    Ok(counts)
}

fn role_references(tx: &Transaction, version_id: &str) -> Result<i64> {
    let mut total = 0;
    for spec in documents(tx, "SELECT spec FROM contract_role_versions", vec![])? {
        total += i64::from(
            is(&spec["source_version"], version_id) || pins_catalog(&spec["catalog"], version_id)?,
        );
    }
    let computed = |request: &Value| -> Result<i64> {
        let daily = request["daily_inputs"].as_array().ok_or_else(refused)?;
        Ok(i64::from(
            is(&request["contracts_version_id"], version_id)
                || daily.iter().any(|item| is(&item["version_id"], version_id)),
        ))
    };
    for spec in documents(tx, "SELECT spec FROM computed_role_versions", vec![])? {
        total += computed(&spec["request"])?;
    }
    for payload in documents(
        tx,
        "SELECT payload FROM jobs WHERE kind = ?",
        vec![json!("contract_roles.continue")],
    )? {
        total += computed(&payload["spec"]["request"])?;
    }
    for request in documents(tx, "SELECT request FROM role_sync_workflows", vec![])? {
        let identifiers = request["sync_job_ids"].as_array().ok_or_else(refused)?;
        for identifier in identifiers {
            let rows = tx.rows(
                "SELECT kind, payload, result FROM jobs WHERE id = ?",
                vec![identifier.clone()],
            )?;
            let row = rows
                .first()
                .ok_or_else(|| LifecycleError::Refused("同步依赖任务不存在".into()))?;
            let payload = document(&row[1])?;
            if row[0] != "data.sync" || payload["type_id"] != "futures.daily" {
                return Err(LifecycleError::Refused("续算依赖必须是日线同步任务".into()));
            }
            if payload["contract_identity"].is_null() {
                return Err(LifecycleError::Refused("同步依赖缺少固定合约身份".into()));
            }
            let result = if row[2].is_null() {
                Value::Null
            } else {
                document(&row[2])?
            };
            total += i64::from(is(&result["version_id"], version_id));
        }
    }
    Ok(total)
}

fn boolean(value: &Value) -> bool {
    value.as_bool().unwrap_or(value.as_i64() == Some(1))
}

/// The archive state, latest status and references of one version.
pub fn inspect(tx: &Transaction, version_id: &str) -> Result<Value> {
    let rows = tx.rows(
        "SELECT dataset_id FROM data_versions WHERE id = ?",
        vec![json!(version_id)],
    )?;
    let dataset_id = rows.first().ok_or(LifecycleError::NotFound)?[0].clone();
    let state = tx
        .rows(
            "SELECT archived, revision, updated_at FROM data_version_states WHERE version_id = ?",
            vec![json!(version_id)],
        )?
        .into_iter()
        .next();
    let counts = references(tx, version_id)?;
    let latest = tx.rows(
        "SELECT id FROM data_versions WHERE dataset_id = ? \
         ORDER BY created_at DESC, id DESC LIMIT 1",
        vec![dataset_id],
    )?;
    let references: Map<String, Value> = counts
        .iter()
        .map(|(key, value)| ((*key).to_string(), json!(value)))
        .collect();
    Ok(json!({
        "version_id": version_id,
        "archived": state.as_ref().is_some_and(|row| boolean(&row[0])),
        "revision": state.as_ref().map_or(json!(0), |row| row[1].clone()),
        "updated_at": state.as_ref().map_or(Value::Null, |row| row[2].clone()),
        "is_latest": latest.first().is_some_and(|row| is(&row[0], version_id)),
        "references": references,
        "reference_count": counts.values().sum::<i64>(),
        "protected": true,
        "can_delete": false,
        "protection_reason": "所有版本保留文件与固定引用；暂不提供永久删除或分区回收。",
    }))
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ArchiveRequest {
    pub archived: bool,
    pub expected_revision: i64,
}

fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_secs_f64())
        .unwrap_or_default()
}

/// Archive or restore a version under compare-and-set on its revision. A
/// retried request that already took effect returns the current state.
pub fn archive(tx: &Transaction, version_id: &str, request: &ArchiveRequest) -> Result<Value> {
    let found = tx.rows(
        &format!("SELECT id FROM data_versions WHERE id = ?{}", tx.locked()),
        vec![json!(version_id)],
    )?;
    if found.is_empty() {
        return Err(LifecycleError::NotFound);
    }
    tx.execute(
        "INSERT INTO data_version_states (version_id, archived, revision, updated_at) \
         VALUES (?, FALSE, 0, ?) ON CONFLICT (version_id) DO NOTHING",
        vec![json!(version_id), json!(now())],
    )?;
    let state = tx.rows(
        &format!(
            "SELECT archived, revision FROM data_version_states WHERE version_id = ?{}",
            tx.locked()
        ),
        vec![json!(version_id)],
    )?;
    let (archived, revision) = (boolean(&state[0][0]), state[0][1].as_i64().unwrap_or(-1));
    if revision != request.expected_revision {
        if revision == request.expected_revision + 1 && archived == request.archived {
            return inspect(tx, version_id);
        }
        return Err(LifecycleError::Conflict(
            "版本归档状态已被另一窗口修改，请刷新后重试".into(),
        ));
    }
    let changed = tx.execute(
        "UPDATE data_version_states SET archived = ?, revision = ?, updated_at = ? \
         WHERE version_id = ? AND revision = ?",
        vec![
            json!(request.archived),
            json!(request.expected_revision + 1),
            json!(now()),
            json!(version_id),
            json!(request.expected_revision),
        ],
    )?;
    if changed != 1 {
        return Err(LifecycleError::Conflict(
            "版本归档状态已改变，请刷新后重试".into(),
        ));
    }
    inspect(tx, version_id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rules_and_research_inputs_pin_every_fixed_basis() {
        let rule = json!({"basis": {"version_id": "basis"},
                          "contract": {"provenance": {"source_version": "identity"}},
                          "periods": [{"settlement_basis": null},
                                      {"settlement_basis": {"evidence": {"version_id": "settlement"}}}]});
        for pinned in ["basis", "identity", "settlement"] {
            assert_eq!(count(rule_versions(&rule), pinned), 1, "{pinned}");
        }
        assert_eq!(count(rule_versions(&rule), "other"), 0);
        let run = json!({"request": {"version_id": "daily", "rules": {"spec": rule}},
                         "coverage": {"calendar_version_id": "calendar"},
                         "version": {"id": "fixed", "manifest": {"inputs": ["raw"]}}});
        for pinned in [
            "daily",
            "identity",
            "calendar",
            "fixed",
            "raw",
            "settlement",
        ] {
            assert_eq!(count(research_inputs(&run), pinned), 1, "{pinned}");
        }
        assert!(pins_catalog(&json!({"inputs": [{"version_id": "v"}]}), "v").unwrap());
        assert!(matches!(
            pins_catalog(&json!({}), "v"),
            Err(LifecycleError::Refused(_))
        ));
    }
}
