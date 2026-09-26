use crate::Result;
use crate::merge::{CoverageGap, MergeRequest, ParentVersion, cumulative_series};
use crate::partitions::{Cell, Partition, PartitionedVersion, check_partitions};
use chrono::{Datelike, NaiveDate};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeSet;

#[derive(Debug, Clone, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScanRequest {
    #[schemars(length(min = 1, max = 256))]
    pub version_id: String,
    #[schemars(length(min = 1, max = 1000))]
    pub contract_ids: Vec<String>,
    pub start: NaiveDate,
    pub end: NaiveDate,
    #[schemars(length(min = 1, max = 30))]
    pub columns: Vec<String>,
    #[serde(default = "default_batch_rows")]
    #[schemars(range(min = 1, max = 4096))]
    pub batch_rows: usize,
}
fn default_batch_rows() -> usize {
    1024
}
impl ScanRequest {
    pub fn validate(&self) -> Result<()> {
        if self.version_id.is_empty() || self.version_id.len() > 256 {
            return Err("扫描版本标识长度不合法".into());
        }
        if self.start > self.end || self.start.year() < 1 || self.end.year() > 9999 {
            return Err("扫描日期范围倒置或超出有效范围".into());
        }
        if !(1..=4096).contains(&self.batch_rows)
            || !(1..=1000).contains(&self.contract_ids.len())
            || !(1..=30).contains(&self.columns.len())
        {
            return Err("扫描请求超出数量限制".into());
        }
        for items in [&self.contract_ids, &self.columns] {
            if items.iter().any(|v| v.is_empty() || v.len() > 256)
                || items.iter().collect::<BTreeSet<_>>().len() != items.len()
            {
                return Err("扫描合约或字段重复、为空或过长".into());
            }
        }
        if self.columns.iter().any(|v| v.starts_with('_')) {
            return Err("扫描不允许将内部来源字段作为业务列".into());
        }
        Ok(())
    }
}

pub fn schema() -> Value {
    #[derive(JsonSchema)]
    #[allow(dead_code)]
    #[serde(untagged)]
    enum Models {
        ScanRequest(ScanRequest),
        Cell(Cell),
        Partition(Partition),
        PartitionedVersion(PartitionedVersion),
        CoverageGap(CoverageGap),
        ParentVersion(ParentVersion),
        MergeRequest(MergeRequest),
    }
    let mut schema =
        serde_json::to_value(schemars::schema_for!(Models)).expect("schema is serializable");
    // Every nullable field of these models is mandatory: requiring all keys keeps
    // the null branch, where schemars(required) would narrow it to non-null.
    for name in [
        "Partition",
        "PartitionedVersion",
        "CoverageGap",
        "ParentVersion",
        "MergeRequest",
    ] {
        let object = schema["$defs"][name]
            .as_object_mut()
            .expect("model definition");
        let fields: Vec<Value> = object["properties"]
            .as_object()
            .expect("model properties")
            .keys()
            .cloned()
            .map(Value::String)
            .collect();
        object.insert("required".into(), Value::Array(fields));
    }
    schema
}
pub fn invoke(operation: &str, input: Value) -> Result<Value> {
    match operation {
        "schema" => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Empty {}
            serde_json::from_value::<Empty>(input).map_err(|e| e.to_string())?;
            Ok(schema())
        }
        "validate" => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request {
                model: String,
                value: Value,
            }
            let r: Request = serde_json::from_value(input).map_err(|e| e.to_string())?;
            fn parse<T: serde::de::DeserializeOwned>(value: Value) -> Result<T> {
                serde_json::from_value(value).map_err(|e| e.to_string())
            }
            match r.model.as_str() {
                "ScanRequest" => parse::<ScanRequest>(r.value.clone())?.validate()?,
                "Partition" => parse::<Partition>(r.value.clone())?.validate()?,
                "PartitionedVersion" => {
                    check_partitions(&parse::<PartitionedVersion>(r.value.clone())?.partitions)?;
                }
                "CoverageGap" => {
                    let gap = parse::<CoverageGap>(r.value.clone())?;
                    if gap.start > gap.end {
                        return Err("覆盖缺口起止倒置".into());
                    }
                }
                "ParentVersion" => {
                    check_partitions(&parse::<ParentVersion>(r.value.clone())?.index.partitions)?;
                }
                "MergeRequest" => parse::<MergeRequest>(r.value.clone())?.validate()?,
                _ => return Err("unsupported data-store model".into()),
            }
            Ok(r.value)
        }
        "cumulative_series" => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request {
                type_id: String,
            }
            let r: Request = serde_json::from_value(input).map_err(|e| e.to_string())?;
            Ok(cumulative_series(&r.type_id).map_or(Value::Null, Value::from))
        }
        _ => Err("unsupported data-store operation".into()),
    }
}
