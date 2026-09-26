//! Tushare Pro futures adapter (built-in data source). Exact vendor values
//! remain in retrieval evidence; this adapter only plans requests, transports
//! them and maps vendor fields onto the data types.
use asterion_data_store::provider::{
    Capability, ConfigurationField, ConfigurationSpec, Partition, Provider, ProviderManifest,
    Result, SyncRequest,
};
use asterion_data_store::pyfmt::{decimal_text, float_repr, iso_date};
use asterion_data_store::table::Row;
use bigdecimal::{BigDecimal, Signed, Zero};
use chrono::{Duration, NaiveDate, NaiveDateTime, TimeZone};
use chrono_tz::Asia::Shanghai;
use regex::Regex;
use serde_json::{Map, Value, json};
use std::collections::{BTreeMap, BTreeSet};

const EXCHANGES: [&str; 6] = ["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"];
const ENDPOINT: &str = "https://api.tushare.pro";
const MINUTE_FIELDS: [&str; 9] = [
    "ts_code",
    "trade_time",
    "open",
    "close",
    "high",
    "low",
    "vol",
    "amount",
    "oi",
];

fn suffix(exchange: &str) -> &'static str {
    match exchange {
        "SHFE" => "SHF",
        "DCE" => "DCE",
        "CZCE" => "ZCE",
        "CFFEX" => "CFX",
        "INE" => "INE",
        _ => "GFE",
    }
}

fn fields(dataset: &str) -> Vec<String> {
    let text = match dataset {
        "mapping" => "ts_code,trade_date,mapping_ts_code",
        "contracts" => {
            "ts_code,symbol,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate"
        }
        "settlement" => {
            "ts_code,trade_date,settle,trading_fee_rate,trading_fee,delivery_fee,b_hedging_margin_rate,s_hedging_margin_rate,long_margin_rate,short_margin_rate,offset_today_fee,exchange"
        }
        "calendar" => "exchange,cal_date,is_open,pretrade_date",
        _ => {
            "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi,oi_chg"
        }
    };
    text.split(',').map(String::from).collect()
}

fn matches(pattern: &str, text: &str) -> bool {
    Regex::new(&format!("^(?:{pattern})$"))
        .expect("pattern")
        .is_match(text)
}

/// Python's `str()` of a vendor JSON value.
fn text(value: &Value) -> Option<String> {
    match value {
        Value::String(text) => Some(text.clone()),
        Value::Number(number) if number.is_f64() => Some(float_repr(number.as_f64()?)),
        Value::Number(number) => Some(number.to_string()),
        Value::Bool(flag) => Some(if *flag { "True" } else { "False" }.into()),
        Value::Null => Some("None".into()),
        _ => None,
    }
}

fn truthy(value: Option<&Value>) -> bool {
    match value {
        None | Some(Value::Null) => false,
        Some(Value::String(text)) => !text.is_empty(),
        Some(Value::Bool(flag)) => *flag,
        Some(Value::Number(number)) => number.as_f64() != Some(0.0),
        Some(Value::Array(items)) => !items.is_empty(),
        Some(Value::Object(fields)) => !fields.is_empty(),
    }
}

fn day(value: &Value) -> Result<String> {
    text(value)
        .and_then(|t| iso_date(&t).ok())
        .map(|d| d.format("%Y-%m-%d").to_string())
        .ok_or_else(|| "数据校验失败：日期格式不正确".into())
}

/// `Decimal(str(value))` with its canonical text.
fn decimal(value: &Value) -> Option<(String, BigDecimal)> {
    let canonical = text(value).and_then(|t| decimal_text(&t))?;
    let parsed = canonical.parse().ok()?;
    Some((canonical, parsed))
}

/// A finite decimal, stored as `str(Decimal)`.
fn number(value: &Value, nonnegative: bool) -> Result<Value> {
    match decimal(value) {
        Some((canonical, parsed)) if !(nonnegative && parsed < BigDecimal::zero()) => {
            Ok(json!(canonical))
        }
        _ => Err("数据校验失败：数值缺失或非法".into()),
    }
}

