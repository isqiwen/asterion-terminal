//! Identity catalogs built from one fixed, complete standard contracts version.
//! Selection never chooses a newer version or infers identity from a market
//! code; availability is never backdated before the version was recorded.
use crate::types;
use asterion_instrument_catalog::{
    CatalogInput, Contract, Exchange, Product, Provenance, ReferenceCatalog, SourceSymbol, Validate,
};
use chrono::{DateTime, FixedOffset, NaiveDate, TimeZone, Utc};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};

pub type Result<T> = std::result::Result<T, String>;

const TYPE: &str = "futures.contracts";
const LIMIT: u64 = 10_000;

/// A catalog input must be the current published standard contracts version it names.
pub fn validate_catalog_input(item: &CatalogInput, manifest: &Value) -> Result<()> {
    let schema = types::manifest_of(TYPE)?.schema_version;
    let text = |name: &str| manifest.get(name).and_then(Value::as_str);
    let demo = !matches!(
        manifest.get("demo"),
        None | Some(Value::Null) | Some(Value::Bool(false))
    );
    if text("source") != Some(item.source.as_str())
        || text("checksum") != Some(item.checksum.as_str())
        || text("layer") != Some("STANDARD")
        || text("state") != Some("PUBLISHED")
        || demo
        || manifest["type"]["id"] != TYPE
        || manifest["type"]["schema_version"] != json!(schema)
    {
        return Err("目录输入不是匹配的当前标准合约资料版本".into());
    }
    Ok(())
}

/// Python `datetime.fromtimestamp(t, UTC)`, moved up to the next microsecond
/// when rounding placed it before `t`.
fn recorded_at(created_at: f64) -> Result<DateTime<FixedOffset>> {
    let mut whole = created_at.trunc();
    let mut micros = (created_at.fract() * 1e6).round_ties_even();
    if micros >= 1e6 {
        whole += 1.0;
        micros -= 1e6;
    } else if micros < 0.0 {
        whole -= 1.0;
        micros += 1e6;
    }
    let mut total = whole as i64 * 1_000_000 + micros as i64;
    if (total as f64) / 1e6 < created_at {
        total += 1;
    }
    Utc.timestamp_micros(total)
        .single()
        .map(|at| at.fixed_offset())
        .ok_or_else(|| "合约资料版本登记时间无效".into())
}

/// An actual contract is its product, full delivery month and listing date.
fn contract_id(product_id: &str, month: &str, listed: NaiveDate) -> String {
    format!(
        "{product_id}.{}.{}",
        month.replace('-', ""),
        listed.format("%Y%m%d")
    )
}

fn text<'a>(row: &'a Value, name: &str) -> Option<&'a str> {
    row.get(name).and_then(Value::as_str)
}

fn day(row: &Value, name: &str) -> Result<Option<NaiveDate>> {
    match row.get(name) {
        None | Some(Value::Null) => Ok(None),
        Some(value) => value
            .as_str()
            .and_then(|text| NaiveDate::parse_from_str(text, "%Y-%m-%d").ok())
            .map(Some)
            .ok_or_else(|| "合约资料日期格式不正确".into()),
    }
}

fn complete(preview: &Value) -> Result<(&Value, &Vec<Value>)> {
    let rows = preview["rows"]
        .as_array()
        .ok_or("合约资料未完整读取，不能发布身份目录")?;
    let total = preview["total"].as_u64().unwrap_or(0);
    if !(1..=LIMIT).contains(&total) || total != rows.len() as u64 {
        return Err("合约资料未完整读取，不能发布身份目录".into());
    }
    Ok((&preview["version"], rows))
}

