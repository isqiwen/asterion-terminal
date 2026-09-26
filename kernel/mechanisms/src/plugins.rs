//! Plugin declaration/permission checks and lifecycle state owned by the kernel.
//! Source dependencies and activation dependencies are intentionally separate.

use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
pub enum Layer {
    L2,
    L3,
    L4,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct Capability {
    pub id: String,
    pub provider: String,
    pub contract: String,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct Resource {
    pub id: String,
    pub contract: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub id: String,
    pub api_version: i64,
    pub layer: Layer,
    pub type_requires: Vec<String>,
    pub requires: Vec<String>,
    pub provides: Vec<Capability>,
    pub consumes: Vec<Capability>,
    pub resources: Vec<Resource>,
    #[serde(default)]
    pub observes: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ValidationRequest {
    pub plugins: Vec<Manifest>,
}

pub type Grants = BTreeMap<String, Vec<Resource>>;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BindingsRequest {
    pub plugins: Vec<Manifest>,
    pub grants: Grants,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Plan {
    pub order: Vec<String>,
    pub type_order: Vec<String>,
}

fn valid_id(value: &str) -> bool {
    let mut segments = value.split('.');
    let Some(first) = segments.next() else {
        return false;
    };
    first.as_bytes().first().is_some_and(u8::is_ascii_lowercase)
        && first
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
        && value.contains('.')
        && segments.all(|p| {
            !p.is_empty()
                && p.bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
        })
}

fn reserved(value: &str) -> bool {
    [
        "kernel",
        "foundation",
        "core",
        "asterion.kernel",
        "asterion.foundation",
        "asterion.core",
    ]
    .iter()
    .any(|prefix| {
        value == *prefix
            || value
                .strip_prefix(prefix)
                .is_some_and(|s| s.starts_with('.'))
    })
}

pub fn validate(plugins: &[Manifest]) -> Result<Plan, String> {
    let mut by_id = BTreeMap::new();
    for plugin in plugins {
        if plugin.api_version != 1 {
            return Err(format!("Unsupported plugin contract: {}", plugin.id));
        }
        if !valid_id(&plugin.id) {
            return Err(format!("Invalid plugin ID: {}", plugin.id));
        }
        if reserved(&plugin.id) {
            return Err(format!("Reserved kernel namespace: {}", plugin.id));
        }
        if by_id.insert(plugin.id.as_str(), plugin).is_some() {
            return Err(format!("Duplicate plugin: {}", plugin.id));
        }
    }
    // Both complete graphs are checked before any callback is admitted.
    let order = graph_order(plugins, &by_id, false)?;
    let type_order = graph_order(plugins, &by_id, true)?;
    let mut capabilities = BTreeMap::new();
    for plugin in plugins {
        for dependency in &plugin.type_requires {
            let target = by_id[dependency.as_str()];
            if target.layer > plugin.layer {
                return Err(format!(
                    "Source dependency points upward: {} -> {}",
                    plugin.id, target.id
                ));
            }
            if plugin.layer == Layer::L4 && target.layer == Layer::L2 {
                return Err(format!(
                    "UI must use an application interface: {} -> {}",
                    plugin.id, target.id
                ));
            }
        }
        for dependency in &plugin.requires {
            let target = by_id[dependency.as_str()];
            if (plugin.layer != Layer::L4 && target.layer == Layer::L4)
                || (plugin.layer == Layer::L4 && target.layer == Layer::L2)
            {
                return Err(format!(
                    "Invalid activation layer boundary: {} -> {}",
                    plugin.id, target.id
                ));
            }
        }
        for capability in &plugin.provides {
            if capability.provider != plugin.id {
                return Err(format!("Capability owner mismatch: {}", capability.id));
            }
            if reserved(&capability.id) {
                return Err(format!("Reserved kernel capability: {}", capability.id));
            }
            if capability.id.is_empty() || capability.contract.is_empty() {
                return Err("Capability identity and contract must be nonempty".into());
            }
            if capabilities
                .insert(capability.id.as_str(), capability)
                .is_some()
            {
                return Err(format!("Duplicate capability: {}", capability.id));
            }
        }
        // Resource multiplicity is checked again with grants at activation; doing
        // it there preserves declaration-only inspection without granting access.
        for resource in &plugin.resources {
            if resource.id.is_empty() || resource.contract.is_empty() {
                return Err("Resource identity and contract must be nonempty".into());
            }
        }
    }
    for plugin in plugins {
        let mut consumed = BTreeSet::new();
        for capability in &plugin.consumes {
            if !consumed.insert(capability.id.as_str()) {
                return Err(format!(
                    "Duplicate capability consumption: {}",
                    capability.id
                ));
            }
            if !plugin.requires.contains(&capability.provider) {
                return Err(format!("Undeclared provider dependency: {}", capability.id));
            }
            if capabilities.get(capability.id.as_str()) != Some(&capability) {
                return Err(format!(
                    "Missing or mismatched capability: {}",
                    capability.id
                ));
            }
        }
    }
    Ok(Plan { order, type_order })
}

fn graph_order<'a>(
    plugins: &'a [Manifest],
    by_id: &BTreeMap<&'a str, &'a Manifest>,
    types: bool,
) -> Result<Vec<String>, String> {
    fn visit<'a>(
        id: &'a str,
        by_id: &BTreeMap<&'a str, &'a Manifest>,
        types: bool,
        visiting: &mut BTreeSet<&'a str>,
        visited: &mut BTreeSet<&'a str>,
        ordered: &mut Vec<String>,
    ) -> Result<(), String> {
        if visiting.contains(id) {
            return Err(format!(
                "Plugin {}dependency cycle: {id}",
                if types { "type " } else { "" }
            ));
        }
        if visited.contains(id) {
            return Ok(());
        }
        let plugin = by_id
            .get(id)
            .ok_or_else(|| format!("Missing required plugin: {id}"))?;
        visiting.insert(id);
        let dependencies = if types {
            &plugin.type_requires
        } else {
            &plugin.requires
        };
        let mut unique = BTreeSet::new();
        for dependency in dependencies {
            if !unique.insert(dependency.as_str()) {
                return Err(format!("Duplicate plugin dependency: {id} -> {dependency}"));
            }
            visit(dependency, by_id, types, visiting, visited, ordered)?;
        }
        visiting.remove(id);
        visited.insert(id);
        ordered.push(id.to_owned());
        Ok(())
    }
    let (mut visiting, mut visited, mut ordered) = (BTreeSet::new(), BTreeSet::new(), Vec::new());
    for plugin in plugins {
        visit(
            &plugin.id,
            by_id,
            types,
            &mut visiting,
            &mut visited,
            &mut ordered,
        )?;
    }
    Ok(ordered)
}

pub fn validate_grants(plugins: &[Manifest], grants: &Grants) -> Result<(), String> {
    let declared: BTreeSet<_> = plugins.iter().map(|p| p.id.as_str()).collect();
    if grants.keys().any(|id| !declared.contains(id.as_str())) {
        return Err("Resource grants name unknown plugins".into());
    }
    for plugin in plugins {
        let expected: BTreeSet<_> = plugin.resources.iter().collect();
        let ids: BTreeSet<_> = plugin.resources.iter().map(|r| r.id.as_str()).collect();
        if ids.len() != plugin.resources.len() {
            return Err(format!("Duplicate resource declaration: {}", plugin.id));
        }
        let actual = grants.get(&plugin.id).map(Vec::as_slice).unwrap_or(&[]);
        let actual_set: BTreeSet<_> = actual.iter().collect();
        if actual_set.len() != actual.len() || expected != actual_set {
            return Err(format!(
                "Resource grants do not match declaration: {}",
                plugin.id
            ));
        }
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HostState {
    Declared,
    Activating,
    Active,
    Closed,
}

/// A permission/liveness guard, never an unscoped service locator. A binding must
/// check it on each dispatch; closing invalidates even handles cloned earlier.
#[derive(Clone)]
pub struct ScopedHandle {
    host: Arc<AtomicBool>,
    owner: Arc<AtomicBool>,
    provider: Option<Arc<AtomicBool>>,
}

impl ScopedHandle {
    pub(crate) fn same_scope(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.host, &other.host)
            && Arc::ptr_eq(&self.owner, &other.owner)
            && match (&self.provider, &other.provider) {
                (None, None) => true,
                (Some(a), Some(b)) => Arc::ptr_eq(a, b),
                _ => false,
            }
    }
    pub fn check(&self) -> Result<(), String> {
        if !self.host.load(Ordering::Acquire)
            || !self.owner.load(Ordering::Acquire)
            || self
                .provider
                .as_ref()
                .is_some_and(|p| !p.load(Ordering::Acquire))
        {
            Err("Plugin handle is closed or not active".into())
        } else {
            Ok(())
        }
    }
}

/// Fixed lifecycle coordinator. Language hosts execute the returned activation
/// and reverse-close callbacks; they must stop owned work before resource close.
/// This slice does not implement task draining or a scheduler.
pub struct Host {
    plugins: BTreeMap<String, Manifest>,
    plan: Plan,
    state: HostState,
    current: Option<String>,
    active: Vec<String>,
    returned: Vec<String>,
    live: Arc<AtomicBool>,
    plugin_live: BTreeMap<String, Arc<AtomicBool>>,
}

impl Host {
    pub fn new(plugins: Vec<Manifest>) -> Result<Self, String> {
        let plan = validate(&plugins)?;
        let plugin_live = plugins
            .iter()
            .map(|p| (p.id.clone(), Arc::new(AtomicBool::new(false))))
            .collect();
        Ok(Self {
            plugins: plugins.into_iter().map(|p| (p.id.clone(), p)).collect(),
            plan,
            state: HostState::Declared,
            current: None,
            active: Vec::new(),
            returned: Vec::new(),
            live: Arc::new(AtomicBool::new(false)),
            plugin_live,
        })
    }

    pub fn state(&self) -> HostState {
        self.state
    }
    pub fn plan(&self) -> &Plan {
        &self.plan
    }

    pub fn begin(&mut self, grants: &Grants) -> Result<(), String> {
        if self.state != HostState::Declared {
            return Err("Plugin host cannot be activated twice".into());
        }
        if let Err(error) =
            validate_grants(&self.plugins.values().cloned().collect::<Vec<_>>(), grants)
        {
            self.close();
            return Err(error);
        }
        self.state = HostState::Activating;
        self.live.store(true, Ordering::Release);
        Ok(())
    }

    pub fn next_activation(&mut self) -> Result<Option<String>, String> {
        if self.state != HostState::Activating || !self.live.load(Ordering::Acquire) {
            return Err("Plugin host is not activating".into());
        }
        if self.current.is_some() {
            return Err("Previous plugin activation is unfinished".into());
        }
        self.current = self.plan.order.get(self.active.len()).cloned();
        if let Some(id) = &self.current {
            self.plugin_live[id].store(true, Ordering::Release);
        }
        Ok(self.current.clone())
    }

    pub fn returned(&mut self, id: &str) -> Result<(), String> {
        if self.state != HostState::Activating || self.current.as_deref() != Some(id) {
            return Err(format!("Unexpected plugin activation: {id}"));
        }
        if self.returned.iter().any(|value| value == id) {
            return Err(format!("Plugin callback already returned: {id}"));
        }
        self.returned.push(id.to_owned());
        Ok(())
    }

    pub fn activated(&mut self, id: &str, exports: &[Capability]) -> Result<(), String> {
        if self.state != HostState::Activating || self.current.as_deref() != Some(id) {
            return Err(format!("Unexpected plugin activation: {id}"));
        }
        // Language bindings acknowledge callback completion before decoding its
        // exports, so malformed language values also receive cleanup on abort.
        if self.returned.last().map(String::as_str) != Some(id) {
            self.returned(id)?;
        }
        self.current = None;
        let actual: BTreeSet<_> = exports.iter().collect();
        let expected: BTreeSet<_> = self.plugins[id].provides.iter().collect();
        if actual.len() != exports.len() || actual != expected {
            self.live.store(false, Ordering::Release);
            return Err(format!("Capability exports do not match declaration: {id}"));
        }
        self.active.push(id.to_owned());
        Ok(())
    }

    pub fn finish(&mut self) -> Result<(), String> {
        if self.state != HostState::Activating
            || self.current.is_some()
            || self.active.len() != self.plan.order.len()
            || !self.live.load(Ordering::Acquire)
        {
            return Err("Plugin host activation is incomplete".into());
        }
        self.state = HostState::Active;
        Ok(())
    }

    fn handle(&self, owner: &str, provider: Option<&str>) -> Result<ScopedHandle, String> {
        let owner_live = self
            .plugin_live
            .get(owner)
            .ok_or_else(|| format!("Unknown plugin: {owner}"))?;
        let provider_live = provider
            .map(|id| {
                self.plugin_live
                    .get(id)
                    .cloned()
                    .ok_or_else(|| format!("Unknown plugin: {id}"))
            })
            .transpose()?;
        let handle = ScopedHandle {
            host: self.live.clone(),
            owner: owner_live.clone(),
            provider: provider_live,
        };
        handle.check()?;
        Ok(handle)
    }

    pub fn resource_handle(
        &self,
        owner: &str,
        resource: &Resource,
    ) -> Result<ScopedHandle, String> {
        let plugin = self
            .plugins
            .get(owner)
            .ok_or_else(|| format!("Unknown plugin: {owner}"))?;
        if !plugin.resources.contains(resource) {
            return Err(format!("Undeclared resource: {owner} -> {}", resource.id));
        }
        self.handle(owner, None)
    }

    pub fn capability_handle(
        &self,
        owner: &str,
        capability: &Capability,
    ) -> Result<ScopedHandle, String> {
        let plugin = self
            .plugins
            .get(owner)
            .ok_or_else(|| format!("Unknown plugin: {owner}"))?;
        if !plugin.consumes.contains(capability) {
            return Err(format!(
                "Undeclared capability: {owner} -> {}",
                capability.id
            ));
        }
        if !self.active.contains(&capability.provider) {
            return Err(format!("Plugin is not active: {}", capability.provider));
        }
        self.handle(owner, Some(&capability.provider))
    }

    pub fn export_handle(&self, capability: &Capability) -> Result<ScopedHandle, String> {
        if !self.active.contains(&capability.provider) {
            return Err(format!("Plugin is not active: {}", capability.provider));
        }
        if !self.plugins[&capability.provider]
            .provides
            .contains(capability)
        {
            return Err(format!("Capability is not available: {}", capability.id));
        }
        self.handle(&capability.provider, None)
    }

    pub fn observation_handle(&self, owner: &str, name: &str) -> Result<ScopedHandle, String> {
        let plugin = self
            .plugins
            .get(owner)
            .ok_or_else(|| format!("Unknown plugin: {owner}"))?;
        if !plugin.observes.iter().any(|value| value == name) {
            return Err(format!("Undeclared hook observation: {owner} -> {name}"));
        }
        self.handle(owner, None)
    }

    pub fn context_handle(&self, owner: &str) -> Result<ScopedHandle, String> {
        if self.state != HostState::Activating || self.current.as_deref() != Some(owner) {
            return Err("Plugin context is not activating".into());
        }
        self.handle(owner, None)
    }

    pub fn plugin_handle(&self, owner: &str) -> Result<ScopedHandle, String> {
        if !self.active.iter().any(|id| id == owner) {
            return Err(format!("Plugin is not active: {owner}"));
        }
        self.handle(owner, None)
    }

    pub fn close(&mut self) -> Vec<String> {
        if self.state == HostState::Closed {
            return Vec::new();
        }
        self.live.store(false, Ordering::Release);
        for live in self.plugin_live.values() {
            live.store(false, Ordering::Release);
        }
        self.state = HostState::Closed;
        self.current = None;
        self.active.clear();
        self.returned.drain(..).rev().collect()
    }
}

impl Drop for Host {
    fn drop(&mut self) {
        self.close();
    }
}
