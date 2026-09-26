//! Standard tables as ordered records: reading stored tables into rows whose
//! columns keep their stored order (dates and times in ISO form, decimals as
//! numbers), and encoding record lists as Parquet tables.
use arrow_array::{
    Array, ArrayRef, BooleanArray, Date32Array, Decimal128Array, Float32Array, Float64Array,
    Int8Array, Int16Array, Int32Array, Int64Array, LargeListArray, LargeStringArray, ListArray,
    RecordBatch, StringArray, StringViewArray, StructArray, TimestampMicrosecondArray,
    TimestampMillisecondArray, TimestampNanosecondArray, TimestampSecondArray, UInt8Array,
    UInt16Array, UInt32Array, UInt64Array,
};
use arrow_schema::{DataType, TimeUnit};
use chrono::{DateTime, FixedOffset, NaiveDate, NaiveDateTime, Offset, TimeZone, Timelike};
use serde::de::{Deserializer, MapAccess, Visitor};
use serde::ser::{SerializeMap, Serializer};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};

/// One record whose fields keep their stored order.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Row(pub Vec<(String, Value)>);

impl Row {
    pub fn get(&self, name: &str) -> Option<&Value> {
        self.0
            .iter()
            .find(|(key, _)| key == name)
            .map(|(_, value)| value)
    }
}

impl Serialize for Row {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.0.len()))?;
        for (key, value) in &self.0 {
            map.serialize_entry(key, value)?;
        }
        map.end()
    }
}

impl<'de> Deserialize<'de> for Row {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        struct Fields;
        impl<'de> Visitor<'de> for Fields {
            type Value = Row;
            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                f.write_str("a record")
            }
            fn visit_map<A: MapAccess<'de>>(
                self,
                mut access: A,
            ) -> std::result::Result<Row, A::Error> {
                let mut fields = Vec::new();
                while let Some(entry) = access.next_entry::<String, Value>()? {
                    fields.push(entry);
                }
                Ok(Row(fields))
            }
        }
        deserializer.deserialize_map(Fields)
    }
}

type Result<T> = std::result::Result<T, String>;

fn unsupported() -> String {
    "数据列类型尚不支持预览".into()
}

fn number(value: f64) -> Result<Value> {
    serde_json::Number::from_f64(value)
        .map(Value::Number)
        .ok_or_else(|| "数据包含非有限数值，不能预览".into())
}

/// Python's `datetime.isoformat()`: microseconds only when present.
fn iso(stamp: NaiveDateTime, offset: Option<FixedOffset>) -> String {
    let mut text = stamp.format("%Y-%m-%dT%H:%M:%S").to_string();
    let micros = stamp.nanosecond() / 1000;
    if micros != 0 {
        text.push_str(&format!(".{micros:06}"));
    }
    if let Some(offset) = offset {
        let seconds = offset.local_minus_utc();
        let minutes = seconds.unsigned_abs() / 60;
        text.push_str(&format!(
            "{}{:02}:{:02}",
            if seconds < 0 { '-' } else { '+' },
            minutes / 60,
            minutes % 60
        ));
    }
    text
}

fn timestamp(value: i64, unit: &TimeUnit, zone: Option<&str>) -> Result<Value> {
    let (seconds, nanos) = match unit {
        TimeUnit::Second => (value, 0),
        TimeUnit::Millisecond => (value.div_euclid(1000), value.rem_euclid(1000) * 1_000_000),
        TimeUnit::Microsecond => (
            value.div_euclid(1_000_000),
            value.rem_euclid(1_000_000) * 1000,
        ),
        TimeUnit::Nanosecond => (
            value.div_euclid(1_000_000_000),
            value.rem_euclid(1_000_000_000),
        ),
    };
    let utc = DateTime::from_timestamp(seconds, nanos as u32).ok_or("数据时间超出范围")?;
    let Some(zone) = zone else {
        return Ok(json!(iso(utc.naive_utc(), None)));
    };
    let offset = if let Ok(fixed) = zone.parse::<FixedOffset>() {
        fixed
    } else {
        let tz: chrono_tz::Tz = zone.parse().map_err(|_| "数据时区不受支持")?;
        tz.offset_from_utc_datetime(&utc.naive_utc()).fix()
    };
    Ok(json!(iso(
        utc.with_timezone(&offset).naive_local(),
        Some(offset)
    )))
}

