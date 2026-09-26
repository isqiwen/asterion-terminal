//! Data application service (L3): the catalogue of published immutable data
//! versions and checksum-verified reading of their rows. The Rust entry serves it directly; until the remaining data
//! consumers move to Rust, the internal Python process calls the same library
//! through the language binding, so each operation has one implementation.
pub mod catalog;
pub mod collect;
pub mod configuration;
pub mod connections;
pub mod credentials;
pub mod ids;
pub mod imports;
pub mod lifecycle;
pub mod observations;
pub mod preview;
pub mod providers;
pub mod snapshots;
pub mod sync;