fn optional(value: Option<&Value>, nonnegative: bool) -> Result<Value> {
    match value {
        None | Some(Value::Null) => Ok(Value::Null),
        Some(value) => number(value, nonnegative),
    }
}

const MISSING: &str = "数据校验失败：缺失必需字段";

fn field<'a>(row: &'a Value, name: &str) -> Result<&'a Value> {
    row.get(name).ok_or_else(|| MISSING.into())
}

fn string<'a>(row: &'a Value, name: &str) -> Result<&'a str> {
    field(row, name)?.as_str().ok_or_else(|| MISSING.into())
}

pub struct Tushare {
    endpoint: String,
    backoff: std::time::Duration,
}

/// Test builds of dependent crates may point the vendor endpoint at a local
/// stand-in; release builds have no such setting.
#[cfg(feature = "test-endpoint")]
static TEST_ENDPOINT: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);

#[cfg(feature = "test-endpoint")]
pub fn set_test_endpoint(endpoint: &str) {
    *TEST_ENDPOINT.lock().expect("endpoint") = Some(endpoint.into());
}

#[cfg(feature = "test-endpoint")]
fn endpoint() -> String {
    TEST_ENDPOINT
        .lock()
        .expect("endpoint")
        .clone()
        .unwrap_or_else(|| ENDPOINT.into())
}
#[cfg(not(feature = "test-endpoint"))]
fn endpoint() -> String {
    ENDPOINT.into()
}

impl Default for Tushare {
    fn default() -> Self {
        Self {
            endpoint: endpoint(),
            backoff: std::time::Duration::from_secs(1),
        }
    }
}

/// Datasets as (id, data type, label, symbol required, description). Every
/// dataset except contracts is dated; the calendar excludes GFEX.
const DATASETS: [(&str, &str, &str, bool, &str); 6] = [
    (
        "mapping",
        "futures.role_mapping",
        "主力合约映射",
        true,
        "来源代码例如 RB.SHF；供应商每日主力对应月合约，历史公布时刻未知",
    ),
    (
        "contracts",
        "futures.contracts",
        "期货合约资料",
        false,
        "普通合约基础资料；不包含完整交易规则",
    ),
    (
        "calendar",
        "futures.calendar",
        "期货交易日历",
        false,
        "交易日与休市日；不包含日内交易时段",
    ),
    (
        "settlement",
        "futures.settlement",
        "每日结算参数",
        true,
        "盘后参数快照；费率单位与历史生效时间须另行确认",
    ),
    (
        "daily",
        "futures.daily",
        "期货历史日线",
        true,
        "单个实际合约的日线、结算价、成交量与持仓量",
    ),
    (
        "minute",
        "futures.minute",
        "期货历史分钟",
        true,
        "实际月份合约1/5/15/30/60分钟；须独立分钟权限及固定交易时段，单任务一个交易日",
    ),
];

fn capabilities() -> Vec<Capability> {
    let strings = |items: &[&str]| items.iter().map(|item| (*item).into()).collect();
    DATASETS
        .iter()
        .map(
            |&(id, type_id, label, symbol_required, description)| Capability {
                frequencies: if id == "minute" {
                    strings(&["1m", "5m", "15m", "30m", "60m"])
                } else {
                    Vec::new()
                },
                id: id.into(),
                label: label.into(),
                type_id: type_id.into(),
                exchanges: strings(if id == "calendar" {
                    &EXCHANGES[..5]
                } else {
                    &EXCHANGES
                }),
                date_range: id != "contracts",
                symbol_required,
                description: description.into(),
                defaults: Map::new(),
            },
        )
        .collect()
}

