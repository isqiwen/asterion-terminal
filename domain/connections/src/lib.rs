//! L2 connection ownership. Protocol implementations enter only through these
//! source ports; this plugin never imports a vendor or grants trading writes.
mod models;
mod schema;
pub use models::*;
pub use schema::invoke;

use serde_json::{Value, json};
use std::{
    cell::Cell,
    collections::BTreeMap,
    io::Read,
    path::{Path, PathBuf},
    sync::{
        Arc, Condvar, Mutex, Weak,
        atomic::{AtomicBool, Ordering},
    },
    thread::ThreadId,
    time::{SystemTime, UNIX_EPOCH},
};

pub type Result<T> = std::result::Result<T, Error>;
thread_local! { static CALLBACK_DEPTH: Cell<usize> = const { Cell::new(0) }; }

fn bounded_message(message: impl Into<String>) -> String {
    let message = message.into();
    if message.len() > MAX_TEXT_BYTES {
        "连接信息超出文本预算".into()
    } else {
        message
    }
}
#[derive(Debug)]
pub struct Error {
    pub message: String,
    pub category: &'static str,
    pub retryable: bool,
    pub io: bool,
}
impl Error {
    pub fn invalid(message: impl Into<String>) -> Self {
        Self {
            message: bounded_message(message),
            category: "invalid",
            retryable: false,
            io: false,
        }
    }
    pub fn source(message: impl Into<String>, category: &'static str, retryable: bool) -> Self {
        Self {
            message: bounded_message(message),
            category,
            retryable,
            io: false,
        }
    }
    fn io() -> Self {
        Self {
            message: "连接配置保存失败，请重新加载确认当前配置".into(),
            category: "storage",
            retryable: true,
            io: true,
        }
    }
    fn stale() -> Self {
        Self::source("连接已变更，响应已丢弃", "session_invalid", false)
    }
}
impl From<serde_json::Error> for Error {
    fn from(_: serde_json::Error) -> Self {
        Self::invalid("连接契约格式无效")
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}
impl std::error::Error for Error {}

/// Already authorized fixed L1 secret port; no source can select cryptography.
pub trait SecretPort: Send + Sync {
    fn encrypt(&self, value: &[u8]) -> Result<Vec<u8>>;
    fn decrypt(&self, value: &[u8]) -> Result<Vec<u8>>;
}
pub trait Connector: Send + Sync {
    fn validate(&self, config: &Values, secrets: &Values) -> Result<()>;
    fn open(&self, profile: &ConnectionProfile, secrets: &Values) -> Result<Arc<dyn Session>>;
}
pub trait Session: Send + Sync {
    fn start_market(&self, subscriptions: &[Subscription], emit: Emitter) -> Result<()>;
    fn subscriptions(&self, subscriptions: &[Subscription]) -> Result<()>;
    fn stop_market(&self) -> Result<()>;
    fn read(&self, kind: ReadKind, request: &ReadRequest, cancel: Arc<ReadTicket>)
    -> Result<Value>;
    fn close(&self) -> Result<()>;
}
pub trait Cancellation: Send + Sync {
    fn is_cancelled(&self) -> bool;
}
pub struct Contribution {
    pub descriptor: ConnectorDescriptor,
    pub source: Arc<dyn Connector>,
}

#[derive(Clone, Default)]
struct Runtime {
    market: ChannelState,
    account: ChannelState,
    subscriptions: Vec<Subscription>,
}
struct Lease {
    id: String,
    revision: u64,
    live: AtomicBool,
}
struct Attached {
    lease: Arc<Lease>,
    session: Arc<dyn Session>,
}
struct State {
    saved: Vec<Saved>,
    selected: Option<String>,
    catalog: Vec<Contribution>,
    runtimes: BTreeMap<String, Runtime>,
    active: Option<Attached>,
    initialized: bool,
    closed: bool,
    load_error: bool,
}
impl State {
    fn row(&self, id: &str) -> Result<&Saved> {
        self.saved
            .iter()
            .find(|r| r.profile.connection_id == id)
            .ok_or_else(|| Error::invalid("连接不存在，请先配置连接"))
    }
    fn runtime(&mut self, id: &str) -> Result<&mut Runtime> {
        self.row(id)?;
        Ok(self.runtimes.entry(id.into()).or_default())
    }
    fn connector(&self, id: &str, feature: Option<Feature>) -> Result<&Contribution> {
        if self.closed || !self.initialized {
            return Err(Error::source("连接服务尚未就绪", "unavailable", false));
        }
        let row = self.row(id)?;
        let source = self
            .catalog
            .iter()
            .find(|c| c.descriptor.id == row.profile.connector_id)
            .ok_or_else(|| Error::source("所需接入插件不可用，配置已保留", "unsupported", false))?;
        if feature.is_some_and(|f| !source.descriptor.supports(f)) {
            return Err(Error::source("当前接入不支持此功能", "unsupported", false));
        }
        Ok(source)
    }
    fn writable(&self) -> Result<()> {
        if self.load_error {
            return Err(Error::invalid(LOAD_NOTICE));
        }
        if self.closed || !self.initialized {
            return Err(Error::source("连接服务尚未就绪", "unavailable", false));
        }
        Ok(())
    }
    fn offline(&self, id: &str) -> bool {
        self.active.as_ref().is_none_or(|a| a.lease.id != id)
            && self.runtimes.get(id).is_none_or(|r| {
                r.market.state == ConnectionStatus::Disconnected
                    && r.account.state == ConnectionStatus::Disconnected
            })
    }
}
const LOAD_NOTICE: &str = "连接配置校验失败，原文件已保留；当前配置不受支持。";

/// Serialize user lifecycle operations, without holding the state lock across
/// provider calls. Synchronous provider callbacks may inspect state and emit;
/// recursively initiating a lifecycle operation is rejected instead of deadlocking.
#[derive(Default)]
struct Gate {
    owner: Mutex<Option<ThreadId>>,
    wake: Condvar,
}
struct Operation<'a>(&'a Gate);
impl Gate {
    fn enter(&self) -> Result<Operation<'_>> {
        if CALLBACK_DEPTH.get() != 0 {
            return Err(Error::invalid("回调内不能重入连接生命周期操作"));
        }
        let current = std::thread::current().id();
        let mut owner = self.owner.lock().unwrap();
        if *owner == Some(current) {
            return Err(Error::invalid("连接操作正在执行，不支持回调内重入"));
        }
        while owner.is_some() {
            owner = self.wake.wait(owner).unwrap();
        }
        *owner = Some(current);
        Ok(Operation(self))
    }
}
impl Drop for Operation<'_> {
    fn drop(&mut self) {
        *self.0.owner.lock().unwrap() = None;
        self.0.wake.notify_all();
    }
}

