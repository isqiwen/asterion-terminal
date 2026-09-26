//! Fixed host environment selection. Service, database and backup semantics stay in
//! the product adapter; this module alone owns locking and selection commits.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::os::unix::fs::DirBuilderExt;
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, Instant};

use crate::files::{FileLock, ReadRoot, atomic_write};

const MANIFEST: &str = "environment.json";
const RECORD_LIMIT: u64 = 64 * 1024;

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Selection {
    pub active: PathBuf,
    #[serde(deserialize_with = "nullable")]
    pub previous: Option<PathBuf>,
    #[serde(deserialize_with = "nullable")]
    pub pending: Option<PathBuf>,
    #[serde(deserialize_with = "nullable")]
    pub protection_backup: Option<PathBuf>,
}

fn nullable<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<PathBuf>, D::Error> {
    Option::<PathBuf>::deserialize(deserializer)
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Layout {
    pub required_files: Vec<PathBuf>,
    pub required_directories: Vec<PathBuf>,
    pub offline_locks: Vec<PathBuf>,
}

impl Layout {
    fn check(&self) -> Result<(), String> {
        for path in self
            .required_files
            .iter()
            .chain(&self.required_directories)
            .chain(&self.offline_locks)
        {
            if path.as_os_str().is_empty()
                || !path
                    .components()
                    .all(|part| matches!(part, Component::Normal(_)))
            {
                return Err("环境布局只接受相对普通路径".into());
            }
        }
        if self.required_files.is_empty() || self.offline_locks.len() > 16 {
            return Err("环境布局无效".into());
        }
        Ok(())
    }
    fn complete(&self, state: &Path) -> bool {
        self.required_files
            .iter()
            .all(|path| state.join(path).is_file())
            && self
                .required_directories
                .iter()
                .all(|path| state.join(path).is_dir())
    }
}

#[derive(Debug, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case")]
pub enum Action<'a> {
    ValidateTarget {
        state: &'a Path,
        rollback: bool,
    },
    Stop {
        state: &'a Path,
    },
    Start {
        state: &'a Path,
    },
    Protect {
        state: &'a Path,
        destination: &'a Path,
    },
    Verify {
        state: &'a Path,
    },
}

#[derive(Debug)]
pub struct AdapterError<E> {
    pub error: E,
    /// Interrupted processes retain the journal for recovery on the next launch.
    pub interrupted: bool,
}

#[derive(Debug)]
pub enum Error<E> {
    Mechanism(String),
    Adapter(AdapterError<E>),
}
impl<E> From<String> for Error<E> {
    fn from(value: String) -> Self {
        Self::Mechanism(value)
    }
}

/// The lease is the only mutation entry point. Dropping it releases the OS lock.
pub struct Lease {
    host: PathBuf,
    backup_directory: PathBuf,
    layout: Layout,
    _lock: FileLock,
}

impl Lease {
    pub fn acquire(
        host: &Path,
        backup_directory: &Path,
        layout: Layout,
        wait: bool,
    ) -> Result<Self, String> {
        layout.check()?;
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(host)
            .map_err(|_| "环境宿主目录不可用")?;
        let host = host.canonicalize().map_err(|_| "环境宿主目录不可用")?;
        if !backup_directory.is_absolute() {
            return Err("保护备份目录必须是绝对路径".into());
        }
        let deadline = Instant::now() + Duration::from_secs(if wait { 60 } else { 0 });
        let lock = loop {
            match FileLock::acquire(&host.join("maintenance.lock"), false) {
                Ok(lock) => break lock,
                Err(crate::files::Error::Io(error))
                    if error.kind() == std::io::ErrorKind::WouldBlock =>
                {
                    if Instant::now() >= deadline {
                        return Err("本机维护正在进行，请稍后重试".into());
                    }
                    std::thread::sleep(Duration::from_millis(100));
                }
                Err(_) => return Err("环境维护锁不可用".into()),
            }
        };
        Ok(Self {
            host,
            backup_directory: backup_directory.to_path_buf(),
            layout,
            _lock: lock,
        })
    }