impl Tushare {
    fn request(&self, partition: &Partition, token: &str) -> Result<Vec<Value>> {
        let client = reqwest::blocking::Client::builder()
            .timeout(std::time::Duration::from_secs(30))
            .redirect(reqwest::redirect::Policy::none())
            .build()
            .map_err(|_| "无法连接 Tushare，请检查网络后重试")?;
        let body = json!({"api_name": partition.api, "token": token,
                          "params": partition.params, "fields": partition.fields.join(",")});
        for attempt in 0..3u32 {
            let retry = |attempt: u32| std::thread::sleep(self.backoff * (1 << attempt));
            let response = match client
                .post(&self.endpoint)
                .header("content-type", "application/json")
                .body(body.to_string())
                .send()
            {
                Ok(response) => response,
                Err(_) if attempt < 2 => {
                    retry(attempt);
                    continue;
                }
                Err(_) => return Err("无法连接 Tushare，请检查网络后重试".into()),
            };
            let status = response.status().as_u16();
            if status == 429 || status >= 500 {
                if attempt < 2 {
                    retry(attempt);
                    continue;
                }
                return Err("Tushare 限流或服务暂不可用，请稍后重试".into());
            }
            if status != 200 {
                return Err("Tushare 请求被拒绝，请检查 Token 和接口权限".into());
            }
            let bytes = match response.bytes() {
                Ok(bytes) => bytes,
                Err(_) if attempt < 2 => {
                    retry(attempt);
                    continue;
                }
                Err(_) => return Err("无法连接 Tushare，请检查网络后重试".into()),
            };
            let body: Value = serde_json::from_slice(&bytes).map_err(|_| "Tushare 返回格式异常")?;
            return records(&body, partition);
        }
        Err("Tushare 请求失败".into())
    }
}

/// The records of a vendor response, checked for structure and completeness.
fn records(body: &Value, partition: &Partition) -> Result<Vec<Value>> {
    let format = || "Tushare 返回格式异常".to_string();
    let object = body.as_object().ok_or_else(format)?;
    // Python equality: 0, 0.0 and False all accept.
    let accepted = match object.get("code") {
        Some(Value::Number(code)) => code.as_f64() == Some(0.0),
        Some(Value::Bool(flag)) => !flag,
        _ => false,
    };
    if !accepted {
        // Never echo msg: remote errors can contain request credentials.
        let message = object
            .get("msg")
            .map(|m| text(m).unwrap_or_else(|| m.to_string()))
            .unwrap_or_default()
            .to_lowercase();
        return Err(if message.contains("token") {
            "AUTH_FAILED：Tushare Token 无效或已失效"
        } else if message.contains("权限") || message.contains("积分") {
            "PERMISSION_DENIED：Tushare 账号没有该接口的积分或访问权限"
        } else if message.contains('频') || message.contains("每分钟") || message.contains("每天")
        {
            "RATE_LIMITED：Tushare 调用额度或频率受限，请稍后重试"
        } else {
            "PROVIDER_REJECTED：Tushare 拒绝请求，请检查账号权限或额度"
        }
        .into());
    }
    let data = object.get("data").ok_or_else(format)?;
    let (Some(names), Some(items)) = (data.get("fields"), data.get("items")) else {
        return Err(format());
    };
    let structure = || "Tushare 返回字段结构异常".to_string();
    let names: Vec<&str> = names
        .as_array()
        .ok_or_else(structure)?
        .iter()
        .map(|n| n.as_str().ok_or_else(structure))
        .collect::<Result<_>>()?;
    let unique: BTreeSet<&str> = names.iter().copied().collect();
    let items = items.as_array().ok_or_else(structure)?;
    if unique.len() != names.len()
        || !partition.fields.iter().all(|f| unique.contains(f.as_str()))
        || items
            .iter()
            .any(|r| r.as_array().is_none_or(|r| r.len() != names.len()))
    {
        return Err(structure());
    }
    if items.len() as u64 >= partition.limit {
        return Err("返回数据触及接口上限，无法确认完整性；未发布".into());
    }
    Ok(items
        .iter()
        .map(|item| {
            let values = item.as_array().expect("checked row");
            Value::Object(
                names
                    .iter()
                    .map(|n| n.to_string())
                    .zip(values.iter().cloned())
                    .collect(),
            )
        })
        .collect())
}

