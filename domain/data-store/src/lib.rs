//! Fixed-version data: the built-in data types, partitioned cumulative
//! publication, verified partition reads and bounded daily scans. This L2 plugin owns Parquet, partition and
//! identity semantics; confined file access and write-once bytes are L1.
pub mod bars;
pub mod identity;
mod merge;
pub mod minute;
mod partitions;
pub mod provider;
pub mod pyfmt;
mod request;
mod scan;
mod source;
pub mod table;
pub mod types;

pub use merge::{
    CoverageGap, MergeMetrics, MergeOutcome, MergeRequest, ParentVersion, cumulative_series, merge,
};
pub use partitions::{
    Cell, Partition, PartitionedVersion, read_index, read_partition, verify_version,
};
pub use request::{ScanRequest, invoke, schema};
pub use scan::{DailyScan, Metrics};

pub type Result<T> = std::result::Result<T, String>;
