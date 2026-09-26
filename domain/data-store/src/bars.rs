//! The chart bar table: validated bars stored with exact decimal prices and
//! UTC microsecond times, and the daily projection that charts daily versions.
use crate::pyfmt::{Parts, integral, parts, render};
use crate::table::write_parquet;
use crate::types;
use arrow_array::{
    ArrayRef, Date32Array, Decimal128Array, Int64Array, RecordBatch, StringArray,
    TimestampMicrosecondArray,
};
use arrow_schema::{DataType, Field, Schema, TimeUnit};
use chrono::{DateTime, FixedOffset, NaiveDate};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::sync::Arc;

pub type Result<T> = std::result::Result<T, String>;

const SCALE: i8 = 8;

/// The value scaled to `SCALE` places, for exact decimal storage.
fn scaled(text: &str) -> Result<i128> {
    let parts = parts(text).ok_or("价格不是有效数字")?;
    let shift = parts.exponent + i64::from(SCALE);
    if shift < 0 {
        return Err("价格超过八位小数".into());
    }
    let mut value: i128 = parts.digits.parse().map_err(|_| "价格超出范围")?;
    for _ in 0..shift {
        value = value.checked_mul(10).ok_or("价格超出范围")?;
    }
    Ok(if parts.negative { -value } else { value })
}

fn stamp(value: &Value) -> Result<DateTime<FixedOffset>> {
    let text = value.as_str().ok_or("行情时间无效")?;
    DateTime::parse_from_rfc3339(&text.replacen(' ', "T", 1)).map_err(|_| "行情时间无效".into())
}

fn text<'a>(row: &'a Value, name: &str) -> Result<&'a str> {
    row[name].as_str().ok_or_else(|| format!("{name} 缺失"))
}

/// Encode validated bars as the chart table, sorted by contract and time, with
/// the manifest of the published snapshot.
pub fn encode_bars(rows: &[Value]) -> Result<(Vec<u8>, Value)> {
    if rows.is_empty() {
        return Err("Source returned no rows; no snapshot published".into());
    }
    types::validate("futures.bars", rows).map_err(|error| {
        if error.starts_with("返回了重复数据键") {
            "Duplicate contract/event_time keys".into()
        } else {
            error
        }
    })?;
    let mut bars: Vec<(String, DateTime<FixedOffset>, &Value)> = rows
        .iter()
        .map(|row| {
            Ok((
                text(row, "contract")?.to_string(),
                stamp(&row["event_time"])?,
                row,
            ))
        })
        .collect::<Result<_>>()?;
    bars.sort_by(|a, b| (&a.0, a.1).cmp(&(&b.0, b.1)));
    if bars
        .windows(2)
        .any(|pair| pair[0].0 == pair[1].0 && pair[0].1 == pair[1].1)
    {
        return Err("Duplicate contract/event_time keys".into());
    }
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).expect("epoch");
    let micros = |value: DateTime<FixedOffset>| value.timestamp_micros();
    let mut contracts = Vec::new();
    let mut events = Vec::new();
    let mut availability = Vec::new();
    let mut days = Vec::new();
    let mut prices: [Vec<i128>; 4] = Default::default();
    let mut volumes = Vec::new();
    for (contract, event, row) in &bars {
        contracts.push(contract.clone());
        events.push(micros(*event));
        availability.push(micros(stamp(&row["available_at"])?));
        let day = NaiveDate::parse_from_str(
            text(row, "trading_day")?.get(..10).unwrap_or(""),
            "%Y-%m-%d",
        )
        .map_err(|_| "交易日无效")?;
        days.push((day - epoch).num_days() as i32);
        for (index, name) in ["open", "high", "low", "close"].iter().enumerate() {
            let value = match &row[*name] {
                Value::String(text) => text.clone(),
                other => other.to_string(),
            };
            prices[index].push(scaled(&value)?);
        }
        volumes.push(match &row["volume"] {
            Value::String(text) => integral(text).ok_or("成交量无效")?,
            value => value.as_i64().ok_or("成交量无效")?,
        });
    }
    let utc = || DataType::Timestamp(TimeUnit::Microsecond, Some("UTC".into()));
    let mut fields = vec![
        Field::new("contract", DataType::Utf8, true),
        Field::new("event_time", utc(), true),
        Field::new("available_at", utc(), true),
        Field::new("trading_day", DataType::Date32, true),
    ];
    let mut columns: Vec<ArrayRef> = vec![
        Arc::new(StringArray::from(contracts.clone())),
        Arc::new(TimestampMicrosecondArray::from(events).with_timezone("UTC")),
        Arc::new(TimestampMicrosecondArray::from(availability).with_timezone("UTC")),
        Arc::new(Date32Array::from(days)),
    ];
    for (name, values) in ["open", "high", "low", "close"].iter().zip(prices) {
        fields.push(Field::new(*name, DataType::Decimal128(20, SCALE), true));
        columns.push(Arc::new(
            Decimal128Array::from(values)
                .with_precision_and_scale(20, SCALE)
                .map_err(|_| "价格超出范围")?,
        ));
    }
    fields.push(Field::new("volume", DataType::Int64, true));
    columns.push(Arc::new(Int64Array::from(volumes)));
    let batch = RecordBatch::try_new(Arc::new(Schema::new(fields)), columns)
        .map_err(|_| "行情表无法编码")?;
    let content = write_parquet(&batch)?;
    let first = bars.iter().map(|b| b.1).min().expect("rows");
    let last = bars.iter().map(|b| b.1).max().expect("rows");
    let mut distinct = contracts.clone();
    distinct.sort();
    distinct.dedup();
    let manifest = json!({
        "schema_version": 1,
        "storage": "parquet",
        "demo": contracts.iter().any(|c| c == "SIM.DEMO001"),
        "rows": bars.len(),
        "checksum": hex::encode(Sha256::digest(&content)),
        "contracts": distinct,
        "start": iso(first),
        "end": iso(last),
    });
    Ok((content, manifest))
}