/// The source's full delivery year, never the displayed three/four digit code.
fn delivery_month(value: &Value) -> Result<Value> {
    match value {
        Value::Null => return Ok(Value::Null),
        Value::String(text) if text.is_empty() => return Ok(Value::Null),
        _ => {}
    }
    let text = value
        .as_str()
        .filter(|t| t.len() == 6 && t.bytes().all(|b| b.is_ascii_digit()))
        .ok_or("交割月份必须提供完整 YYYYMM，不能从代码推断年份")?;
    iso_date(&format!("{}-{}-01", &text[..4], &text[4..]))
        .map(|d| json!(d.format("%Y-%m").to_string()))
        .map_err(|_| "交割月份无效".into())
}

fn positive(value: &Value) -> Value {
    decimal(value)
        .filter(|(_, n)| n.is_positive() && n <= &BigDecimal::from(1_000_000))
        .map_or(Value::Null, |(canonical, _)| json!(canonical))
}

/// Interpret this provider's quotation fields before publishing standard evidence.
fn contract_multiplier(row: &Row) -> Result<(Value, String)> {
    let get = |name: &str| row.get(name).cloned().unwrap_or(Value::Null);
    let symbol = get("symbol");
    if get("exchange") == "CFFEX" {
        if symbol
            .as_str()
            .is_some_and(|s| matches(r"(?:IF|IH|IC|IM)[0-9]{4}\.CFX", s))
        {
            return Ok((
                positive(&get("multiplier")),
                "股指合约使用 fut_basic.multiplier；请确认每点每手金额。".into(),
            ));
        }
    } else if truthy(Some(&get("trade_unit"))) {
        let unit = get("trade_unit");
        let unit = unit.as_str().ok_or(MISSING)?;
        let quote = get("quote_unit");
        // Unhashable values cannot be quotation units.
        if quote.is_array() || quote.is_object() {
            return Err(MISSING.into());
        }
        if [format!("元/{unit}"), format!("人民币元/{unit}")]
            .iter()
            .any(|allowed| quote.as_str() == Some(allowed))
        {
            return Ok((
                positive(&get("per_unit")),
                "报价为元/交易单位，使用 fut_basic.per_unit 作为每手乘数；请确认单位。".into(),
            ));
        }
    }
    Ok((
        Value::Null,
        "报价和交易单位未能明确匹配，请核实后手动填写研究乘数。".into(),
    ))
}

fn row(fields: Vec<(&str, Value)>) -> Row {
    Row(fields
        .into_iter()
        .map(|(k, v)| (k.to_string(), v))
        .collect())
}

impl Tushare {
    fn minute_plan(&self, request: &SyncRequest) -> Result<Vec<Partition>> {
        if !matches(
            &format!(r"[A-Z]+[0-9]{{3,4}}\.{}", suffix(&request.exchange)),
            &request.symbol,
        ) {
            return Err("分钟同步须使用实际月份合约代码".into());
        }
        let (Some(frequency), Some(window), Some(start)) =
            (&request.frequency, &request.window, request.start)
        else {
            return Err("分钟同步须固定单个交易日及来源时间窗口".into());
        };
        if Some(start) != request.end {
            return Err("分钟同步须固定单个交易日及来源时间窗口".into());
        }
        let local = |stamp: &chrono::DateTime<chrono::FixedOffset>| {
            stamp
                .with_timezone(&Shanghai)
                .format("%Y-%m-%d %H:%M:%S")
                .to_string()
        };
        let params: BTreeMap<String, String> = [
            ("ts_code", request.symbol.clone()),
            ("freq", frequency.replace('m', "min")),
            ("start_date", local(&window.start)),
            ("end_date", local(&window.end)),
        ]
        .into_iter()
        .map(|(k, v)| (k.into(), v))
        .collect();
        let day = start.format("%Y-%m-%d").to_string();
        Ok(vec![Partition {
            api: "ft_mins".into(),
            params,
            fields: MINUTE_FIELDS.iter().map(|f| (*f).into()).collect(),
            limit: 8000,
            start: Some(day.clone()),
            end: Some(day),
        }])
    }

