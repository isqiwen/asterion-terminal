//! Catalogue queries over published versions: the latest version of each
//! collection, a collection's version history and the business directory
//! derived from published identities (never guessed from market codes).
use asterion_store::{StoreError, Transaction};
use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::sync::LazyLock;

#[derive(Debug)]
pub enum CatalogError {
    /// A stored identity does not satisfy the current catalogue contract.
    Invalid(String),
    Store(StoreError),
}
impl From<StoreError> for CatalogError {
    fn from(error: StoreError) -> Self {
        Self::Store(error)
    }
}
impl std::fmt::Display for CatalogError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Invalid(message) => f.write_str(message),
            Self::Store(error) => write!(f, "{error}"),
        }
    }
}
type Result<T> = std::result::Result<T, CatalogError>;

fn invalid(message: &str) -> CatalogError {
    CatalogError::Invalid(message.into())
}

/// Filters of the latest-version listing. Empty strings do not filter.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct CatalogQuery {
    pub directory: String,
    pub include_archived: bool,
    pub domain: String,
    pub type_id: String,
    pub source: String,
    pub layer: String,
    pub search: String,
    pub offset: i64,
    pub limit: i64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct DirectoryNode {
    pub path: Vec<String>,
    pub label: String,
    pub count: usize,
}

const EXCHANGES: [&str; 6] = ["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"];
static CONTRACT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.([A-Z]+)\.([0-9]{6})\.([0-9]{8})$")
        .expect("pattern")
});

fn label(value: &str) -> String {
    if let Some(parts) = CONTRACT.captures(value) {
        let (product, month, listed) = (&parts[2], &parts[3], &parts[4]);
        return format!(
            "{product} {}-{}（{}-{}-{} 上市）",
            &month[..4],
            &month[4..],
            &listed[..4],
            &listed[4..6],
            &listed[6..]
        );
    }
    match value {
        "SHFE" => "SHFE · 上期所",
        "DCE" => "DCE · 大商所",
        "CZCE" => "CZCE · 郑商所",
        "CFFEX" => "CFFEX · 中金所",
        "INE" => "INE · 能源中心",
        "GFEX" => "GFEX · 广期所",
        "_shared" => "交易所公共资料",
        "reference" => "基础资料",
        "contracts" => "实际合约",
        "unclassified" => "未分类资料",
        "RAW" => "原始",
        "STANDARD" => "标准",
        "DERIVED" => "加工",
        "futures.contracts" => "合约资料",
        "futures.calendar" => "交易日历",
        "futures.daily" => "日线",
        "futures.minute" => "分钟",
        "futures.bars" => "日内行情",
        "futures.settlement" => "结算资料",
        "1d" => "日线",
        "1m" => "1分钟",
        "5m" => "5分钟",
        "15m" => "15分钟",
        "30m" => "30分钟",
        "1h" => "1小时",
        "60m" => "60分钟",
        other => other,
    }
    .to_string()
}

fn text<'a>(value: &'a Value, what: &str) -> Result<&'a str> {
    value
        .as_str()
        .ok_or_else(|| invalid(&format!("目录{what}不符合当前契约")))
}

