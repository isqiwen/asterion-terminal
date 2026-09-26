//! Copy-on-write cumulative publication. The latest observation of a primary
//! key wins; equal observation times must agree; unreturned rows are never
//! deleted. Untouched partitions are reused by reference, never rewritten.
use crate::Result;
use crate::partitions::{
    Cell, MAX_COLUMNS, MAX_VALUE_BYTES, OBSERVED, Partition, PartitionedVersion, RAW_VERSION,
    cells, check_partitions, encode, encode_index, read_index, read_partition, required_option,
    store_partition,
};
use asterion_kernel::artifacts::{ArtifactRef, ArtifactStore};
use chrono::{DateTime, Days, FixedOffset, NaiveDate};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};

const MAX_INCOMING_ROWS: usize = 1_000_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PartitionBy {
    Month,
    TradingDay,
}
struct Layout {
    series: &'static str,
    partition_by: PartitionBy,
    calendar: bool,
}
fn layout(type_id: &str) -> Option<Layout> {
    Some(match type_id {
        "futures.daily" => Layout {
            series: "monthly-observed-v1",
            partition_by: PartitionBy::Month,
            calendar: false,
        },
        "futures.calendar" => Layout {
            series: "monthly-observed-v1",
            partition_by: PartitionBy::Month,
            calendar: true,
        },
        "futures.minute" => Layout {
            series: "trading-day-minute-v1",
            partition_by: PartitionBy::TradingDay,
            calendar: false,
        },
        _ => return None,
    })
}

/// The cumulative version series of a data type, or None when unsupported.
pub fn cumulative_series(type_id: &str) -> Option<&'static str> {
    layout(type_id).map(|layout| layout.series)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CoverageGap {
    pub start: NaiveDate,
    pub end: NaiveDate,
}

#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ParentVersion {
    #[schemars(length(min = 1, max = 256))]
    pub id: String,
    #[schemars(range(min = 1))]
    pub revision: i64,
    pub observed_at: String,
    #[serde(deserialize_with = "required_option")]
    pub coverage_gaps: Option<Vec<CoverageGap>>,
    pub first: String,
    pub last: String,
    pub index: PartitionedVersion,
}

#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct MergeRequest {
    #[schemars(length(min = 1, max = 128))]
    pub job_id: String,
    pub type_id: String,
    pub time_field: String,
    #[schemars(length(min = 1, max = 16))]
    pub primary_key: Vec<String>,
    #[schemars(length(min = 1, max = 256))]
    pub raw_version_id: String,
    #[serde(deserialize_with = "required_option")]
    pub parent: Option<ParentVersion>,
    #[schemars(length(min = 1, max = 126))]
    pub columns: Vec<String>,
    #[schemars(length(min = 1, max = 1000000))]
    pub rows: Vec<Vec<Cell>>,
    pub observed_at: Vec<String>,
}

fn instant(value: &str) -> Result<DateTime<FixedOffset>> {
    DateTime::parse_from_rfc3339(value).map_err(|_| "来源观测时间无效".into())
}
fn day(value: &str) -> Result<NaiveDate> {
    NaiveDate::parse_from_str(value, "%Y-%m-%d").map_err(|_| "日历日期无效".into())
}

