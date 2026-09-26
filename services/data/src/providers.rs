//! The built-in data sources. Sources are compiled into the release; none are
//! installable, and every plan passes the same admission check.
use asterion_data_store::provider::{
    Partition, Provider, ProviderManifest, Result, SyncRequest, checked_plan,
};
use asterion_provider_tushare::Tushare;

pub fn builtin() -> Vec<Box<dyn Provider>> {
    vec![Box::new(Tushare::default())]
}

pub fn manifests() -> Vec<ProviderManifest> {
    builtin()
        .iter()
        .map(|provider| provider.manifest())
        .collect()
}

pub fn get(id: &str) -> Result<Box<dyn Provider>> {
    builtin()
        .into_iter()
        .find(|provider| provider.manifest().id == id)
        .ok_or_else(|| "不支持该数据源；数据源只能是内置实现".into())
}

/// A provider's partitions for a request, admitted by `checked_plan`.
pub fn plan(provider: &dyn Provider, request: &SyncRequest) -> Result<Vec<Partition>> {
    let parts = provider.plan(request)?;
    checked_plan(request, &parts)?;
    Ok(parts)
}