/// Python's `datetime.isoformat()` of an aware time: microseconds when
/// present, numeric offset.
fn iso(value: DateTime<FixedOffset>) -> String {
    let mut text = value.format("%Y-%m-%dT%H:%M:%S").to_string();
    let micros = value.timestamp_subsec_micros();
    if micros != 0 {
        text.push_str(&format!(".{micros:06}"));
    }
    text + &value.format("%:z").to_string()
}

/// Daily rows projected onto the chart table: one bar per trading day at
/// 00:00 UTC, known from the row's observation time or `available_at`.
pub fn daily_chart(rows: &[Value], available_at: &str) -> Result<(Vec<u8>, Value)> {
    let bars: Vec<Value> = rows
        .iter()
        .map(|row| {
            let day = text(row, "trading_day")?;
            let volume = integral(&match &row["vol"] {
                Value::String(text) => text.clone(),
                other => other.to_string(),
            })
            .ok_or("成交量无效")?;
            Ok(json!({
                "contract": row["contract"],
                "event_time": format!("{day}T00:00:00+00:00"),
                "available_at": row.get("_observed_at").cloned().unwrap_or_else(|| json!(available_at)),
                "trading_day": day,
                "open": row["open"], "high": row["high"], "low": row["low"], "close": row["close"],
                "volume": volume,
            }))
        })
        .collect::<Result<_>>()?;
    let (content, mut manifest) = encode_bars(&bars)?;
    manifest["frequency"] = json!("1d");
    manifest["time_semantics"] = json!("trading_day_label");
    Ok((content, manifest))
}

/// Pydantic's JSON form of a UTC time: microseconds when present, then `Z`.
fn utc_text(micros: i64) -> Result<String> {
    let value = DateTime::from_timestamp_micros(micros).ok_or("行情时间超出范围")?;
    let mut text = value.format("%Y-%m-%dT%H:%M:%S").to_string();
    if value.timestamp_subsec_micros() != 0 {
        text.push_str(&format!(".{:06}", value.timestamp_subsec_micros()));
    }
    Ok(text + "Z")
}