impl MergeRequest {
    pub fn validate(&self) -> Result<()> {
        let segment = &self.job_id;
        if segment.is_empty()
            || segment.len() > 128
            || matches!(segment.as_str(), "." | "..")
            || !segment
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_' | b'.'))
        {
            return Err("累积发布任务标识不合法".into());
        }
        let layout = layout(&self.type_id).ok_or("该类型尚不支持累积发布")?;
        let columns: BTreeSet<_> = self.columns.iter().collect();
        if self.columns.is_empty()
            || self.columns.len() > MAX_COLUMNS - 2
            || columns.len() != self.columns.len()
            || self
                .columns
                .iter()
                .any(|c| c.is_empty() || c.len() > 256 || c.starts_with('_'))
        {
            return Err("累积发布字段为空、重复或使用内部名称".into());
        }
        let keys: BTreeSet<_> = self.primary_key.iter().collect();
        if !columns.contains(&self.time_field)
            || self.primary_key.is_empty()
            || self.primary_key.len() > 16
            || keys.len() != self.primary_key.len()
            || !keys.iter().all(|k| columns.contains(k))
            || (layout.partition_by == PartitionBy::TradingDay
                && !columns.contains(&"trading_day".to_string()))
        {
            return Err("累积发布时间字段或主键未声明".into());
        }
        if self.raw_version_id.is_empty() || self.raw_version_id.len() > 256 {
            return Err("原始版本标识不合法".into());
        }
        if self.rows.is_empty()
            || self.rows.len() > MAX_INCOMING_ROWS
            || self.observed_at.len() != self.rows.len()
        {
            return Err("累积发布记录为空、超出上限或缺少观测时间".into());
        }
        for row in &self.rows {
            if row.len() != self.columns.len()
                || row
                    .iter()
                    .any(|c| c.text().is_some_and(|t| t.len() > MAX_VALUE_BYTES))
            {
                return Err("累积发布记录字段数量不一致或值过长".into());
            }
        }
        for value in &self.observed_at {
            instant(value)?;
        }
        if let Some(parent) = &self.parent {
            if parent.id.is_empty() || parent.id.len() > 256 || parent.revision < 1 {
                return Err("父版本标识或修订号不合法".into());
            }
            instant(&parent.observed_at)?;
            check_partitions(&parent.index.partitions)?;
            if layout.calendar && parent.coverage_gaps.is_none() {
                return Err("父版本缺少日历缺口依据".into());
            }
        }
        Ok(())
    }
}

#[derive(Serialize)]
pub struct MergeOutcome {
    pub series: &'static str,
    pub rows: i64,
    pub index: ArtifactRef,
    pub detail: Value,
    /// Every row of each partition this acquisition touched, with provenance.
    pub touched: Vec<BTreeMap<String, Cell>>,
    /// Actual work, not persisted: existing partitions decoded for this merge.
    pub metrics: MergeMetrics,
}

#[derive(Default, Serialize)]
pub struct MergeMetrics {
    pub partitions_read: Vec<String>,
    pub bytes_read: u64,
}

#[derive(Default, Serialize)]
struct Changes {
    added: u64,
    revised: u64,
    refreshed: u64,
    unchanged: u64,
    stale_ignored: u64,
}

fn text<'a>(cell: &'a Cell, message: &str) -> Result<&'a str> {
    cell.text().ok_or_else(|| message.into())
}

/// Extend fixed coverage and subtract observed dates; recollection never deletes.
fn calendar_gaps(
    parent: Option<&ParentVersion>,
    dates: &[NaiveDate],
    first: NaiveDate,
    last: NaiveDate,
) -> Result<Vec<CoverageGap>> {
    let mut intervals = Vec::new();
    if let Some(parent) = parent {
        intervals.extend(
            parent
                .coverage_gaps
                .iter()
                .flatten()
                .map(|g| (g.start, g.end)),
        );
        let (old_first, old_last) = (day(&parent.first)?, day(&parent.last)?);
        if first < old_first {
            intervals.push((first, old_first - Days::new(1)));
        }
        if last > old_last {
            intervals.push((old_last + Days::new(1), last));
        }
    } else {
        intervals.push((first, last));
    }
    intervals.sort();
    let days: BTreeSet<_> = dates.iter().copied().collect();
    let mut result = Vec::new();
    for (left, right) in intervals {
        let mut cursor = left;
        for &observed in days.range(left..=right) {
            if observed < cursor {
                continue;
            }
            if cursor < observed {
                result.push(CoverageGap {
                    start: cursor,
                    end: observed - Days::new(1),
                });
            }
            cursor = observed + Days::new(1);
        }
        if cursor <= right {
            result.push(CoverageGap {
                start: cursor,
                end: right,
            });
        }
    }
    Ok(result)
}