    fn minute_rows(&self, request: &SyncRequest, rows: &[Value]) -> Result<Vec<Row>> {
        let (Some(window), Some(start)) = (&request.window, request.start) else {
            return Err("分钟来源窗口缺失".into());
        };
        let refused = || "分钟来源记录缺失、重复或不符合请求".to_string();
        let mut seen = BTreeSet::new();
        let mut result = Vec::new();
        for record in rows {
            if record.get("ts_code").and_then(Value::as_str) != Some(request.symbol.as_str()) {
                return Err(refused());
            }
            let wall = record
                .get("trade_time")
                .and_then(Value::as_str)
                .ok_or_else(refused)?;
            let naive =
                NaiveDateTime::parse_from_str(wall, "%Y-%m-%d %H:%M:%S").map_err(|_| refused())?;
            let stamp = Shanghai
                .from_local_datetime(&naive)
                .single()
                .ok_or_else(refused)?;
            if stamp < window.start || stamp > window.end || !seen.insert(stamp) {
                return Err(refused());
            }
            let value = |name: &str, nonnegative: bool| {
                let value = record.get(name).ok_or_else(refused)?;
                number(value, nonnegative).map_err(|_| refused())
            };
            result.push(row(vec![
                ("symbol", json!(request.symbol)),
                ("frequency", json!(request.frequency)),
                (
                    "contract",
                    json!(format!(
                        "{}.{}",
                        request.exchange,
                        request.symbol.split('.').next().unwrap_or_default()
                    )),
                ),
                ("exchange", json!(request.exchange)),
                ("trading_day", json!(start.format("%Y-%m-%d").to_string())),
                ("event_time", json!(stamp.fixed_offset().to_rfc3339())),
                ("open", value("open", false)?),
                ("high", value("high", false)?),
                ("low", value("low", false)?),
                ("close", value("close", false)?),
                ("vol", value("vol", true)?),
                ("amount", value("amount", true)?),
                ("oi", value("oi", true)?),
            ]));
        }
        result.sort_by(|a, b| {
            let key = |r: &Row| {
                r.get("event_time")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string()
            };
            key(a).cmp(&key(b))
        });
        Ok(result)
    }

