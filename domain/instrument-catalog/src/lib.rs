//! Immutable instrument identities and explicitly sourced, point-in-time mappings.
//! No storage, source adapters, symbol-year inference, or system-clock dependencies.

use chrono::{DateTime, Datelike, FixedOffset, NaiveDate};
use regex::Regex;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::LazyLock;

pub type Result<T> = std::result::Result<T, String>;

pub trait Validate {
    fn validate(&self) -> Result<()>;
}
fn require(ok: bool, message: &str) -> Result<()> {
    if ok { Ok(()) } else { Err(message.into()) }
}
fn length(value: &str, min: usize, max: usize, field: &str) -> Result<()> {
    require(
        (min..=max).contains(&value.chars().count()),
        &format!("{field}: invalid length"),
    )
}
fn valid_date(date: NaiveDate) -> Result<()> {
    require(
        (1..=9999).contains(&date.year()),
        "date year must be in 1..9999",
    )
}
fn valid_stamp(stamp: DateTime<FixedOffset>) -> Result<()> {
    valid_date(stamp.date_naive())?;
    require(
        stamp.timestamp_subsec_nanos() < 1_000_000_000,
        "leap seconds are unsupported",
    )?;
    require(
        stamp.timestamp_subsec_nanos().is_multiple_of(1000),
        "datetime precision must not exceed microseconds",
    )
}
fn parsed<T: DeserializeOwned + Validate>(value: Value) -> Result<T> {
    let model: T = serde_json::from_value(value).map_err(|e| e.to_string())?;
    model.validate()?;
    Ok(model)
}
fn output<T: Serialize>(value: T) -> Result<Value> {
    serde_json::to_value(value).map_err(|e| e.to_string())
}