pub fn merge(store: &ArtifactStore, request: &MergeRequest) -> Result<MergeOutcome> {
    request.validate()?;
    let layout = layout(&request.type_id).expect("validated layout");
    let position = |name: &str| {
        request
            .columns
            .iter()
            .position(|c| c == name)
            .expect("validated column")
    };
    let time = position(&request.time_field);
    let key_columns: Vec<usize> = request.primary_key.iter().map(|k| position(k)).collect();
    let width = request.columns.len();
    let mut output = request.columns.clone();
    output.extend([OBSERVED.to_string(), RAW_VERSION.to_string()]);
    let expected: BTreeSet<&String> = output.iter().collect();

    let parent = request.parent.as_ref();
    let previous: BTreeMap<String, Partition> = match parent {
        Some(parent) => read_index(store, &parent.index)
            .map_err(|e| format!("父版本{e}"))?
            .into_iter()
            .map(|p| (p.key.clone(), p))
            .collect(),
        None => BTreeMap::new(),
    };
    let observed: Vec<_> = request
        .observed_at
        .iter()
        .map(|v| instant(v))
        .collect::<Result<_>>()?;
    let mut incoming: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    for (index, row) in request.rows.iter().enumerate() {
        let value = text(&row[time], "时间字段必须为文本")?;
        let key = match layout.partition_by {
            PartitionBy::Month => value
                .get(..7)
                .filter(|k| k.is_ascii())
                .ok_or("时间字段不是有效日期")?,
            PartitionBy::TradingDay => text(&row[position("trading_day")], "交易日字段必须为文本")?,
        };
        incoming.entry(key.to_string()).or_default().push(index);
    }

    let mut changes = Changes::default();
    let mut metrics = MergeMetrics::default();
    let mut partitions = Vec::new();
    let mut touched: Vec<Vec<Cell>> = Vec::new();
    let keys: BTreeSet<&String> = previous.keys().chain(incoming.keys()).collect();
    for key in keys {
        let Some(indexes) = incoming.get(key) else {
            partitions.push(previous[key].clone());
            continue;
        };
        let mut merged: BTreeMap<Vec<Cell>, Vec<Cell>> = BTreeMap::new();
        let primary = |row: &[Cell]| key_columns.iter().map(|&i| row[i].clone()).collect();
        if let Some(part) = previous.get(key) {
            let (names, rows) = cells(&read_partition(store, part)?)?;
            metrics.partitions_read.push(key.clone());
            metrics.bytes_read += part.bytes;
            if names.iter().collect::<BTreeSet<_>>() != expected || names.len() != output.len() {
                return Err("父版本分区字段与本次采集不一致".into());
            }
            let order: Vec<usize> = output
                .iter()
                .map(|n| names.iter().position(|m| m == n).expect("same fields"))
                .collect();
            for row in rows {
                let row: Vec<Cell> = order.iter().map(|&i| row[i].clone()).collect();
                merged.insert(primary(&row), row);
            }
        }
        let mut changed = false;
        for &index in indexes {
            let row = &request.rows[index];
            let pk: Vec<Cell> = primary(row);
            if let Some(current) = merged.get(&pk) {
                let same = current[..width] == row[..];
                let current_time = instant(text(&current[width], "分区来源观测时间无效")?)?;
                if observed[index] < current_time {
                    changes.stale_ignored += 1;
                    continue;
                }
                if observed[index] == current_time {
                    if !same {
                        return Err("相同采集时间出现不同记录，拒绝自动裁决".into());
                    }
                    changes.unchanged += 1;
                    continue;
                }
                if same {
                    changes.refreshed += 1;
                } else {
                    changes.revised += 1;
                }
            } else {
                changes.added += 1;
            }
            let mut stored = row.clone();
            stored.push(Cell::Text(request.observed_at[index].clone()));
            stored.push(Cell::Text(request.raw_version_id.clone()));
            merged.insert(pk, stored);
            changed = true;
        }
        let mut values: Vec<(Vec<Cell>, Vec<Cell>)> = merged.into_iter().collect();
        values.sort_by(|(a_key, a), (b_key, b)| (&a[time], a_key).cmp(&(&b[time], b_key)));
        let values: Vec<Vec<Cell>> = values.into_iter().map(|(_, row)| row).collect();
        let part = if changed {
            let stored = store_partition(store, &encode(&output, &values)?)?;
            let inputs: BTreeSet<String> = values
                .iter()
                .map(|row| text(&row[width + 1], "分区原始版本标识无效").map(str::to_string))
                .collect::<Result<_>>()?;
            Partition {
                key: key.clone(),
                checksum: stored.sha256,
                rows: values.len() as i64,
                bytes: stored.bytes,
                first: text(&values[0][time], "时间字段必须为文本")?.to_string(),
                last: text(&values[values.len() - 1][time], "时间字段必须为文本")?.to_string(),
                inputs: inputs.into_iter().collect(),
                replaces: previous.get(key).map(|p| p.checksum.clone()),
            }
        } else {
            previous[key].clone()
        };
        partitions.push(part);
        touched.extend(values);
    }
    let rows = check_partitions(&partitions)?;
    let (first, last) = (
        partitions[0].first.clone(),
        partitions[partitions.len() - 1].last.clone(),
    );
    let (coverage, gaps) = if layout.calendar {
        let dates = request
            .rows
            .iter()
            .map(|row| day(text(&row[time], "日历日期无效")?))
            .collect::<Result<Vec<_>>>()?;
        let gaps = calendar_gaps(parent, &dates, day(&first)?, day(&last)?)?;
        let coverage = if gaps.is_empty() {
            "CALENDAR_COMPLETE"
        } else {
            "CALENDAR_GAPS"
        };
        (coverage, Some(gaps))
    } else {
        ("RETURNED_ROWS_ONLY", None)
    };
    // The first maximum in touched order, then the parent's, as recorded text.
    let mut latest: Option<(DateTime<FixedOffset>, String)> = None;
    let candidates = touched
        .iter()
        .map(|row| text(&row[width], "分区来源观测时间无效"))
        .chain(parent.map(|p| Ok(p.observed_at.as_str())));
    for candidate in candidates {
        let candidate = candidate?;
        let value = instant(candidate)?;
        if latest.as_ref().is_none_or(|(best, _)| value > *best) {
            latest = Some((value, candidate.to_string()));
        }
    }
    let (_, latest) = latest.expect("at least one touched row");
    let index = store
        .put_addressed(
            &format!("datasets/{}/partitions", request.job_id),
            ".json",
            &encode_index(&partitions)?,
        )
        .map_err(|e| e.to_string())?;
    let detail = json!({
        "observed_at": latest,
        "available_at": latest,
        "parent_version_id": parent.map(|p| p.id.clone()),
        "revision": parent.map_or(1, |p| p.revision + 1),
        "merge_policy": "LATEST_OBSERVED_ROW_NO_DELETIONS",
        "changes": changes,
        "partitions": partitions,
        "logical_bytes": partitions.iter().map(|p| p.bytes).sum::<u64>(),
        "first": first,
        "last": last,
        "coverage": coverage,
        "coverage_gaps": gaps,
        "acquired_rows": request.rows.len(),
    });
    let touched = touched
        .into_iter()
        .map(|row| output.iter().cloned().zip(row).collect())
        .collect();
    Ok(MergeOutcome {
        series: layout.series,
        rows,
        index,
        detail,
        touched,
        metrics,
    })
}