fn decimal(value: i128, scale: i8) -> Result<Value> {
    if scale <= 0 {
        let whole = value
            .checked_mul(10i128.pow(u32::from(scale.unsigned_abs())))
            .ok_or("数值超出范围")?;
        return i64::try_from(whole)
            .map(|v| json!(v))
            .map_err(|_| "数值超出范围".into());
    }
    number(value as f64 / 10f64.powi(i32::from(scale)))
}

macro_rules! at {
    ($array:expr, $type:ty, $index:expr) => {
        $array
            .as_any()
            .downcast_ref::<$type>()
            .ok_or_else(unsupported)?
            .value($index)
    };
}

fn cell(array: &ArrayRef, index: usize) -> Result<Value> {
    if array.is_null(index) {
        return Ok(Value::Null);
    }
    Ok(match array.data_type() {
        DataType::Null => Value::Null,
        DataType::Boolean => json!(at!(array, BooleanArray, index)),
        DataType::Int8 => json!(at!(array, Int8Array, index)),
        DataType::Int16 => json!(at!(array, Int16Array, index)),
        DataType::Int32 => json!(at!(array, Int32Array, index)),
        DataType::Int64 => json!(at!(array, Int64Array, index)),
        DataType::UInt8 => json!(at!(array, UInt8Array, index)),
        DataType::UInt16 => json!(at!(array, UInt16Array, index)),
        DataType::UInt32 => json!(at!(array, UInt32Array, index)),
        DataType::UInt64 => json!(at!(array, UInt64Array, index)),
        DataType::Float32 => number(f64::from(at!(array, Float32Array, index)))?,
        DataType::Float64 => number(at!(array, Float64Array, index))?,
        DataType::Utf8 => json!(at!(array, StringArray, index)),
        DataType::LargeUtf8 => json!(at!(array, LargeStringArray, index)),
        DataType::Utf8View => json!(at!(array, StringViewArray, index)),
        DataType::Date32 => {
            let days = at!(array, Date32Array, index);
            let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).expect("epoch");
            let date = epoch
                .checked_add_signed(chrono::Duration::days(i64::from(days)))
                .ok_or("数据日期超出范围")?;
            json!(date.format("%Y-%m-%d").to_string())
        }
        DataType::Timestamp(unit, zone) => {
            let value = match unit {
                TimeUnit::Second => at!(array, TimestampSecondArray, index),
                TimeUnit::Millisecond => at!(array, TimestampMillisecondArray, index),
                TimeUnit::Microsecond => at!(array, TimestampMicrosecondArray, index),
                TimeUnit::Nanosecond => at!(array, TimestampNanosecondArray, index),
            };
            timestamp(value, unit, zone.as_deref())?
        }
        DataType::Decimal128(_, scale) => decimal(at!(array, Decimal128Array, index), *scale)?,
        DataType::List(_) => {
            let values = array
                .as_any()
                .downcast_ref::<ListArray>()
                .ok_or_else(unsupported)?
                .value(index);
            Value::Array(
                (0..values.len())
                    .map(|i| cell(&values, i))
                    .collect::<Result<_>>()?,
            )
        }
        DataType::LargeList(_) => {
            let values = array
                .as_any()
                .downcast_ref::<LargeListArray>()
                .ok_or_else(unsupported)?
                .value(index);
            Value::Array(
                (0..values.len())
                    .map(|i| cell(&values, i))
                    .collect::<Result<_>>()?,
            )
        }
        DataType::Struct(fields) => {
            let values = array
                .as_any()
                .downcast_ref::<StructArray>()
                .ok_or_else(unsupported)?;
            let mut object = Map::new();
            for (field, column) in fields.iter().zip(values.columns()) {
                object.insert(field.name().clone(), cell(column, index)?);
            }
            Value::Object(object)
        }
        _ => return Err(unsupported()),
    })
}

/// Rows `offset..offset + limit` of a batch, in stored column order.
pub fn batch_rows(batch: &RecordBatch, offset: usize, limit: usize) -> Result<Vec<Row>> {
    let schema = batch.schema();
    let end = batch.num_rows().min(offset.saturating_add(limit));
    (offset.min(end)..end)
        .map(|index| {
            schema
                .fields()
                .iter()
                .zip(batch.columns())
                .map(|(field, column)| Ok((field.name().clone(), cell(column, index)?)))
                .collect::<Result<Vec<_>>>()
                .map(Row)
        })
        .collect()
}