// Match the current JSON datetime contract: UTC Z, otherwise explicit offset;
// omit zero fractions and emit six digits when microseconds are present.
mod stamp {
    use chrono::{DateTime, FixedOffset, Timelike};
    use serde::{Deserialize, Deserializer, Serializer};
    pub fn serialize<S: Serializer>(
        value: &DateTime<FixedOffset>,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        let mut result = value.format("%Y-%m-%dT%H:%M:%S").to_string();
        if value.nanosecond() != 0 {
            result.push_str(&format!(".{:06}", value.nanosecond() / 1000));
        }
        let offset = value.offset().local_minus_utc();
        if offset == 0 {
            result.push('Z');
        } else {
            // The wire contract truncates historical sub-minute offsets.
            let minutes = offset.unsigned_abs() / 60;
            result.push_str(&format!(
                "{}{:02}:{:02}",
                if offset < 0 { '-' } else { '+' },
                minutes / 60,
                minutes % 60
            ));
        }
        serializer.serialize_str(&result)
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<DateTime<FixedOffset>, D::Error> {
        let value = String::deserialize(deserializer)?;
        let parsed = DateTime::parse_from_rfc3339(&value).map_err(serde::de::Error::custom)?;
        let parsed = parsed
            .with_nanosecond(parsed.nanosecond() / 1000 * 1000)
            .ok_or_else(|| serde::de::Error::custom("invalid datetime"))?;
        super::valid_stamp(parsed).map_err(serde::de::Error::custom)?;
        Ok(parsed)
    }
}
fn required_option<'de, D: serde::Deserializer<'de>, T: Deserialize<'de>>(
    d: D,
) -> std::result::Result<Option<T>, D::Error> {
    Option::<T>::deserialize(d)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub enum Exchange {
    SHFE,
    DCE,
    CZCE,
    CFFEX,
    INE,
    GFEX,
}
impl Exchange {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::SHFE => "SHFE",
            Self::DCE => "DCE",
            Self::CZCE => "CZCE",
            Self::CFFEX => "CFFEX",
            Self::INE => "INE",
            Self::GFEX => "GFEX",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Provenance {
    #[schemars(length(min = 1))]
    pub source: String,
    #[schemars(length(min = 1))]
    pub source_version: String,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub observed_at: DateTime<FixedOffset>,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub available_at: DateTime<FixedOffset>,
}
impl Validate for Provenance {
    fn validate(&self) -> Result<()> {
        length(&self.source, 1, usize::MAX, "source")?;
        length(&self.source_version, 1, usize::MAX, "source_version")?;
        valid_stamp(self.observed_at)?;
        valid_stamp(self.available_at)?;
        require(
            self.available_at >= self.observed_at,
            "available_at must not precede observed_at",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Product {
    #[schemars(regex(pattern = r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+$"))]
    pub id: String,
    pub exchange: Exchange,
    #[schemars(length(min = 1))]
    pub name: String,
    #[schemars(regex(pattern = r"^[A-Z]{3}$"))]
    pub currency: String,
    pub provenance: Provenance,
}
impl Validate for Product {
    fn validate(&self) -> Result<()> {
        static ID: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+$").unwrap());
        require(ID.is_match(&self.id), "invalid product id")?;
        length(&self.name, 1, usize::MAX, "name")?;
        require(
            self.currency.len() == 3 && self.currency.bytes().all(|c| c.is_ascii_uppercase()),
            "invalid currency",
        )?;
        self.provenance.validate()?;
        require(
            self.id.starts_with(&format!("{}.", self.exchange.as_str())),
            "Product ID and exchange disagree",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Contract {
    #[schemars(length(min = 1, max = 100))]
    pub id: String,
    pub product_id: String,
    #[schemars(regex(pattern = r"^[0-9]{4}-(0[1-9]|1[0-2])$"))]
    pub delivery_month: String,
    pub listed_on: NaiveDate,
    pub last_trade_on: NaiveDate,
    #[serde(deserialize_with = "required_option")]
    pub last_delivery_on: Option<NaiveDate>,
    pub provenance: Provenance,
}
impl Validate for Contract {
    fn validate(&self) -> Result<()> {
        length(&self.id, 1, 100, "id")?;
        static MONTH: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^[0-9]{4}-(0[1-9]|1[0-2])$").unwrap());
        require(
            MONTH.is_match(&self.delivery_month),
            "invalid delivery_month",
        )?;
        let month = NaiveDate::parse_from_str(&format!("{}-01", self.delivery_month), "%Y-%m-%d")
            .map_err(|e| e.to_string())?;
        valid_date(month)?;
        valid_date(self.listed_on)?;
        valid_date(self.last_trade_on)?;
        if let Some(date) = self.last_delivery_on {
            valid_date(date)?;
        }
        self.provenance.validate()?;
        let expected = format!(
            "{}.{}.{}",
            self.product_id,
            self.delivery_month.replace('-', ""),
            self.listed_on.format("%Y%m%d")
        );
        require(
            self.id == expected,
            "实际合约身份必须匹配品种、完整交割年月与上市日期",
        )?;
        require(
            self.last_trade_on >= self.listed_on,
            "最后交易日不得早于上市日",
        )?;
        require(
            self.last_delivery_on
                .is_none_or(|date| date >= self.last_trade_on),
            "最后交割日不得早于最后交易日",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SourceSymbol {
    #[schemars(regex(pattern = r"^[A-Za-z][A-Za-z0-9_.:-]{0,63}$"))]
    pub source: String,
    #[schemars(length(min = 1, max = 64), regex(pattern = r"^\S+$"))]
    pub symbol: String,
    #[schemars(length(min = 1, max = 100))]
    pub contract_id: String,
    pub valid_from: NaiveDate,
    pub valid_until: NaiveDate,
    pub provenance: Provenance,
}
impl Validate for SourceSymbol {
    fn validate(&self) -> Result<()> {
        static SOURCE: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"^[A-Za-z][A-Za-z0-9_.:-]{0,63}$").unwrap());
        require(SOURCE.is_match(&self.source), "invalid source")?;
        length(&self.symbol, 1, 64, "symbol")?;
        require(
            !self.symbol.chars().any(char::is_whitespace),
            "invalid symbol",
        )?;
        length(&self.contract_id, 1, 100, "contract_id")?;
        valid_date(self.valid_from)?;
        valid_date(self.valid_until)?;
        self.provenance.validate()?;
        require(
            self.valid_until >= self.valid_from,
            "来源代码有效日期范围无效",
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ResolutionRequest {
    #[schemars(length(min = 1, max = 64))]
    pub source: String,
    #[schemars(length(min = 1, max = 64))]
    pub symbol: String,
    pub trading_day: NaiveDate,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub information_at: DateTime<FixedOffset>,
}
impl Validate for ResolutionRequest {
    fn validate(&self) -> Result<()> {
        length(&self.source, 1, 64, "source")?;
        length(&self.symbol, 1, 64, "symbol")?;
        valid_date(self.trading_day)?;
        valid_stamp(self.information_at)
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ContractResolution {
    pub contract: Contract,
    pub mapping: SourceSymbol,
}
impl Validate for ContractResolution {
    fn validate(&self) -> Result<()> {
        self.contract.validate()?;
        self.mapping.validate()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CatalogInput {
    #[schemars(length(min = 1, max = 100))]
    pub version_id: String,
    #[schemars(regex(pattern = r"^[0-9a-f]{64}$"))]
    pub checksum: String,
    #[schemars(length(min = 1, max = 64))]
    pub source: String,
}
fn fingerprint(value: &str) -> Result<()> {
    require(
        value.len() == 64
            && value
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)),
        "invalid SHA-256 fingerprint",
    )
}
impl Validate for CatalogInput {
    fn validate(&self) -> Result<()> {
        length(&self.version_id, 1, 100, "version_id")?;
        fingerprint(&self.checksum)?;
        length(&self.source, 1, 64, "source")
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ReferenceCatalog {
    #[schemars(range(min = 2, max = 2))]
    pub schema_version: u8,
    pub inputs: Vec<CatalogInput>,
    pub products: Vec<Product>,
    pub contracts: Vec<Contract>,
    pub symbols: Vec<SourceSymbol>,
}
impl Validate for ReferenceCatalog {
    fn validate(&self) -> Result<()> {
        require(
            self.schema_version == 2,
            "unsupported reference catalog schema_version",
        )?;
        let mut inputs = BTreeSet::new();
        for item in &self.inputs {
            item.validate()?;
            require(inputs.insert(&item.version_id), "目录输入版本重复")?;
        }
        let mut products = BTreeSet::new();
        let mut contracts = BTreeMap::new();
        for item in &self.products {
            item.validate()?;
            require(
                products.insert(&item.id),
                "Duplicate product or contract ID",
            )?;
        }
        let mut lifecycles: BTreeMap<(&str, &str), Vec<&Contract>> = BTreeMap::new();
        for item in &self.contracts {
            item.validate()?;
            require(
                contracts.insert(&item.id, item).is_none(),
                "Duplicate product or contract ID",
            )?;
            require(
                products.contains(&item.product_id),
                "Unknown contract product",
            )?;
            lifecycles
                .entry((&item.product_id, &item.delivery_month))
                .or_default()
                .push(item);
        }
        for group in lifecycles.values_mut() {
            group.sort_by_key(|c| c.listed_on);
            require(
                group
                    .windows(2)
                    .all(|pair| pair[0].last_trade_on < pair[1].listed_on),
                "同一品种交割月份存在重叠合约生命周期",
            )?;
        }
        let mut groups: BTreeMap<(&str, &str), Vec<&SourceSymbol>> = BTreeMap::new();
        for item in &self.symbols {
            item.validate()?;
            let contract = contracts
                .get(&item.contract_id)
                .ok_or("来源代码引用未知实际合约")?;
            require(
                contract.listed_on <= item.valid_from && item.valid_until <= contract.last_trade_on,
                "来源代码有效期超出实际合约上市范围",
            )?;
            groups
                .entry((&item.source, &item.symbol))
                .or_default()
                .push(item);
        }
        for group in groups.values_mut() {
            group.sort_by_key(|m| m.valid_from);
            require(
                group
                    .windows(2)
                    .all(|pair| pair[0].valid_until < pair[1].valid_from),
                "同一来源代码存在重叠映射",
            )?;
        }
        Ok(())
    }
}

/// Validated immutable catalog with reusable indexes. Construction validates once.
#[derive(Debug, Clone)]
pub struct Catalog {
    model: ReferenceCatalog,
    id: String,
    products: BTreeMap<String, usize>,
    contracts: BTreeMap<String, usize>,
    symbols: BTreeMap<String, BTreeMap<String, Vec<usize>>>,
}
impl Catalog {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(model: ReferenceCatalog) -> Result<Self> {
        model.validate()?;
        let bytes = asterion_foundation::canonical(&output(&model)?)?;
        require(bytes.len() <= 4_000_000, "Reference catalog exceeds 4 MB")?;
        let id = asterion_foundation::digest(&output(&model)?)?;
        let products = model
            .products
            .iter()
            .enumerate()
            .map(|(i, v)| (v.id.clone(), i))
            .collect();
        let contracts = model
            .contracts
            .iter()
            .enumerate()
            .map(|(i, v)| (v.id.clone(), i))
            .collect();
        let mut symbols: BTreeMap<String, BTreeMap<String, Vec<usize>>> = BTreeMap::new();
        for (index, mapping) in model.symbols.iter().enumerate() {
            symbols
                .entry(mapping.source.clone())
                .or_default()
                .entry(mapping.symbol.clone())
                .or_default()
                .push(index);
        }
        for group in symbols.values_mut().flat_map(|source| source.values_mut()) {
            group.sort_by_key(|i| model.symbols[*i].valid_from);
        }
        Ok(Self {
            model,
            id,
            products,
            contracts,
            symbols,
        })
    }
    pub fn model(&self) -> &ReferenceCatalog {
        &self.model
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn resolve(&self, request: &ResolutionRequest) -> Result<ContractResolution> {
        request.validate()?;
        let group = self
            .symbols
            .get(&request.source)
            .and_then(|source| source.get(&request.symbol))
            .ok_or("没有唯一适用的来源代码映射")?;
        let position =
            group.partition_point(|i| self.model.symbols[*i].valid_from <= request.trading_day);
        let index = position
            .checked_sub(1)
            .ok_or("没有唯一适用的来源代码映射")?;
        let mapping = &self.model.symbols[group[index]];
        require(
            request.trading_day <= mapping.valid_until,
            "没有唯一适用的来源代码映射",
        )?;
        let contract = &self.model.contracts[self.contracts[&mapping.contract_id]];
        let product = &self.model.products[self.products[&contract.product_id]];
        require(
            [
                &mapping.provenance,
                &contract.provenance,
                &product.provenance,
            ]
            .iter()
            .all(|p| p.available_at <= request.information_at),
            "身份或映射依据在指定信息截止时间尚不可知",
        )?;
        Ok(ContractResolution {
            contract: contract.clone(),
            mapping: mapping.clone(),
        })
    }
}
pub fn catalog_id(model: &ReferenceCatalog) -> Result<String> {
    Ok(Catalog::new(model.clone())?.id)
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SourceIdentity {
    pub catalog_id: String,
    pub catalog: ReferenceCatalog,
    pub source: String,
    pub symbol: String,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub information_at: DateTime<FixedOffset>,
}
impl Validate for SourceIdentity {
    fn validate(&self) -> Result<()> {
        SourceResolver::new(self.clone()).map(|_| ())
    }
}
#[derive(Debug, Clone)]
pub struct SourceResolver {
    identity: SourceIdentity,
    catalog: Catalog,
}
impl SourceResolver {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(identity: SourceIdentity) -> Result<Self> {
        valid_stamp(identity.information_at)?;
        let catalog = Catalog::new(identity.catalog.clone())?;
        require(identity.catalog_id == catalog.id, "来源身份目录指纹不一致")?;
        require(
            identity.catalog.inputs.len() == 1
                && identity.catalog.inputs[0].source == identity.source,
            "来源身份必须绑定唯一同源资料版本",
        )?;
        require(
            catalog
                .symbols
                .get(&identity.source)
                .is_some_and(|m| m.contains_key(&identity.symbol)),
            "来源身份代码不在目录中",
        )?;
        Ok(Self { identity, catalog })
    }
    pub fn model(&self) -> &SourceIdentity {
        &self.identity
    }
    pub fn resolve(&self, day: NaiveDate) -> Result<Contract> {
        Ok(self
            .catalog
            .resolve(&ResolutionRequest {
                source: self.identity.source.clone(),
                symbol: self.identity.symbol.clone(),
                trading_day: day,
                information_at: self.identity.information_at,
            })?
            .contract)
    }
    pub fn validate_rows(&self, rows: &[Value], exchange: &str) -> Result<Vec<String>> {
        let mut ids = BTreeSet::new();
        for row in rows {
            let actual = self.resolve(row_date(row, "trading_day")?)?;
            require(
                row_string(row, "symbol")? == self.identity.symbol
                    && row_string(row, "exchange")? == exchange
                    && actual.product_id.starts_with(&format!("{exchange}.")),
                "行情记录与固定来源合约不一致",
            )?;
            validate_market_code(row_string(row, "contract")?, &actual)?;
            ids.insert(actual.id);
        }
        require(ids.len() == 1, "同步数据必须属于单一实际合约生命周期")?;
        Ok(ids.into_iter().collect())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct FileSymbol {
    #[schemars(length(min = 1, max = 100))]
    pub contract: String,
    #[schemars(length(min = 1, max = 64))]
    pub source: String,
    #[schemars(length(min = 1, max = 64))]
    pub symbol: String,
}
impl Validate for FileSymbol {
    fn validate(&self) -> Result<()> {
        length(&self.contract, 1, 100, "contract")?;
        length(&self.source, 1, 64, "source")?;
        length(&self.symbol, 1, 64, "symbol")
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ImportIdentity {
    #[schemars(regex(pattern = r"^[0-9a-f]{64}$"))]
    pub catalog_id: String,
    pub catalog: ReferenceCatalog,
    #[serde(with = "stamp")]
    #[schemars(with = "DateTime<FixedOffset>")]
    pub information_at: DateTime<FixedOffset>,
    #[schemars(length(min = 1, max = 10000))]
    pub bindings: Vec<FileSymbol>,
}
impl Validate for ImportIdentity {
    fn validate(&self) -> Result<()> {
        ImportResolver::new(self.clone()).map(|_| ())
    }
}
#[derive(Debug, Clone)]
pub struct ImportResolver {
    identity: ImportIdentity,
    catalog: Catalog,
    bindings: BTreeMap<String, usize>,
}
impl ImportResolver {
    pub fn from_value(value: Value) -> Result<Self> {
        Self::new(serde_json::from_value(value).map_err(|e| e.to_string())?)
    }
    pub fn new(identity: ImportIdentity) -> Result<Self> {
        fingerprint(&identity.catalog_id)?;
        valid_stamp(identity.information_at)?;
        require(
            (1..=10000).contains(&identity.bindings.len()),
            "bindings: invalid length",
        )?;
        let catalog = Catalog::new(identity.catalog.clone())?;
        require(identity.catalog_id == catalog.id, "导入合约目录指纹不一致")?;
        let mut bindings = BTreeMap::new();
        for (index, binding) in identity.bindings.iter().enumerate() {
            binding.validate()?;
            require(
                bindings.insert(binding.contract.clone(), index).is_none(),
                "文件合约代码映射重复",
            )?;
            require(
                catalog
                    .symbols
                    .get(&binding.source)
                    .is_some_and(|source| source.contains_key(&binding.symbol)),
                "文件代码映射不在固定目录中",
            )?;
        }
        Ok(Self {
            identity,
            catalog,
            bindings,
        })
    }
    pub fn model(&self) -> &ImportIdentity {
        &self.identity
    }
    pub fn resolve(&self, contract: &str, day: NaiveDate) -> Result<Contract> {
        let index = self
            .bindings
            .get(contract)
            .ok_or("文件合约缺少明确的身份映射")?;
        let binding = &self.identity.bindings[*index];
        let actual = self
            .catalog
            .resolve(&ResolutionRequest {
                source: binding.source.clone(),
                symbol: binding.symbol.clone(),
                trading_day: day,
                information_at: self.identity.information_at,
            })?
            .contract;
        validate_market_code(contract, &actual)?;
        Ok(actual)
    }
    pub fn validate_rows(&self, rows: &[Value]) -> Result<()> {
        let actual: BTreeSet<&str> = rows
            .iter()
            .map(|row| row_string(row, "contract"))
            .collect::<Result<_>>()?;
        let expected: BTreeSet<&str> = self.bindings.keys().map(String::as_str).collect();
        require(actual == expected, "身份映射必须与文件合约集合完全一致")?;
        for row in rows {
            self.resolve(row_string(row, "contract")?, row_date(row, "trading_day")?)?;
        }
        Ok(())
    }
}
fn row_string<'a>(row: &'a Value, key: &str) -> Result<&'a str> {
    row.get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("row.{key}: expected string"))
}
fn row_date(row: &Value, key: &str) -> Result<NaiveDate> {
    let day =
        NaiveDate::parse_from_str(row_string(row, key)?, "%Y-%m-%d").map_err(|e| e.to_string())?;
    valid_date(day)?;
    Ok(day)
}

pub fn validate_market_code(code: &str, actual: &Contract) -> Result<()> {
    actual.validate()?;
    static CODE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"^([A-Z]+)\.([A-Za-z]+)([0-9]{3,4})$").unwrap());
    let captures = CODE.captures(code).ok_or("文件代码与实际合约品种不一致")?;
    require(
        format!("{}.{}", &captures[1], captures[2].to_ascii_uppercase()) == actual.product_id,
        "文件代码与实际合约品种不一致",
    )?;
    require(
        actual
            .delivery_month
            .replace('-', "")
            .ends_with(&captures[3]),
        "文件代码月份与目录的完整交割年月不一致",
    )
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ReferenceRelease {
    pub id: String,
    pub published_at: f64,
    pub catalog: ReferenceCatalog,
}
impl Validate for ReferenceRelease {
    fn validate(&self) -> Result<()> {
        require(self.published_at.is_finite(), "published_at must be finite")?;
        require(
            self.id == catalog_id(&self.catalog)?,
            "合约目录版本指纹不一致",
        )
    }
}

/// JSON boundary for bindings. Operation payloads and nested domain objects are strict.
pub fn invoke(operation: &str, input: Value) -> Result<Value> {
    macro_rules! request {($name:ident {$($field:ident:$ty:ty),* $(,)?})=>{{
        #[derive(Deserialize)] #[serde(deny_unknown_fields)] struct $name {$($field:$ty),*}
        serde_json::from_value::<$name>(input).map_err(|e|e.to_string())?
    }};}
    match operation {
        "schema" => {
            request!(Request {});
            Ok(schema())
        }
        "validate" => {
            let r = request!(Request {
                model: String,
                value: Value
            });
            macro_rules! model {
                ($ty:ty) => {
                    output(parsed::<$ty>(r.value)?)
                };
            }
            match r.model.as_str() {
                "Provenance" => model!(Provenance),
                "Product" => model!(Product),
                "Contract" => model!(Contract),
                "SourceSymbol" => model!(SourceSymbol),
                "ResolutionRequest" => model!(ResolutionRequest),
                "ContractResolution" => model!(ContractResolution),
                "CatalogInput" => model!(CatalogInput),
                "ReferenceCatalog" => model!(ReferenceCatalog),
                "SourceIdentity" => model!(SourceIdentity),
                "FileSymbol" => model!(FileSymbol),
                "ImportIdentity" => model!(ImportIdentity),
                "ReferenceRelease" => model!(ReferenceRelease),
                _ => Err(format!("unsupported catalog model: {}", r.model)),
            }
        }
        "catalog_id" => {
            let r = request!(Request { catalog: Value });
            output(Catalog::from_value(r.catalog)?.id())
        }
        "snapshot" => {
            let r = request!(Request {
                catalog: Value,
                published_at: f64
            });
            let catalog = Catalog::from_value(r.catalog)?;
            let result = ReferenceRelease {
                id: catalog.id.clone(),
                published_at: r.published_at,
                catalog: catalog.model,
            };
            result.validate()?;
            output(result)
        }
        "resolve" => {
            let r = request!(Request {
                catalog: Value,
                request: Value
            });
            output(Catalog::from_value(r.catalog)?.resolve(&parsed(r.request)?)?)
        }
        "source_resolve" => {
            let r = request!(Request {
                identity: Value,
                day: NaiveDate
            });
            output(SourceResolver::from_value(r.identity)?.resolve(r.day)?)
        }
        "source_validate_rows" => {
            let r = request!(Request{identity:Value,rows:Vec<Value>,exchange:String});
            output(SourceResolver::from_value(r.identity)?.validate_rows(&r.rows, &r.exchange)?)
        }
        "import_resolve" => {
            let r = request!(Request {
                identity: Value,
                contract: String,
                day: NaiveDate
            });
            output(ImportResolver::from_value(r.identity)?.resolve(&r.contract, r.day)?)
        }
        "import_validate_rows" => {
            let r = request!(Request{identity:Value,rows:Vec<Value>});
            ImportResolver::from_value(r.identity)?.validate_rows(&r.rows)?;
            Ok(Value::Null)
        }
        "validate_market_code" => {
            let r = request!(Request {
                contract: String,
                actual: Value
            });
            validate_market_code(&r.contract, &parsed(r.actual)?)?;
            Ok(Value::Null)
        }
        _ => Err(format!("unsupported catalog operation: {operation}")),
    }
}

pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[allow(dead_code)]
    #[serde(untagged)]
    enum Models {
        Provenance(Provenance),
        Product(Product),
        Contract(Contract),
        SourceSymbol(SourceSymbol),
        ResolutionRequest(ResolutionRequest),
        ContractResolution(ContractResolution),
        CatalogInput(CatalogInput),
        ReferenceCatalog(ReferenceCatalog),
        SourceIdentity(SourceIdentity),
        FileSymbol(FileSymbol),
        ImportIdentity(ImportIdentity),
        ReferenceRelease(ReferenceRelease),
    }
    let mut schema =
        serde_json::to_value(schemars::schema_for!(Models)).expect("JSON schema is serializable");
    // Option represents JSON null; deserialize_with makes this field mandatory.
    // Marking schemars(required) would incorrectly erase the nullable branch.
    schema["$defs"]["Contract"]["required"]
        .as_array_mut()
        .expect("object required fields")
        .push(Value::String("last_delivery_on".into()));
    schema
}
#[cfg(test)]
mod tests;