/// Directory branches of a published version manifest.
pub fn paths(manifest: &Value) -> Result<Vec<Vec<String>>> {
    let scope = &manifest["scope"];
    let definition = &manifest["type"];
    let type_id = text(&definition["id"], "数据类型")?;
    let kind = if matches!(type_id, "futures.bars" | "futures.minute") {
        text(&definition["frequency"], "数据频率")?
    } else {
        type_id
    };
    let tail = [
        kind.to_string(),
        text(&manifest["source"], "来源")?.to_string(),
        text(&manifest["layer"], "层级")?.to_string(),
    ];
    let branch = |head: &[&str]| -> Vec<String> {
        head.iter()
            .map(|part| (*part).to_string())
            .chain(tail.iter().cloned())
            .collect()
    };
    let identifiers = scope["contract_ids"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    if !identifiers.is_empty() {
        let mut result = BTreeSet::new();
        for identifier in &identifiers {
            let identifier = identifier.as_str().unwrap_or_default();
            let parts = CONTRACT
                .captures(identifier)
                .ok_or_else(|| invalid("目录实际合约身份不符合当前契约"))?;
            result.insert(branch(&[&parts[1], &parts[2], "contracts", identifier]));
        }
        return Ok(result.into_iter().collect());
    }
    let exchange = scope["exchange"].as_str().unwrap_or_default();
    if matches!(type_id, "futures.contracts" | "futures.calendar") && EXCHANGES.contains(&exchange)
    {
        // Exchange-wide evidence must not pretend to belong to a single product.
        return Ok(vec![branch(&[exchange, "_shared", "reference"])]);
    }
    Ok(vec![branch(&["unclassified"])])
}

fn belongs(manifest: &Value, directory: &[String]) -> Result<bool> {
    Ok(paths(manifest)?
        .iter()
        .any(|branch| branch.len() >= directory.len() && branch[..directory.len()] == *directory))
}

fn nodes(records: &[(String, Vec<Vec<String>>)]) -> Vec<DirectoryNode> {
    let mut members: BTreeMap<Vec<String>, BTreeSet<&str>> = BTreeMap::new();
    for (identifier, branches) in records {
        for branch in branches {
            for length in 1..=branch.len() {
                members
                    .entry(branch[..length].to_vec())
                    .or_default()
                    .insert(identifier);
            }
        }
    }
    members
        .into_iter()
        .map(|(path, ids)| DirectoryNode {
            label: label(path.last().expect("non-empty path")),
            path,
            count: ids.len(),
        })
        .collect()
}

/// A catalogue row with its manifest decoded and without internal storage paths.
pub(crate) fn public_version(names: &[&str], row: Vec<Value>) -> Result<Value> {
    let mut record = Map::new();
    for (name, value) in names.iter().zip(row) {
        let value = match *name {
            "manifest" => {
                let mut manifest: Value = match value {
                    Value::String(encoded) => serde_json::from_str(&encoded)
                        .map_err(|_| invalid("数据版本清单不符合当前契约"))?,
                    other => other,
                };
                if let Some(fields) = manifest.as_object_mut() {
                    fields.remove("path");
                }
                manifest
            }
            "archived" => json!(value.as_bool().unwrap_or(value.as_i64() == Some(1))),
            _ => value,
        };
        record.insert((*name).to_string(), value);
    }
    Ok(Value::Object(record))
}

const LATEST: [&str; 9] = [
    "id",
    "dataset_id",
    "job_id",
    "created_at",
    "rows",
    "manifest",
    "archived",
    "rank",
    "version_count",
];

/// The latest version of each collection passing the filters, newest first.
fn latest(tx: &Transaction, query: &CatalogQuery) -> Result<(String, Vec<Value>)> {
    let mut conditions = vec!["r.rank = 1".to_string()];
    let mut params = Vec::new();
    for (column, value) in [
        ("c.domain", &query.domain),
        ("c.type_id", &query.type_id),
        ("c.layer", &query.layer),
    ] {
        if !value.is_empty() {
            conditions.push(format!("{column} = ?"));
            params.push(json!(value));
        }
    }
    if !query.source.is_empty() {
        if query.source.starts_with("c_") {
            // Connection-scoped collections are selected by their connection.
            conditions.push(if tx.is_postgres() {
                "c.identity #>> '{scope,connection_id}' = ?".into()
            } else {
                "JSON_EXTRACT(c.identity, '$.scope.connection_id') = ?".into()
            });
        } else {
            conditions.push("c.source = ?".into());
        }
        params.push(json!(query.source));
    }
    if !query.search.is_empty() {
        // Search only declared catalogue identities, never secrets or raw payloads.
        let escaped = query
            .search
            .replace('/', "//")
            .replace('%', "/%")
            .replace('_', "/_");
        conditions.push("CAST(c.identity AS VARCHAR) LIKE ? ESCAPE '/'".into());
        params.push(json!(format!("%{escaped}%")));
    }
    if !query.include_archived {
        conditions.push("r.archived = FALSE".into());
    }
    let sql = format!(
        "SELECT r.id, r.dataset_id, r.job_id, r.created_at, r.rows, r.manifest, r.archived, \
         r.rank, r.version_count FROM (SELECT v.id, v.dataset_id, v.job_id, v.created_at, \
         v.rows, v.manifest, COALESCE(s.archived, FALSE) AS archived, ROW_NUMBER() OVER \
         (PARTITION BY v.dataset_id ORDER BY v.created_at DESC, v.id DESC) AS rank, \
         COUNT(*) OVER (PARTITION BY v.dataset_id) AS version_count FROM data_versions v \
         LEFT OUTER JOIN data_version_states s ON s.version_id = v.id) r \
         JOIN data_collections c ON c.id = r.dataset_id WHERE {}",
        conditions.join(" AND ")
    );
    Ok((sql, params))
}

/// One page of latest versions, optionally within a directory branch.
pub fn list(tx: &Transaction, query: &CatalogQuery) -> Result<Value> {
    let (sql, params) = latest(tx, query)?;
    let ordered = format!("{sql} ORDER BY r.created_at DESC, r.id");
    if !query.directory.is_empty() {
        let branch: Vec<String> = query.directory.split('/').map(String::from).collect();
        let (mut selected, mut total) = (Vec::new(), 0i64);
        for row in tx.rows(&ordered, params)? {
            let record = public_version(&LATEST, row)?;
            if belongs(&record["manifest"], &branch)? {
                if query.offset <= total && total < query.offset + query.limit {
                    selected.push(record);
                }
                total += 1;
            }
        }
        return Ok(json!({"items": selected, "total": total, "offset": query.offset}));
    }
    let total = tx.rows(&format!("SELECT COUNT(*) FROM ({sql}) q"), params.clone())?[0][0].clone();
    let mut page = params;
    page.extend([json!(query.limit), json!(query.offset)]);
    let items = tx
        .rows(&format!("{ordered} LIMIT ? OFFSET ?"), page)?
        .into_iter()
        .map(|row| public_version(&LATEST, row))
        .collect::<Result<Vec<_>>>()?;
    Ok(json!({"items": items, "total": total, "offset": query.offset}))
}

/// Directory tree of the latest versions with the number of collections below each node.
pub fn hierarchy(tx: &Transaction, include_archived: bool) -> Result<Vec<DirectoryNode>> {
    let query = CatalogQuery {
        include_archived,
        ..CatalogQuery::default()
    };
    let (sql, params) = latest(tx, &query)?;
    let mut records = Vec::new();
    for row in tx.rows(&format!("{sql} ORDER BY r.created_at DESC, r.id"), params)? {
        let record = public_version(&LATEST, row)?;
        let identifier = record["dataset_id"]
            .as_str()
            .unwrap_or_default()
            .to_string();
        records.push((identifier, paths(&record["manifest"])?));
    }
    Ok(nodes(&records))
}

const HISTORY: [&str; 7] = [
    "id",
    "dataset_id",
    "job_id",
    "created_at",
    "rows",
    "manifest",
    "archived",
];

/// All versions of one collection, newest first.
pub fn history(tx: &Transaction, dataset_id: &str, offset: i64, limit: i64) -> Result<Value> {
    let base = "FROM data_versions v LEFT OUTER JOIN data_version_states s \
                ON s.version_id = v.id WHERE v.dataset_id = ?";
    let total = tx.rows(&format!("SELECT COUNT(*) {base}"), vec![json!(dataset_id)])?[0][0].clone();
    let items = tx
        .rows(
            &format!(
                "SELECT v.id, v.dataset_id, v.job_id, v.created_at, v.rows, v.manifest, \
                 COALESCE(s.archived, FALSE) {base} ORDER BY v.created_at DESC, v.id \
                 LIMIT ? OFFSET ?"
            ),
            vec![json!(dataset_id), json!(limit), json!(offset)],
        )?
        .into_iter()
        .map(|row| public_version(&HISTORY, row))
        .collect::<Result<Vec<_>>>()?;
    Ok(json!({"items": items, "total": total, "offset": offset}))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn directory_paths_follow_published_identities_only() {
        let manifest = |type_id: &str, scope: Value| {
            json!({"type": {"id": type_id, "frequency": "1m"}, "source": "tushare",
                   "layer": "STANDARD", "scope": scope})
        };
        let contracts = manifest(
            "futures.minute",
            json!({"contract_ids": ["SHFE.RB.202610.20251001", "SHFE.RB.202610.20251001"]}),
        );
        assert_eq!(
            paths(&contracts).unwrap(),
            vec![vec![
                "SHFE",
                "RB",
                "contracts",
                "SHFE.RB.202610.20251001",
                "1m",
                "tushare",
                "STANDARD"
            ]]
        );
        let shared = manifest("futures.calendar", json!({"exchange": "DCE"}));
        assert_eq!(
            paths(&shared).unwrap()[0][..3],
            ["DCE", "_shared", "reference"]
        );
        let other = manifest("futures.daily", json!({"exchange": "XX"}));
        assert_eq!(paths(&other).unwrap()[0][0], "unclassified");
        let bad = manifest("futures.daily", json!({"contract_ids": ["rb2610"]}));
        assert!(matches!(paths(&bad), Err(CatalogError::Invalid(_))));
        assert_eq!(
            label("SHFE.RB.202610.20251001"),
            "RB 2026-10（2025-10-01 上市）"
        );
        let tree = nodes(&[
            ("a".into(), paths(&contracts).unwrap()),
            ("b".into(), paths(&shared).unwrap()),
        ]);
        assert_eq!(
            tree[0],
            DirectoryNode {
                path: vec!["DCE".into()],
                label: "DCE · 大商所".into(),
                count: 1
            }
        );
        assert!(belongs(&contracts, &["SHFE".into(), "RB".into()]).unwrap());
        assert!(!belongs(&contracts, &["SHFE".into(), "AU".into()]).unwrap());
    }
}