pub struct Manager {
    path: PathBuf,
    secrets: Arc<dyn SecretPort>,
    owners: Values,
    state: Mutex<State>,
    operation: Gate,
}
impl Manager {
    pub fn new(
        path: impl Into<PathBuf>,
        secrets: Arc<dyn SecretPort>,
        owners: Values,
    ) -> Arc<Self> {
        let path = path.into();
        let loaded = Self::load(&path, secrets.as_ref());
        let load_error = loaded.is_err();
        let (saved, selected) = loaded.unwrap_or_default();
        Arc::new(Self {
            path,
            secrets,
            owners,
            state: Mutex::new(State {
                saved,
                selected,
                catalog: vec![],
                runtimes: BTreeMap::new(),
                active: None,
                initialized: false,
                closed: false,
                load_error,
            }),
            operation: Gate::default(),
        })
    }
    fn load(path: &Path, secrets: &dyn SecretPort) -> Result<(Vec<Saved>, Option<String>)> {
        let raw = match read_file(path) {
            Ok(raw) => raw,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok((vec![], None)),
            Err(_) => return Err(Error::invalid(LOAD_NOTICE)),
        };
        let value = asterion_foundation::communication::parse_json(&raw).map_err(Error::invalid)?;
        let stored: Stored = serde_json::from_value(value.clone())?;
        if stored.version != 2
            || stored.connections.len() > MAX_PROFILES
            || value
                .as_object()
                .is_none_or(|m| m.len() != 3 || !m.contains_key("selected_id"))
        {
            return Err(Error::invalid(LOAD_NOTICE));
        }
        let mut ids = std::collections::BTreeSet::new();
        for (row, raw) in stored.connections.iter().zip(
            value["connections"]
                .as_array()
                .ok_or_else(|| Error::invalid(LOAD_NOTICE))?,
        ) {
            // flatten is used only for the current on-disk record; it must not
            // silently ignore fields alongside the embedded profile.
            if raw
                .as_object()
                .is_none_or(|m| m.len() != 6 || !m.contains_key("encrypted"))
            {
                return Err(Error::invalid(LOAD_NOTICE));
            }
            row.profile.validate()?;
            if !ids.insert(&row.profile.connection_id) {
                return Err(Error::invalid(LOAD_NOTICE));
            }
            Self::decrypt(secrets, row)?;
        }
        if stored
            .selected_id
            .as_ref()
            .is_some_and(|id| !ids.contains(id))
        {
            return Err(Error::invalid(LOAD_NOTICE));
        }
        Ok((stored.connections, stored.selected_id))
    }
    fn decrypt(port: &dyn SecretPort, row: &Saved) -> Result<Values> {
        let Some(encrypted) = &row.encrypted else {
            return Ok(Values::new());
        };
        let raw = port.decrypt(encrypted.as_bytes())?;
        let value = asterion_foundation::communication::parse_json(&raw).map_err(Error::invalid)?;
        if value.as_object().is_none_or(|v| v.len() != 2)
            || value["profile"] != serde_json::to_value(&row.profile)?
        {
            return Err(Error::invalid("凭据与连接身份不匹配"));
        }
        let secrets = serde_json::from_value(value["secrets"].clone())?;
        validate_values(&secrets)?;
        Ok(secrets)
    }
    fn persist(&self, rows: &[Saved], selected: &Option<String>) -> Result<()> {
        std::fs::create_dir_all(self.path.parent().ok_or_else(Error::io)?)
            .map_err(|_| Error::io())?;
        let raw = serde_json::to_vec(&Stored {
            version: 2,
            connections: rows.to_vec(),
            selected_id: selected.clone(),
        })?;
        if raw.len() > MAX_STATE_BYTES {
            return Err(Error::invalid("连接配置超出存储预算"));
        }
        let write = asterion_kernel::files::atomic_write(&self.path, &raw, true);
        // L1 explicitly distinguishes errors after rename/link publication.
        // Memory follows that published record even when durability could not
        // be confirmed; the caller still receives an explicit storage failure.
        let visible = write.is_ok() || write.as_ref().is_err_and(|error| error.published());
        if visible {
            let mut state = self.state.lock().unwrap();
            state.saved = rows.to_vec();
            state.selected = selected.clone();
            let ids = rows
                .iter()
                .map(|r| r.profile.connection_id.as_str())
                .collect::<std::collections::BTreeSet<_>>();
            state.runtimes.retain(|id, _| ids.contains(id.as_str()));
        }
        write.map_err(|_| {
            if visible {
                Error {
                    message: "配置已写入，但持久化确认失败；请重新加载确认".into(),
                    category: "storage",
                    retryable: true,
                    io: true,
                }
            } else {
                Error::io()
            }
        })
    }
    pub fn initialize(&self, catalog: Vec<Contribution>) -> Result<()> {
        let _operation = self.operation.enter()?;
        let mut state = self.state.lock().unwrap();
        if state.initialized || state.closed {
            return Err(Error::invalid("接入目录已经初始化"));
        }
        if catalog.len() > MAX_CONNECTORS {
            return Err(Error::invalid("接入数量超出限制"));
        }
        let mut ids = std::collections::BTreeSet::new();
        for item in &catalog {
            item.descriptor.validate()?;
            if self.owners.get(&item.descriptor.id) != Some(&item.descriptor.owner)
                || !ids.insert(&item.descriptor.id)
            {
                return Err(Error::invalid("接入贡献归属错误或 ID 重复"));
            }
        }
        state.catalog = catalog;
        state.initialized = true;
        Ok(())
    }
    pub fn profiles(&self) -> Vec<String> {
        self.state
            .lock()
            .unwrap()
            .saved
            .iter()
            .map(|r| r.profile.connection_id.clone())
            .collect()
    }
    pub fn profile(&self, id: &str) -> Result<ConnectionProfile> {
        Ok(self.state.lock().unwrap().row(id)?.profile.clone())
    }
    pub fn supports(&self, id: &str, feature: Feature) -> bool {
        self.state
            .lock()
            .unwrap()
            .connector(id, Some(feature))
            .is_ok()
    }
    pub fn channel(&self, id: &str, channel: Channel) -> Result<ChannelState> {
        let mut state = self.state.lock().unwrap();
        let r = state.runtime(id)?;
        Ok(match channel {
            Channel::Market => &r.market,
            Channel::Account => &r.account,
        }
        .clone())
    }
    pub fn active_id(&self) -> Option<String> {
        self.state
            .lock()
            .unwrap()
            .active
            .as_ref()
            .map(|a| a.lease.id.clone())
    }
    fn render(
        &self,
        row: Saved,
        descriptor: Option<ConnectorDescriptor>,
        runtime: Runtime,
    ) -> Result<Value> {
        let secrets = Self::decrypt(self.secrets.as_ref(), &row)?;
        let mut value = serde_json::to_value(row.profile)?;
        value.as_object_mut().unwrap().extend(json!({
            "secret_saved": secrets.into_iter().map(|(k,v)| (k,!v.is_empty())).collect::<BTreeMap<_,_>>(),
            "capabilities": descriptor.as_ref().map(|d| d.capabilities.clone()).unwrap_or_default(),
            "available": descriptor.is_some(), "market": runtime.market, "account": runtime.account,
        }).as_object().unwrap().clone());
        Ok(value)
    }
    pub fn view(&self, id: &str) -> Result<Value> {
        let (row, descriptor, runtime) = {
            let state = self.state.lock().unwrap();
            let row = state.row(id)?.clone();
            let descriptor = state
                .catalog
                .iter()
                .find(|c| c.descriptor.id == row.profile.connector_id)
                .map(|c| c.descriptor.clone());
            (
                row,
                descriptor,
                state.runtimes.get(id).cloned().unwrap_or_default(),
            )
        };
        self.render(row, descriptor, runtime)
    }
    pub fn snapshot(&self) -> Result<Value> {
        self.snapshot_inner()
    }
    fn snapshot_inner(&self) -> Result<Value> {
        // Capture one state revision. Decrypt outside the state lock and do not
        // acquire the lifecycle gate: SDK callbacks may read this snapshot.
        let (rows, runtimes, descriptors, selected, active, failed) = {
            let state = self.state.lock().unwrap();
            (
                state.saved.clone(),
                state.runtimes.clone(),
                state
                    .catalog
                    .iter()
                    .map(|c| c.descriptor.clone())
                    .collect::<Vec<_>>(),
                state.selected.clone(),
                state.active.as_ref().map(|a| a.lease.id.clone()),
                state.load_error,
            )
        };
        let connections = rows
            .into_iter()
            .map(|row| {
                let descriptor = descriptors
                    .iter()
                    .find(|d| d.id == row.profile.connector_id)
                    .cloned();
                let runtime = runtimes
                    .get(&row.profile.connection_id)
                    .cloned()
                    .unwrap_or_default();
                self.render(row, descriptor, runtime)
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(
            json!({"connections": connections, "connectors": descriptors, "selected_id": selected, "active_id": active, "notice": if failed { LOAD_NOTICE } else { "" }}),
        )
    }
    pub fn save(&self, body: Save) -> Result<Value> {
        let _operation = self.operation.enter()?;
        validate_values(&body.config)?;
        if body.secrets.len() > MAX_FIELDS
            || body
                .secrets
                .values()
                .any(|v| v.value.as_ref().is_some_and(|v| v.len() > MAX_TEXT_BYTES))
        {
            return Err(Error::invalid("凭据超出配置预算"));
        }
        let (descriptor, source, existing, mut rows, selected) = {
            let state = self.state.lock().unwrap();
            state.writable()?;
            let item = state
                .catalog
                .iter()
                .find(|c| c.descriptor.id == body.connector_id)
                .ok_or_else(|| Error::invalid("接入方式不受支持"))?;
            let existing = body
                .connection_id
                .as_ref()
                .map(|id| state.row(id).cloned())
                .transpose()?;
            if let Some(existing) = &existing {
                if body.expected_revision != Some(existing.profile.config_revision) {
                    return Err(Error::invalid("配置已改变，请重新加载"));
                }
                if body.connector_id != existing.profile.connector_id
                    || item.descriptor.fields.iter().any(|f| {
                        f.identity && body.config.get(&f.key) != existing.profile.config.get(&f.key)
                    })
                {
                    return Err(Error::invalid("账户身份改变请保存为另一份配置"));
                }
                if !state.offline(&existing.profile.connection_id) {
                    return Err(Error::invalid("请先断开连接再修改配置"));
                }
            } else if body.expected_revision.is_some() {
                return Err(Error::invalid("新配置不得携带修订"));
            }
            (
                item.descriptor.clone(),
                item.source.clone(),
                existing,
                state.saved.clone(),
                state.selected.clone(),
            )
        };
        let keys = |secret| {
            descriptor
                .fields
                .iter()
                .filter(|f| f.secret == secret)
                .map(|f| &f.key)
                .collect::<std::collections::BTreeSet<_>>()
        };
        if keys(false) != body.config.keys().collect()
            || keys(true) != body.secrets.keys().collect()
        {
            return Err(Error::invalid("配置字段不符合接入契约"));
        }
        let mut secrets = existing
            .as_ref()
            .map(|row| Self::decrypt(self.secrets.as_ref(), row))
            .transpose()?
            .unwrap_or_default();
        if existing
            .as_ref()
            .is_some_and(|row| body.config != row.profile.config)
            && body
                .secrets
                .values()
                .any(|v| v.action == SecretAction::Keep)
        {
            return Err(Error::invalid("连接目标改变，请重新输入凭据确认"));
        }
        for (key, change) in body.secrets {
            if (change.action == SecretAction::Replace) != change.value.is_some() {
                return Err(Error::invalid("替换凭据必须提供值，其他操作不得携带值"));
            }
            match change.action {
                SecretAction::Replace => {
                    secrets.insert(key, change.value.unwrap());
                }
                SecretAction::Clear => {
                    secrets.remove(&key);
                }
                SecretAction::Keep => {}
            }
        }
        if descriptor
            .fields
            .iter()
            .any(|f| !f.secret && f.required && body.config[&f.key].trim().is_empty())
        {
            return Err(Error::invalid("必填配置不能为空"));
        }
        let profile = ConnectionProfile {
            connection_id: body
                .connection_id
                .unwrap_or_else(|| uuid::Uuid::new_v4().simple().to_string()),
            connector_id: body.connector_id,
            name: body.name,
            config: body.config,
            config_revision: match &existing {
                Some(row) => row
                    .profile
                    .config_revision
                    .checked_add(1)
                    .ok_or_else(|| Error::invalid("配置修订已耗尽"))?,
                None => 1,
            },
        };
        profile.validate()?;
        if descriptor
            .fields
            .iter()
            .filter(|f| f.secret && f.required)
            .all(|f| secrets.get(&f.key).is_some_and(|v| !v.is_empty()))
        {
            source.validate(&profile.config, &secrets)?;
        }
        let encrypted = if secrets.is_empty() {
            None
        } else {
            Some(
                String::from_utf8(self.secrets.encrypt(&serde_json::to_vec(
                    &json!({"profile": profile, "secrets": secrets}),
                )?)?)
                .map_err(|_| Error::invalid("秘密格式无效"))?,
            )
        };
        let id = profile.connection_id.clone();
        let row = Saved { profile, encrypted };
        match rows.iter_mut().find(|r| r.profile.connection_id == id) {
            Some(old) => *old = row,
            None => {
                if rows.len() >= MAX_PROFILES {
                    return Err(Error::invalid("连接数量超出限制"));
                }
                rows.push(row)
            }
        }
        let selected = selected.or_else(|| Some(id.clone()));
        self.persist(&rows, &selected)?;
        self.view(&id)
    }
    pub fn delete(&self, id: &str, revision: u64) -> Result<Value> {
        let _operation = self.operation.enter()?;
        let (rows, selected) = {
            let state = self.state.lock().unwrap();
            state.writable()?;
            if state.row(id)?.profile.config_revision != revision {
                return Err(Error::invalid("配置已改变，请重新加载后删除"));
            }
            if !state.offline(id) {
                return Err(Error::invalid("请先断开连接再删除配置"));
            }
            (
                state
                    .saved
                    .iter()
                    .filter(|r| r.profile.connection_id != id)
                    .cloned()
                    .collect::<Vec<_>>(),
                state.selected.clone().filter(|v| v != id),
            )
        };
        self.persist(&rows, &selected)?;
        self.snapshot_inner()
    }
    fn select_inner(&self, id: &str) -> Result<()> {
        let rows = {
            let state = self.state.lock().unwrap();
            state.writable()?;
            state.row(id)?;
            state.saved.clone()
        };
        let selected = Some(id.into());
        self.persist(&rows, &selected)?;
        Ok(())
    }
    pub fn select(self: &Arc<Self>, id: &str) -> Result<Value> {
        let _operation = self.operation.enter()?;
        if self.active_id().is_some() {
            self.connect_inner(id)?;
        } else {
            self.select_inner(id)?;
        }
        self.view(id)
    }
    pub fn connect(self: &Arc<Self>, id: &str) -> Result<Value> {
        let _operation = self.operation.enter()?;
        self.connect_inner(id)?;
        self.view(id)
    }
    fn connect_inner(self: &Arc<Self>, id: &str) -> Result<()> {
        let (row, descriptor, source) = {
            let state = self.state.lock().unwrap();
            let source = state.connector(id, None)?;
            (
                state.row(id)?.clone(),
                source.descriptor.clone(),
                source.source.clone(),
            )
        };
        let secrets = Self::decrypt(self.secrets.as_ref(), &row)?;
        let config_keys = descriptor
            .fields
            .iter()
            .filter(|f| !f.secret)
            .map(|f| &f.key)
            .collect::<std::collections::BTreeSet<_>>();
        if config_keys != row.profile.config.keys().collect()
            || secrets
                .keys()
                .any(|key| !descriptor.fields.iter().any(|f| f.secret && &f.key == key))
        {
            return Err(Error::invalid("配置字段不符合当前接入契约"));
        }
        if descriptor
            .fields
            .iter()
            .any(|f| f.secret && f.required && secrets.get(&f.key).is_none_or(|v| v.is_empty()))
        {
            return Err(Error::invalid("请填写所需账户凭据"));
        }
        source.validate(&row.profile.config, &secrets)?;
        if self.active_id().as_deref() == Some(id)
            && self
                .state
                .lock()
                .unwrap()
                .active
                .as_ref()
                .is_some_and(|a| a.lease.live.load(Ordering::Acquire))
        {
            return Ok(());
        }
        for key in self.profiles() {
            self.disconnect_inner(&key)?;
        }
        self.select_inner(id)?;
        let session = source.open(&row.profile, &secrets).map_err(|_| {
            Error::source("连接启动失败，请检查参数及运行组件", "unavailable", true)
        })?;
        let (lease, subscriptions) = {
            let mut state = self.state.lock().unwrap();
            let runtime = state.runtime(id)?;
            let lease = Arc::new(Lease {
                id: id.into(),
                revision: row.profile.config_revision,
                live: AtomicBool::new(true),
            });
            if descriptor.supports(Feature::InstrumentCatalog)
                || descriptor.supports(Feature::AccountSnapshot)
            {
                runtime.account.state = ConnectionStatus::Connecting;
                runtime.account.detail = "等待账户查询认证".into();
            }
            if descriptor.supports(Feature::MarketQuotes) {
                runtime.market.state = ConnectionStatus::Connecting;
                runtime.market.detail = "正在连接".into();
            }
            let subscriptions = runtime.subscriptions.clone();
            state.active = Some(Attached {
                lease: lease.clone(),
                session: session.clone(),
            });
            (lease, subscriptions)
        };
        if descriptor.supports(Feature::MarketQuotes)
            && session
                .start_market(
                    &subscriptions,
                    Emitter {
                        manager: Arc::downgrade(self),
                        lease,
                    },
                )
                .is_err()
        {
            self.disconnect_inner(id)?;
            return Err(Error::source(
                "连接启动失败，请检查参数及运行组件",
                "unavailable",
                true,
            ));
        }
        Ok(())
    }
    pub fn disconnect(&self, id: &str) -> Result<Value> {
        let _operation = self.operation.enter()?;
        self.disconnect_inner(id)?;
        self.view(id)
    }
    fn disconnect_inner(&self, id: &str) -> Result<()> {
        let session = {
            let mut state = self.state.lock().unwrap();
            let r = state.runtime(id)?;
            for channel in [&mut r.market, &mut r.account] {
                channel.generation = channel
                    .generation
                    .checked_add(1)
                    .ok_or_else(|| Error::invalid("会话代次已耗尽"))?;
                channel.state = ConnectionStatus::Disconnected;
                channel.detail = "已断开".into();
            }
            state.active.as_ref().filter(|a| a.lease.id == id).map(|a| {
                a.lease.live.store(false, Ordering::Release);
                a.session.clone()
            })
        };
        if let Some(session) = session {
            session
                .stop_market()
                .and_then(|()| session.close())
                .map_err(|_| {
                    Error::source("原连接尚未完成关闭，请重试断开连接", "unavailable", true)
                })?;
            self.state.lock().unwrap().active = None;
        }
        Ok(())
    }
    pub fn subscribe(&self, id: &str, subscriptions: Vec<Subscription>) -> Result<()> {
        let _operation = self.operation.enter()?;
        if subscriptions.len() > 10000 {
            return Err(Error::invalid("订阅数量超出限制"));
        }
        for item in &subscriptions {
            item.validate()?;
        }
        let session = {
            let mut state = self.state.lock().unwrap();
            state.connector(id, Some(Feature::MarketQuotes))?;
            state.runtime(id)?.subscriptions = subscriptions.clone();
            state
                .active
                .as_ref()
                .filter(|a| a.lease.id == id && a.lease.live.load(Ordering::Acquire))
                .map(|a| a.session.clone())
        };
        if let Some(session) = session {
            session.subscriptions(&subscriptions)?;
        }
        Ok(())
    }
    pub fn read(
        self: &Arc<Self>,
        id: &str,
        kind: ReadKind,
        caller: Arc<dyn Cancellation>,
    ) -> Result<Value> {
        let (session, ticket) = {
            let state = self.state.lock().unwrap();
            state.connector(
                id,
                Some(match kind {
                    ReadKind::Instruments => Feature::InstrumentCatalog,
                    ReadKind::Account => Feature::AccountSnapshot,
                }),
            )?;
            if matches!(kind, ReadKind::Account) {
                state.connector(id, Some(Feature::Positions))?;
            }
            let active = state
                .active
                .as_ref()
                .filter(|a| a.lease.id == id && a.lease.live.load(Ordering::Acquire))
                .ok_or_else(|| Error::source("请先连接已保存的配置", "session_invalid", false))?;
            let runtime = state.runtimes.get(id).ok_or_else(Error::stale)?;
            if runtime.account.state == ConnectionStatus::Disconnected {
                return Err(Error::stale());
            }
            let request = ReadRequest {
                connection_id: id.into(),
                generation: runtime.account.generation,
                request_id: uuid::Uuid::new_v4().simple().to_string(),
                started_at: now(),
            };
            (
                active.session.clone(),
                Arc::new(ReadTicket {
                    lease: active.lease.clone(),
                    caller,
                    request,
                    finished: AtomicBool::new(false),
                }),
            )
        };
        let response = if ticket.is_cancelled() {
            Err(Error::stale())
        } else {
            session.read(kind, &ticket.request, ticket.clone())
        };
        let response = response.and_then(|value| {
            bounded_value(&value, MAX_READ_BYTES)?;
            // The domain envelope is small even when the source payload holds
            // thousands of instruments. Do not clone the entire payload.
            let meta: ReadBatch = serde_json::from_value(json!({
                "connection_id": value["connection_id"], "generation": value["generation"],
                "request_id": value["request_id"], "started_at": value["started_at"],
                "observed_at": value["observed_at"], "complete": value["complete"],
            }))?;
            meta.validate()?;
            if meta.connection_id != ticket.request.connection_id
                || meta.generation != ticket.request.generation
                || meta.request_id != ticket.request.request_id
                || meta.started_at != ticket.request.started_at
            {
                return Err(Error::source(
                    "来源查询失败或响应未通过校验",
                    "incomplete",
                    false,
                ));
            }
            Ok(value)
        });
        // A callback may call Python, so cancellation is sampled before taking
        // the state lock. The lease is checked again under that lock.
        let cancelled = ticket.is_cancelled();
        ticket.finished.store(true, Ordering::Release);
        let mut state = self.state.lock().unwrap();
        let current = state.active.as_ref().is_some_and(|a| {
            Arc::ptr_eq(&a.lease, &ticket.lease)
                && a.lease.live.load(Ordering::Acquire)
                && state
                    .row(id)
                    .is_ok_and(|r| r.profile.config_revision == ticket.lease.revision)
        });
        if cancelled || !current {
            return Err(Error::stale());
        }
        let channel = &mut state.runtime(id)?.account;
        if channel.generation != ticket.request.generation {
            return Err(Error::stale());
        }
        match response {
            Ok(value) => {
                channel.state = ConnectionStatus::Ready;
                channel.detail = "账户数据可用".into();
                Ok(value)
            }
            Err(error) => {
                channel.state = ConnectionStatus::Error;
                channel.detail = error.message.clone();
                Err(error)
            }
        }
    }
    pub fn close(&self) -> Result<()> {
        let _operation = self.operation.enter()?;
        if self.state.lock().unwrap().closed {
            return Ok(());
        }
        for id in self.profiles() {
            self.disconnect_inner(&id)?;
        }
        self.state.lock().unwrap().closed = true;
        Ok(())
    }
}

#[derive(Clone)]
pub struct Emitter {
    manager: Weak<Manager>,
    lease: Arc<Lease>,
}
impl Emitter {
    /// The host callback may execute on a provider-owned thread. Mark its
    /// dynamic extent so a lifecycle call cannot wait on the very operation
    /// that is waiting for this synchronous provider callback to finish.
    pub fn dispatch<T>(&self, callback: impl FnOnce() -> T) -> T {
        struct Exit;
        impl Drop for Exit {
            fn drop(&mut self) {
                CALLBACK_DEPTH.set(CALLBACK_DEPTH.get() - 1);
            }
        }
        CALLBACK_DEPTH.set(CALLBACK_DEPTH.get() + 1);
        let _exit = Exit;
        callback()
    }
    pub fn connection_id(&self) -> &str {
        &self.lease.id
    }
    pub fn accept(&self, kind: &str, detail: Option<&str>) -> Option<u64> {
        if kind.len() > 128 || detail.is_some_and(|v| v.len() > MAX_TEXT_BYTES) {
            return None;
        }
        let manager = self.manager.upgrade()?;
        let mut state = manager.state.lock().unwrap();
        if state.closed
            || !self.lease.live.load(Ordering::Acquire)
            || state
                .active
                .as_ref()
                .is_none_or(|a| !Arc::ptr_eq(&a.lease, &self.lease))
        {
            return None;
        }
        let Ok(runtime) = state.runtime(&self.lease.id) else {
            return None;
        };
        let channel = &mut runtime.market;
        if channel.state == ConnectionStatus::Disconnected {
            return None;
        }
        if kind == "reconnecting" && channel.state != ConnectionStatus::Reconnecting {
            channel.generation = channel.generation.checked_add(1)?;
        }
        let state = match kind {
            "connected" => Some((ConnectionStatus::Ready, "行情已登录")),
            "connecting" => Some((ConnectionStatus::Authenticating, "正在登录行情")),
            "reconnecting" => Some((ConnectionStatus::Reconnecting, "行情重连中")),
            "error" => Some((ConnectionStatus::Error, "行情连接失败")),
            _ => None,
        };
        if let Some((status, message)) = state {
            channel.state = status;
            channel.detail = detail.filter(|v| !v.is_empty()).unwrap_or(message).into();
        }
        Some(channel.generation)
    }
}
pub struct ReadTicket {
    lease: Arc<Lease>,
    caller: Arc<dyn Cancellation>,
    request: ReadRequest,
    finished: AtomicBool,
}
impl ReadTicket {
    pub fn is_cancelled(&self) -> bool {
        self.finished.load(Ordering::Acquire)
            || !self.lease.live.load(Ordering::Acquire)
            || self.caller.is_cancelled()
    }
}
fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}

fn read_file(path: &Path) -> std::io::Result<Vec<u8>> {
    let mut raw = Vec::new();
    std::fs::File::open(path)?
        .take(MAX_STATE_BYTES as u64 + 1)
        .read_to_end(&mut raw)?;
    if raw.len() > MAX_STATE_BYTES {
        return Err(std::io::Error::other(
            "Connection configuration exceeds budget",
        ));
    }
    Ok(raw)
}
/// Bound the typed Rust port too, before any metadata clone or serialization.
pub fn bounded_value(value: &Value, bytes: usize) -> Result<()> {
    fn visit(value: &Value, depth: usize, nodes: &mut usize) -> Result<()> {
        *nodes += 1;
        if depth > 64 || *nodes > 500_000 {
            return Err(Error::invalid("来源响应超出结构预算"));
        }
        match value {
            Value::Array(values) => {
                for value in values {
                    visit(value, depth + 1, nodes)?;
                }
            }
            Value::Object(values) => {
                for value in values.values() {
                    visit(value, depth + 1, nodes)?;
                }
            }
            _ => {}
        }
        Ok(())
    }
    visit(value, 0, &mut 0)?;
    struct Budget(usize);
    impl std::io::Write for Budget {
        fn write(&mut self, value: &[u8]) -> std::io::Result<usize> {
            if value.len() > self.0 {
                return Err(std::io::Error::other("budget"));
            }
            self.0 -= value.len();
            Ok(value.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    serde_json::to_writer(Budget(bytes), value).map_err(|_| Error::invalid("来源响应超出字节预算"))
}