    fn normalized(&self, request: &SyncRequest, record: &Value) -> Result<(Row, String)> {
        let exchange = request.exchange.as_str();
        let contract_of = |symbol: &str| {
            format!(
                "{exchange}.{}",
                symbol.split('.').next().unwrap_or_default()
            )
        };
        match request.dataset.as_str() {
            "calendar" => {
                let value = day(field(record, "cal_date")?)?;
                if field(record, "exchange")? != exchange {
                    return Err("交易日历字段不符合请求".into());
                }
                // Python membership in (0, 1, "0", "1"): equal numbers and booleans count.
                let open = match field(record, "is_open")? {
                    Value::Number(n) if n.as_f64() == Some(0.0) => Some(0),
                    Value::Number(n) if n.as_f64() == Some(1.0) => Some(1),
                    Value::Bool(flag) => Some(i64::from(*flag)),
                    Value::String(t) if t == "0" => Some(0),
                    Value::String(t) if t == "1" => Some(1),
                    _ => None,
                };
                let Some(open) = open else {
                    return Err("交易日历字段不符合请求".into());
                };
                let previous = if truthy(record.get("pretrade_date")) {
                    json!(day(field(record, "pretrade_date")?)?)
                } else {
                    Value::Null
                };
                if previous.as_str().is_some_and(|p| p >= value.as_str()) {
                    return Err("上一交易日必须早于当前日期".into());
                }
                Ok((
                    row(vec![
                        ("exchange", json!(exchange)),
                        ("date", json!(value)),
                        ("is_open", json!(open)),
                        ("previous_trading_day", previous),
                    ]),
                    value,
                ))
            }
            "contracts" => {
                if field(record, "exchange")? != exchange {
                    return Err("返回合约的交易所与请求不一致".into());
                }
                let symbol = string(record, "ts_code")?;
                if !matches(
                    &format!(r"[A-Z]+[0-9]{{3,4}}\.{}", suffix(exchange)),
                    symbol,
                ) {
                    return Err("返回了非实际合约或未知代码".into());
                }
                let name = field(record, "name")?.clone();
                let product = field(record, "fut_code")?.clone();
                let month = delivery_month(field(record, "d_month")?)?;
                let last_delivery = match field(record, "last_ddate")? {
                    value if truthy(Some(value)) => json!(day(value)?),
                    _ => Value::Null,
                };
                let listed = day(field(record, "list_date")?)?;
                let delisted = match record.get("delist_date") {
                    Some(value) if truthy(Some(value)) => json!(day(value)?),
                    _ => Value::Null,
                };
                let per_unit = optional(record.get("per_unit"), false)?;
                let multiplier = optional(record.get("multiplier"), false)?;
                let mut normalized = row(vec![
                    ("exchange", json!(exchange)),
                    ("symbol", json!(symbol)),
                    ("contract", json!(contract_of(symbol))),
                    ("name", name.clone()),
                    ("product", product.clone()),
                    ("currency", json!("CNY")),
                    ("delivery_month", month),
                    ("last_delivery_on", last_delivery),
                    ("listed", json!(listed)),
                    ("delisted", delisted.clone()),
                    (
                        "trade_unit",
                        record.get("trade_unit").cloned().unwrap_or(Value::Null),
                    ),
                    ("per_unit", per_unit),
                    ("multiplier", multiplier),
                    (
                        "quote_unit_desc",
                        record
                            .get("quote_unit_desc")
                            .cloned()
                            .unwrap_or(Value::Null),
                    ),
                    (
                        "quote_unit",
                        record.get("quote_unit").cloned().unwrap_or(Value::Null),
                    ),
                    ("rules_status", json!("INCOMPLETE")),
                ]);
                if name.as_str().is_none_or(str::is_empty)
                    || product.as_str().is_none_or(str::is_empty)
                    || delisted.as_str().is_some_and(|d| listed.as_str() > d)
                {
                    return Err("合约名称、品种或上市区间无效".into());
                }
                let code = Regex::new(r"^([A-Z]+)([0-9]{3,4})\.[A-Z]+$").expect("pattern");
                let captures = code
                    .captures(symbol)
                    .filter(|c| product.as_str() == Some(&c[1]))
                    .ok_or("合约代码与来源品种不一致")?;
                let digits = captures[2].to_string();
                if let Some(month) = normalized.get("delivery_month").and_then(Value::as_str) {
                    let compact = month.replace('-', "");
                    if !compact.ends_with(&digits) {
                        return Err("合约代码与来源完整交割年月不一致".into());
                    }
                }
                let (suggested, note) = contract_multiplier(&normalized)?;
                normalized
                    .0
                    .push(("suggested_multiplier".into(), suggested));
                normalized.0.push(("multiplier_note".into(), json!(note)));
                Ok((normalized, format!("{symbol}\u{0}{listed}")))
            }
            "mapping" => {
                let value = day(field(record, "trade_date")?)?;
                let product = request.symbol.split('.').next().unwrap_or_default();
                let target = field(record, "mapping_ts_code")?;
                let pattern = format!(
                    r"{}[0-9]{{3,4}}\.{}",
                    regex::escape(product),
                    suffix(exchange)
                );
                if field(record, "ts_code")? != request.symbol.as_str()
                    || !matches(&pattern, target.as_str().ok_or(MISSING)?)
                {
                    return Err("主力映射代码或目标合约与请求品种不一致".into());
                }
                Ok((
                    row(vec![
                        ("exchange", json!(exchange)),
                        ("product_id", json!(format!("{exchange}.{product}"))),
                        ("symbol", json!(request.symbol)),
                        ("target_symbol", target.clone()),
                        ("role", json!("main")),
                        ("trading_day", json!(value)),
                        ("available_at", Value::Null),
                    ]),
                    value,
                ))
            }
            "settlement" => {
                let value = day(field(record, "trade_date")?)?;
                if field(record, "ts_code")? != request.symbol.as_str()
                    || field(record, "exchange")? != exchange
                {
                    return Err("结算参数的合约或交易所与请求不一致".into());
                }
                let mut normalized = row(vec![
                    ("symbol", json!(request.symbol)),
                    ("contract", json!(contract_of(&request.symbol))),
                    ("exchange", json!(exchange)),
                    ("trading_day", json!(value)),
                ]);
                for key in fields("settlement") {
                    if matches!(key.as_str(), "ts_code" | "trade_date" | "exchange") {
                        continue;
                    }
                    let parsed = optional(Some(field(record, &key)?), key != "settle")?;
                    normalized.0.push((key, parsed));
                }
                Ok((normalized, value))
            }
            _ => {
                let value = day(field(record, "trade_date")?)?;
                if field(record, "ts_code")? != request.symbol.as_str() {
                    return Err("返回行情的合约与请求不一致".into());
                }
                let mut normalized = row(vec![
                    ("symbol", json!(request.symbol)),
                    ("contract", json!(contract_of(&request.symbol))),
                    ("exchange", json!(exchange)),
                    ("trading_day", json!(value)),
                ]);
                for key in ["open", "high", "low", "close"] {
                    normalized
                        .0
                        .push((key.into(), number(field(record, key)?, false)?));
                }
                for key in ["settle", "pre_settle", "pre_close", "oi_chg"] {
                    normalized
                        .0
                        .push((key.into(), optional(record.get(key), false)?));
                }
                let volume = number(record.get("vol").unwrap_or(&Value::Null), true)?;
                normalized.0.push(("vol".into(), volume));
                for key in ["amount", "oi"] {
                    normalized
                        .0
                        .push((key.into(), optional(record.get(key), true)?));
                }
                let price = |key: &str| {
                    normalized
                        .get(key)
                        .and_then(Value::as_str)
                        .and_then(|t| t.parse::<BigDecimal>().ok())
                        .unwrap_or_default()
                };
                let (open, close) = (price("open"), price("close"));
                if price("low") > open.clone().min(close.clone()) || price("high") < open.max(close)
                {
                    return Err("日线 OHLC 价格边界不一致".into());
                }
                Ok((normalized, value))
            }
        }
    }
}