    pub fn read(&self) -> Result<Selection, String> {
        let path = self.host.join(MANIFEST);
        let metadata = match fs::symlink_metadata(&path) {
            Ok(value) => value,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return Ok(Selection {
                    active: self.host.clone(),
                    previous: None,
                    pending: None,
                    protection_backup: None,
                });
            }
            Err(_) => return Err("环境记录不可读".into()),
        };
        if !metadata.is_file() || metadata.len() > RECORD_LIMIT {
            return Err("环境记录无效".into());
        }
        let value: Selection = serde_json::from_slice(
            &ReadRoot::new(&self.host, RECORD_LIMIT, 1)
                .and_then(|root| root.read(MANIFEST))
                .map_err(|_| "环境记录不可读")?,
        )
        .map_err(|_| "环境记录无效")?;
        self.check_selection(&value)?;
        Ok(value)
    }

    pub fn active(&self) -> Result<PathBuf, String> {
        let selected = self.read()?.active;
        if (selected != self.host || self.host.join(MANIFEST).exists())
            && !self.layout.complete(&selected)
        {
            return Err("当前环境目录不可用；请恢复目录后重试，禁止自动创建空环境".into());
        }
        Ok(selected)
    }

    fn check_selection(&self, selection: &Selection) -> Result<(), String> {
        for path in std::iter::once(&selection.active)
            .chain(selection.previous.iter())
            .chain(selection.pending.iter())
            .chain(selection.protection_backup.iter())
        {
            if !path.is_absolute()
                || path
                    .components()
                    .any(|part| matches!(part, Component::ParentDir | Component::CurDir))
            {
                return Err("环境记录无效".into());
            }
        }
        for state in selection.previous.iter().chain(selection.pending.iter()) {
            if state.starts_with(&selection.active) || selection.active.starts_with(state) {
                return Err("环境记录无效".into());
            }
        }
        Ok(())
    }

    fn write(&self, selection: &Selection) -> Result<(), String> {
        self.check_selection(selection)?;
        let bytes = serde_json::to_vec(selection).map_err(|_| "环境记录无效")?;
        atomic_write(&self.host.join(MANIFEST), &bytes, true)
            .map_err(|_| "环境记录不能持久化".into())
    }

    fn offline(&self, state: &Path) -> Result<Vec<FileLock>, String> {
        self.layout
            .offline_locks
            .iter()
            .map(|name| {
                FileLock::acquire(&state.join(name), false)
                    .map_err(|_| "后台服务或其他维护操作仍在运行，请先停止服务".into())
            })
            .collect()
    }

    pub fn recover<E>(
        &self,
        adapter: &mut impl FnMut(Action<'_>) -> Result<Value, AdapterError<E>>,
    ) -> Result<(), Error<E>> {
        let mut selection = self.read()?;
        let Some(target) = selection.pending.as_ref() else {
            return Ok(());
        };
        let source = self.active()?;
        adapter(Action::Stop { state: target }).map_err(Error::Adapter)?;
        adapter(Action::Stop { state: &source }).map_err(Error::Adapter)?;
        adapter(Action::Start { state: &source }).map_err(Error::Adapter)?;
        selection.pending = None;
        self.write(&selection)?;
        Ok(())
    }

    pub fn switch<E>(
        &self,
        target: Option<&Path>,
        adapter: &mut impl FnMut(Action<'_>) -> Result<Value, AdapterError<E>>,
    ) -> Result<Value, Error<E>> {
        self.recover(adapter)?;
        let mut selection = self.read()?;
        let source = self.active()?;
        let rollback = target.is_none();
        let requested = target
            .or(selection.previous.as_deref())
            .ok_or_else(|| "没有可回滚的环境".to_string())?;
        let target = requested
            .canonicalize()
            .map_err(|_| "目标不是完整恢复目录".to_string())?;
        if target.starts_with(&source) || source.starts_with(&target) {
            return Err("切换目标必须是独立目录".to_string().into());
        }
        if !self.layout.complete(&target) {
            return Err("目标不是完整恢复目录".to_string().into());
        }
        if !rollback && (target.starts_with(&self.host) || self.host.starts_with(&target)) {
            return Err("恢复目录必须位于宿主目录之外".to_string().into());
        }
        {
            let _offline = self.offline(&target)?;
            reject_links(&target)?;
            adapter(Action::ValidateTarget {
                state: &target,
                rollback,
            })
            .map_err(Error::Adapter)?;
        }
        let destination = self
            .backup_directory
            .join(format!("protection-{}.zip", uuid::Uuid::new_v4().simple()));
        selection.pending = Some(target.clone());
        self.write(&selection)?;
        let outcome = (|| {
            adapter(Action::Stop { state: &source }).map_err(Error::Adapter)?;
            adapter(Action::Protect {
                state: &source,
                destination: &destination,
            })
            .map_err(Error::Adapter)?;
            selection.protection_backup = Some(destination);
            self.write(&selection)?;
            let verification = {
                let _offline = self.offline(&target)?;
                reject_links(&target)?;
                adapter(Action::Verify { state: &target }).map_err(Error::Adapter)?
            };
            adapter(Action::Start { state: &target }).map_err(Error::Adapter)?;
            selection.active = target;
            selection.previous = Some(source);
            selection.pending = None;
            self.write(&selection)?;
            Ok(serde_json::json!({
                "status": "active", "active": selection.active, "previous": selection.previous,
                "pending": selection.pending, "protection_backup": selection.protection_backup,
                "verification": verification
            }))
        })();
        match outcome {
            Err(Error::Adapter(error)) if error.interrupted => Err(Error::Adapter(error)),
            Err(error) => {
                self.recover(adapter)?;
                Err(error)
            }
            Ok(value) => Ok(value),
        }
    }
}

fn reject_links(root: &Path) -> Result<(), String> {
    let mut pending = vec![(root.to_path_buf(), 0)];
    let mut count = 0_u64;
    while let Some((directory, depth)) = pending.pop() {
        if depth > 128 {
            return Err("环境目录层级超出上限".into());
        }
        for entry in fs::read_dir(directory).map_err(|_| "环境目录不可读")? {
            let entry = entry.map_err(|_| "环境目录不可读")?;
            let kind = entry.file_type().map_err(|_| "环境目录不可读")?;
            count += 1;
            if count > 1_000_000 {
                return Err("环境文件数量超出上限".into());
            }
            if kind.is_symlink() {
                return Err("恢复目录不能包含符号链接".into());
            }
            if kind.is_dir() {
                pending.push((entry.path(), depth + 1));
            } else if !kind.is_file() {
                return Err("恢复目录不能包含特殊文件".into());
            }
        }
    }
    Ok(())
}