/// Records of a CSV text with a header row (RFC 4180 quoting). Short records
/// have null for missing fields; records with extra fields are refused.
pub fn csv_rows(text: &str) -> Result<Vec<Row>> {
    let mut records: Vec<Vec<String>> = Vec::new();
    let mut record = Vec::new();
    let mut field = String::new();
    let (mut quoted, mut started) = (false, false);
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if quoted {
            match c {
                '"' if chars.peek() == Some(&'"') => {
                    chars.next();
                    field.push('"');
                }
                '"' => quoted = false,
                _ => field.push(c),
            }
            continue;
        }
        match c {
            '"' if field.is_empty() => {
                quoted = true;
                started = true;
            }
            ',' => {
                record.push(std::mem::take(&mut field));
                started = true;
            }
            '\r' if chars.peek() == Some(&'\n') => {}
            '\n' | '\r' => {
                if started || !field.is_empty() || !record.is_empty() {
                    record.push(std::mem::take(&mut field));
                    records.push(std::mem::take(&mut record));
                }
                started = false;
            }
            _ => {
                field.push(c);
                started = true;
            }
        }
    }
    if quoted {
        return Err("CSV 引号未闭合".into());
    }
    if started || !field.is_empty() || !record.is_empty() {
        record.push(field);
        records.push(record);
    }
    let mut records = records.into_iter();
    let Some(header) = records.next() else {
        return Ok(Vec::new());
    };
    records
        .map(|values| {
            if values.len() > header.len() {
                return Err("CSV 记录字段多于表头".into());
            }
            Ok(Row(header
                .iter()
                .enumerate()
                .map(|(i, name)| {
                    (
                        name.clone(),
                        values.get(i).map_or(Value::Null, |v| json!(v)),
                    )
                })
                .collect()))
        })
        .collect()
}

/// Parquet column type inferred from a column's values, as the tables were
/// always written: text, integer, float, boolean, or all-null.
fn column_type(values: &[&Value]) -> Result<DataType> {
    let mut kind = DataType::Null;
    for value in values {
        let found = match value {
            Value::Null => continue,
            Value::String(_) => DataType::Utf8,
            Value::Bool(_) => DataType::Boolean,
            Value::Number(number) if number.is_i64() || number.is_u64() => DataType::Int64,
            Value::Number(_) => DataType::Float64,
            _ => return Err("表格字段只能是文本、数值、布尔或空值".into()),
        };
        kind = match (&kind, &found) {
            (DataType::Null, _) => found,
            (a, b) if a == b => kind,
            (DataType::Int64, DataType::Float64) | (DataType::Float64, DataType::Int64) => {
                DataType::Float64
            }
            _ => return Err("同一字段的数值类型不一致".into()),
        };
    }
    Ok(kind)
}

/// Encode records as one Parquet table. Columns appear in first-seen order;
/// a record without a column holds null there.
pub fn encode_table(rows: &[Row]) -> Result<Vec<u8>> {
    use arrow_array::builder::{BooleanBuilder, Float64Builder, Int64Builder, StringBuilder};
    use arrow_array::{NullArray, RecordBatch};
    use arrow_schema::{Field, Schema};
    use std::sync::Arc;
    if rows.is_empty() {
        return Err("表格为空".into());
    }
    let mut names: Vec<&str> = Vec::new();
    for row in rows {
        for (name, _) in &row.0 {
            if !names.contains(&name.as_str()) {
                names.push(name);
            }
        }
    }
    let mut fields = Vec::with_capacity(names.len());
    let mut columns: Vec<ArrayRef> = Vec::with_capacity(names.len());
    for name in names {
        let values: Vec<&Value> = rows
            .iter()
            .map(|row| row.get(name).unwrap_or(&Value::Null))
            .collect();
        let kind = column_type(&values)?;
        let array: ArrayRef = match kind {
            DataType::Null => Arc::new(NullArray::new(values.len())),
            DataType::Utf8 => {
                let mut builder = StringBuilder::new();
                values
                    .iter()
                    .for_each(|v| builder.append_option(v.as_str()));
                Arc::new(builder.finish())
            }
            DataType::Int64 => {
                let mut builder = Int64Builder::new();
                values
                    .iter()
                    .for_each(|v| builder.append_option(v.as_i64()));
                Arc::new(builder.finish())
            }
            DataType::Float64 => {
                let mut builder = Float64Builder::new();
                values
                    .iter()
                    .for_each(|v| builder.append_option(v.as_f64()));
                Arc::new(builder.finish())
            }
            _ => {
                let mut builder = BooleanBuilder::new();
                values
                    .iter()
                    .for_each(|v| builder.append_option(v.as_bool()));
                Arc::new(builder.finish())
            }
        };
        fields.push(Field::new(name, kind, true));
        columns.push(array);
    }
    let batch = RecordBatch::try_new(Arc::new(Schema::new(fields)), columns)
        .map_err(|_| "表格无法编码".to_string())?;
    write_parquet(&batch)
}