/// Up to `limit` bars of a chart table, in the published JSON form: decimal
/// prices as text, times in UTC with `Z`.
pub fn read_bars(content: Vec<u8>, limit: usize) -> Result<Vec<Value>> {
    use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
    let reader = ParquetRecordBatchReaderBuilder::try_new(bytes::Bytes::from(content))
        .and_then(|builder| builder.with_batch_size(limit.max(1)).build())
        .map_err(|_| "图表文件无法解析")?;
    let Some(batch) = reader.into_iter().next() else {
        return Ok(Vec::new());
    };
    let batch = batch.map_err(|_| "图表文件无法解析")?;
    let column = |name: &str| batch.column_by_name(name).ok_or("图表文件缺少字段");
    macro_rules! typed {
        ($name:expr, $type:ty) => {
            column($name)?
                .as_any()
                .downcast_ref::<$type>()
                .ok_or("图表文件字段类型无效")?
        };
    }
    let contracts = typed!("contract", StringArray);
    let events = typed!("event_time", TimestampMicrosecondArray);
    let availability = typed!("available_at", TimestampMicrosecondArray);
    let days = typed!("trading_day", Date32Array);
    let prices = ["open", "high", "low", "close"].map(|name| {
        column(name).and_then(|c| {
            c.as_any()
                .downcast_ref::<Decimal128Array>()
                .ok_or("图表文件字段类型无效")
        })
    });
    let volumes = typed!("volume", Int64Array);
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).expect("epoch");
    (0..batch.num_rows().min(limit))
        .map(|index| {
            let mut row = serde_json::Map::new();
            row.insert("contract".into(), json!(contracts.value(index)));
            row.insert("event_time".into(), json!(utc_text(events.value(index))?));
            row.insert(
                "available_at".into(),
                json!(utc_text(availability.value(index))?),
            );
            let day = epoch + chrono::Duration::days(i64::from(days.value(index)));
            row.insert(
                "trading_day".into(),
                json!(day.format("%Y-%m-%d").to_string()),
            );
            for (name, array) in ["open", "high", "low", "close"].iter().zip(&prices) {
                let array = array.as_ref().map_err(|e| e.to_string())?;
                let parts = Parts {
                    negative: array.value(index) < 0,
                    digits: array.value(index).unsigned_abs().to_string(),
                    exponent: -i64::from(array.scale()),
                };
                row.insert((*name).into(), json!(render(&parts)));
            }
            row.insert("volume".into(), json!(volumes.value(index)));
            Ok(Value::Object(row))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pyfmt::decimal_text;

    #[test]
    fn decimal_text_matches_python_decimal_str() {
        for (input, expected) in [
            ("10.50", "10.50"),
            ("1e2", "1E+2"),
            (" 7 ", "7"),
            ("+1", "1"),
            ("-0", "-0"),
            ("1_000", "1000"),
            ("0.000000001", "1E-9"),
            ("0.00", "0.00"),
            ("000.10", "0.10"),
            ("0.000001", "0.000001"),
            ("0.0000001", "1E-7"),
            ("123", "123"),
            ("1.5E3", "1.5E+3"),
        ] {
            assert_eq!(decimal_text(input).unwrap(), expected, "{input}");
        }
        assert!(decimal_text("x").is_none());
    }

    fn bar(contract: &str, event: &str, open: &str) -> Value {
        json!({"contract": contract, "event_time": event, "available_at": event,
               "trading_day": "2026-01-05", "open": open, "high": "12", "low": "9", "close": "11",
               "volume": 5})
    }

    #[test]
    fn bars_are_sorted_exact_and_read_in_published_form() {
        let rows = vec![
            bar("SHFE.rb2610", "2026-01-05T09:02:00+08:00", "10.5"),
            bar("SHFE.rb2610", "2026-01-05T09:01:00+08:00", "10.12345678"),
        ];
        let (content, manifest) = encode_bars(&rows).unwrap();
        assert_eq!(manifest["start"], "2026-01-05T09:01:00+08:00");
        assert_eq!(manifest["contracts"], json!(["SHFE.rb2610"]));
        assert_eq!(manifest["checksum"].as_str().unwrap().len(), 64);
        let read = read_bars(content, 10).unwrap();
        assert_eq!(read[0]["event_time"], "2026-01-05T01:01:00Z");
        assert_eq!(read[0]["open"], "10.12345678");
        assert_eq!(read[1]["open"], "10.50000000");
        assert_eq!(read[0]["volume"], 5);
        assert_eq!(
            encode_bars(&[rows[0].clone(), rows[0].clone()]).unwrap_err(),
            "Duplicate contract/event_time keys"
        );
        assert!(encode_bars(&[]).is_err());
        for (field, value) in [
            ("contract", json!("RB.CONT")),
            ("high", json!("9.5")),
            ("available_at", json!("2026-01-05T09:00:00+08:00")),
            ("open", json!("NaN")),
            ("event_time", json!("2026-01-05T09:01:00.123+08:00")),
        ] {
            let mut row = rows[0].clone();
            row[field] = value;
            assert!(encode_bars(&[row]).is_err(), "{field}");
        }
        let daily = json!({"contract": "SHFE.rb2610", "trading_day": "2026-01-05", "open": "10",
                           "high": "12", "low": "9", "close": "11", "vol": "100.0"});
        let (chart, manifest) = daily_chart(&[daily], "2026-01-06T00:00:00+00:00").unwrap();
        assert_eq!(
            (
                manifest["frequency"].as_str(),
                manifest["time_semantics"].as_str()
            ),
            (Some("1d"), Some("trading_day_label"))
        );
        assert_eq!(read_bars(chart, 1).unwrap()[0]["volume"], 100);
    }
}