impl Provider for Tushare {
    fn manifest(&self) -> ProviderManifest {
        ProviderManifest {
            id: "tushare".into(),
            name: "Tushare Pro".into(),
            version: "1.7.0".into(),
            api_version: 2,
            description: "真实历史数据；接口积分与访问权限由 Tushare 账号决定。".into(),
            demo: false,
            configuration: ConfigurationSpec {
                schema_version: 1,
                fields: vec![ConfigurationField {
                    id: "token".into(),
                    label: "Tushare Token".into(),
                    kind: "string".into(),
                    secret: true,
                    required: true,
                    default: Value::Null,
                    description: "仅在本机加密保存；不会回显或写入数据集。".into(),
                    placeholder: "粘贴 Tushare Pro Token".into(),
                    min_length: Some(1),
                    max_length: Some(256),
                    minimum: None,
                    maximum: None,
                }],
            },
            capabilities: capabilities(),
        }
    }

    fn plan(&self, request: &SyncRequest) -> Result<Vec<Partition>> {
        let manifest = self.manifest();
        let supported = manifest
            .capabilities
            .iter()
            .any(|c| c.id == request.dataset && c.exchanges.contains(&request.exchange));
        if !supported {
            return Err("该插件不支持所选数据类型或交易所".into());
        }
        if request.dataset == "minute" {
            return self.minute_plan(request);
        }
        if request.window.is_some() || request.frequency.is_some() {
            return Err("此接口不接受日内时间窗口".into());
        }
        let mut params: BTreeMap<String, String> =
            [("exchange".to_string(), request.exchange.clone())].into();
        if request.dataset == "contracts" {
            if !request.symbol.is_empty() || request.start.is_some() || request.end.is_some() {
                return Err("合约资料按交易所同步，不接受合约或日期过滤".into());
            }
            params.insert("fut_type".into(), "1".into());
            return Ok(vec![Partition {
                api: "fut_basic".into(),
                params,
                fields: fields("contracts"),
                limit: 10000,
                start: None,
                end: None,
            }]);
        }
        let (Some(start), Some(end)) = (request.start, request.end) else {
            return Err("请选择同步日期范围".into());
        };
        let code = suffix(&request.exchange);
        match request.dataset.as_str() {
            "daily" | "settlement" => {
                if !matches(&format!(r"[A-Z]+[0-9]{{3,4}}\.{code}"), &request.symbol) {
                    return Err(
                        "请输入该交易所的实际合约代码，例如 RB2610.SHF；暂不支持连续合约".into(),
                    );
                }
                params.insert("ts_code".into(), request.symbol.clone());
            }
            "mapping" => {
                if !matches(&format!(r"[A-Z]+\.{code}"), &request.symbol) {
                    return Err("请输入主力代码，例如 RB.SHF；不接受实际合约或其他连续代码".into());
                }
                params = [("ts_code".to_string(), request.symbol.clone())].into();
            }
            _ if !request.symbol.is_empty() => return Err("交易日历不接受合约过滤".into()),
            _ => {}
        }
        let api = match request.dataset.as_str() {
            "mapping" => "fut_mapping",
            "daily" => "fut_daily",
            "settlement" => "fut_settle",
            _ => "fut_trade_cal",
        };
        let mut partitions = Vec::new();
        let mut cursor = start;
        while cursor <= end {
            let last = (cursor + Duration::days(30)).min(end);
            let mut part = params.clone();
            part.insert("start_date".into(), cursor.format("%Y%m%d").to_string());
            part.insert("end_date".into(), last.format("%Y%m%d").to_string());
            partitions.push(Partition {
                api: api.into(),
                params: part,
                fields: fields(&request.dataset),
                limit: if request.dataset == "settlement" {
                    1600
                } else {
                    2000
                },
                start: Some(cursor.format("%Y-%m-%d").to_string()),
                end: Some(last.format("%Y-%m-%d").to_string()),
            });
            cursor = last + Duration::days(1);
        }
        Ok(partitions)
    }