/// Parquet bytes of one batch, Snappy-compressed.
pub fn write_parquet(batch: &RecordBatch) -> Result<Vec<u8>> {
    let properties = parquet::file::properties::WriterProperties::builder()
        .set_compression(parquet::basic::Compression::SNAPPY)
        .build();
    let mut content = Vec::new();
    let mut writer =
        parquet::arrow::ArrowWriter::try_new(&mut content, batch.schema(), Some(properties))
            .map_err(|_| "表格无法编码".to_string())?;
    writer
        .write(batch)
        .map_err(|_| "表格无法编码".to_string())?;
    writer.close().map_err(|_| "表格无法编码".to_string())?;
    Ok(content)
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::{Decimal128Array, builder::StringBuilder};
    use arrow_schema::{Field, Schema};
    use std::sync::Arc;

    #[test]
    fn rows_keep_column_order_and_published_value_forms() {
        let mut names = StringBuilder::new();
        names.append_value("rb");
        names.append_null();
        let schema = Schema::new(vec![
            Field::new("z", DataType::Utf8, true),
            Field::new(
                "a",
                DataType::Timestamp(TimeUnit::Microsecond, Some("UTC".into())),
                false,
            ),
            Field::new("d", DataType::Date32, false),
            Field::new("p", DataType::Decimal128(20, 8), false),
        ]);
        let batch = RecordBatch::try_new(
            Arc::new(schema),
            vec![
                Arc::new(names.finish()),
                Arc::new(
                    TimestampMicrosecondArray::from(vec![1_704_153_600_500_000, 0])
                        .with_timezone("UTC"),
                ),
                Arc::new(Date32Array::from(vec![19724, 0])),
                Arc::new(
                    Decimal128Array::from(vec![1_050_000_000i128, 100_000_000])
                        .with_precision_and_scale(20, 8)
                        .unwrap(),
                ),
            ],
        )
        .unwrap();
        let rows = batch_rows(&batch, 0, 10).unwrap();
        assert_eq!(
            serde_json::to_string(&rows[0]).unwrap(),
            r#"{"z":"rb","a":"2024-01-02T00:00:00.500000+00:00","d":"2024-01-02","p":10.5}"#
        );
        assert_eq!(rows[1].get("z"), Some(&Value::Null));
        assert_eq!(rows[1].get("a"), Some(&json!("1970-01-01T00:00:00+00:00")));
        assert_eq!(batch_rows(&batch, 1, 10).unwrap().len(), 1);
    }

    #[test]
    fn csv_follows_the_header_with_quoting() {
        let rows = csv_rows("b,a\r\n\"x,1\",\"say \"\"hi\"\"\"\n2\n").unwrap();
        assert_eq!(
            serde_json::to_string(&rows[0]).unwrap(),
            r#"{"b":"x,1","a":"say \"hi\""}"#
        );
        assert_eq!(rows[1].get("a"), Some(&Value::Null));
        assert!(csv_rows("a\n1,2\n").is_err());
        assert!(csv_rows("a\n\"1\n").is_err());
        let ordered: Row = serde_json::from_str(r#"{"z":1,"a":2}"#).unwrap();
        assert_eq!(ordered.0[0].0, "z");
    }

    #[test]
    fn tables_round_trip_with_inferred_column_types() {
        let rows = vec![
            Row(vec![
                ("b".into(), json!("x")),
                ("a".into(), json!(1)),
                ("n".into(), Value::Null),
            ]),
            Row(vec![
                ("b".into(), Value::Null),
                ("a".into(), json!(2.5)),
                ("c".into(), json!(true)),
            ]),
        ];
        let content = encode_table(&rows).unwrap();
        let reader = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(
            bytes::Bytes::from(content),
        )
        .unwrap()
        .build()
        .unwrap();
        let batch = reader.into_iter().next().unwrap().unwrap();
        let back = batch_rows(&batch, 0, 10).unwrap();
        assert_eq!(
            serde_json::to_string(&back).unwrap(),
            r#"[{"b":"x","a":1.0,"n":null,"c":null},{"b":null,"a":2.5,"n":null,"c":true}]"#
        );
        assert!(
            encode_table(&[
                Row(vec![("a".into(), json!("x"))]),
                Row(vec![("a".into(), json!(1))])
            ])
            .is_err()
        );
        assert!(encode_table(&[]).is_err());
    }
}