/// The identity catalog of the selected source symbols in a fixed contracts
/// version, given the version's complete rows as read by the data service.
pub fn source_catalog(
    preview: &Value,
    version_id: &str,
    symbols: &[String],
) -> Result<ReferenceCatalog> {
    let selected: BTreeSet<&str> = symbols.iter().map(String::as_str).collect();
    if selected.len() != symbols.len() {
        return Err("选择的来源代码重复".into());
    }
    if symbols.is_empty() || symbols.len() > LIMIT as usize {
        return Err("所选来源代码数量无效".into());
    }
    let version = &preview["version"];
    if version["id"] != version_id {
        return Err("合约资料版本与请求不一致".into());
    }
    let manifest = &version["manifest"];
    let evidence = CatalogInput {
        version_id: version_id.into(),
        checksum: text(manifest, "checksum").unwrap_or_default().into(),
        source: text(manifest, "source").unwrap_or_default().into(),
    };
    validate_catalog_input(&evidence, manifest)?;
    let (_, rows) = complete(preview)?;
    let mut chosen: Vec<&Value> = rows
        .iter()
        .filter(|row| text(row, "symbol").is_some_and(|symbol| selected.contains(symbol)))
        .collect();
    let owned: Vec<Value> = chosen.iter().map(|row| (*row).clone()).collect();
    types::validate(TYPE, &owned)?;
    let found: BTreeSet<&str> = chosen
        .iter()
        .filter_map(|row| text(row, "symbol"))
        .collect();
    if found != selected {
        return Err("所选来源代码不在固定合约资料版本中".into());
    }
    let created = recorded_at(
        version["created_at"]
            .as_f64()
            .ok_or("合约资料版本登记时间无效")?,
    )?;
    let mut provenance: Provenance = serde_json::from_value(json!({
        "source": evidence.source,
        "source_version": evidence.version_id,
        "observed_at": manifest["observed_at"],
        "available_at": manifest["available_at"],
    }))
    .map_err(|error| error.to_string())?;
    if created > provenance.available_at {
        provenance.available_at = created;
    }
    let listed = |row: &Value| text(row, "listed").unwrap_or_default().to_string();
    chosen.sort_by_key(|row| {
        (
            text(row, "exchange").unwrap_or_default().to_string(),
            text(row, "symbol").unwrap_or_default().to_string(),
            listed(row),
        )
    });
    let mut products: BTreeMap<String, Product> = BTreeMap::new();
    let (mut contracts, mut mappings) = (Vec::new(), Vec::new());
    for row in chosen {
        let symbol = text(row, "symbol").unwrap_or_default();
        let (Some(month), Some(last_trade)) = (text(row, "delivery_month"), day(row, "delisted")?)
        else {
            return Err(format!(
                "{symbol} 缺少完整交割年月或最后交易日，不能发布身份目录"
            ));
        };
        let exchange = text(row, "exchange").unwrap_or_default();
        let product_id = format!("{exchange}.{}", text(row, "product").unwrap_or_default());
        let product = Product {
            id: product_id.clone(),
            exchange: serde_json::from_value::<Exchange>(json!(exchange))
                .map_err(|e| e.to_string())?,
            // The source product code is a label, not an inferred Chinese name.
            name: text(row, "product").unwrap_or_default().into(),
            currency: text(row, "currency").unwrap_or_default().into(),
            provenance: provenance.clone(),
        };
        product.validate()?;
        if products
            .get(&product_id)
            .is_some_and(|known| known != &product)
        {
            return Err("同一品种的资料存在冲突".into());
        }
        products.insert(product_id.clone(), product);
        let listed_on = day(row, "listed")?.ok_or("合约资料缺少上市日期")?;
        let contract = Contract {
            id: format!(
                "{product_id}.{}.{}",
                month.replace('-', ""),
                listed_on.format("%Y%m%d")
            ),
            product_id,
            delivery_month: month.into(),
            listed_on,
            last_trade_on: last_trade,
            last_delivery_on: day(row, "last_delivery_on")?,
            provenance: provenance.clone(),
        };
        contract.validate()?;
        let mapping = SourceSymbol {
            source: evidence.source.clone(),
            symbol: symbol.into(),
            contract_id: contract.id.clone(),
            valid_from: listed_on,
            valid_until: last_trade,
            provenance: provenance.clone(),
        };
        mapping.validate()?;
        contracts.push(contract);
        mappings.push(mapping);
    }
    let catalog = ReferenceCatalog {
        schema_version: 2,
        inputs: vec![evidence],
        products: products.into_values().collect(),
        contracts,
        symbols: mappings,
    };
    catalog.validate()?;
    Ok(catalog)
}

/// Every identity of one product in a fixed source version; no claim of
/// exchange-wide completeness.
pub fn product_catalog(
    preview: &Value,
    version_id: &str,
    product_id: &str,
) -> Result<ReferenceCatalog> {
    let (_, rows) = complete(preview).map_err(|_| "品种候选资料未完整读取".to_string())?;
    let symbols: BTreeSet<String> = rows
        .iter()
        .filter(|row| {
            format!(
                "{}.{}",
                text(row, "exchange").unwrap_or_default(),
                text(row, "product").unwrap_or_default()
            ) == product_id
        })
        .filter_map(|row| text(row, "symbol").map(str::to_string))
        .collect();
    if symbols.is_empty() {
        return Err("固定合约资料中没有该品种".into());
    }
    source_catalog(
        preview,
        version_id,
        &symbols.into_iter().collect::<Vec<_>>(),
    )
}

/// The one canonical lifecycle `contract_id` names in a fixed contracts
/// version, with the source row it was observed in.
pub fn source_contract(preview: &Value, contract_id: &str) -> Result<(Contract, Value)> {
    let rows = preview["rows"]
        .as_array()
        .ok_or("资料中没有唯一匹配的规范合约身份")?;
    let named = |row: &&Value| {
        let (Some(month), Ok(Some(listed))) = (text(row, "delivery_month"), day(row, "listed"))
        else {
            return false;
        };
        let product = format!(
            "{}.{}",
            text(row, "exchange").unwrap_or_default(),
            text(row, "product").unwrap_or_default()
        );
        self::contract_id(&product, month, listed) == contract_id
    };
    let matches: Vec<&Value> = rows.iter().filter(named).collect();
    let [row] = matches.as_slice() else {
        return Err("资料中没有唯一匹配的规范合约身份".into());
    };
    let symbol = text(row, "symbol").unwrap_or_default().to_string();
    let version_id = preview["version"]["id"].as_str().unwrap_or_default();
    let catalog = source_catalog(preview, version_id, &[symbol])?;
    let contract = catalog
        .contracts
        .into_iter()
        .find(|contract| contract.id == contract_id)
        .ok_or("资料中没有唯一匹配的规范合约身份")?;
    Ok((contract, (*row).clone()))
}