    fn probe(&self, configuration: &Map<String, Value>) -> Result<String> {
        let day = NaiveDate::from_ymd_opt(2024, 1, 2);
        let request = SyncRequest {
            command_id: "probe".into(),
            provider: "tushare".into(),
            connection_id: None,
            dataset: "calendar".into(),
            exchange: "SHFE".into(),
            symbol: String::new(),
            frequency: None,
            window: None,
            start: day,
            end: day,
        };
        let rows = self.fetch(&self.plan(&request)?[0], configuration)?;
        if self.normalize(&request, &rows)?.len() != 1 {
            return Err("连接返回空数据，无法确认接口权限".into());
        }
        Ok("交易日历接口验证通过；其他接口权限以实际同步结果为准".into())
    }

    fn fetch(
        &self,
        partition: &Partition,
        configuration: &Map<String, Value>,
    ) -> Result<Vec<Value>> {
        let token = configuration
            .get("token")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if token.is_empty() {
            return Err("请先在设置 → 数据源中保存 Tushare Token".into());
        }
        if token.chars().any(char::is_whitespace) {
            return Err("Token 格式不正确".into());
        }
        self.request(partition, token)
    }

    fn normalize(&self, request: &SyncRequest, rows: &[Value]) -> Result<Vec<Row>> {
        if request.dataset == "minute" {
            return self.minute_rows(request, rows);
        }
        let range = (
            request
                .start
                .map(|d| d.format("%Y-%m-%d").to_string())
                .unwrap_or_else(|| "None".into()),
            request
                .end
                .map(|d| d.format("%Y-%m-%d").to_string())
                .unwrap_or_else(|| "None".into()),
        );
        let mut keys = BTreeSet::new();
        let mut result = Vec::with_capacity(rows.len());
        for record in rows {
            let (normalized, key) = self.normalized(request, record)?;
            if request.dataset != "contracts" && !(range.0 <= key && key <= range.1) {
                return Err("返回日期超出请求范围".into());
            }
            if !keys.insert(key) {
                return Err("返回了重复数据键，未发布".into());
            }
            result.push(normalized);
        }
        let sort_key = |r: &Row| {
            ["trading_day", "date", "symbol"]
                .iter()
                .find_map(|k| r.get(k))
                .and_then(text)
                .unwrap_or_default()
        };
        result.sort_by_key(sort_key);
        Ok(result)
    }
}

#[cfg(test)]
mod tests;
