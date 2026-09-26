//! First-run runtime provisioning. This module owns no business features or user databases.
use flate2::read::GzDecoder;
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};

type Result<T> = std::result::Result<T, String>;
fn err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

struct InstallationLease(File);
impl InstallationLease {
    fn finish(self) -> Result<()> {
        // Called only after successful completion, once all installation commands
        // have exited. An unrelated concurrent fork can still hold a CLOEXEC
        // duplicate, so closing our descriptor alone need not release the flock.
        // Do not unlock in Drop: on failure or parent exit, a running installer
        // intentionally retains this same lease through its stdin descriptor.
        FileExt::unlock(&self.0).map_err(err)
    }
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Artifact {
    pub url: String,
    pub sha256: String,
    pub size: u64,
}
#[derive(Deserialize)]
#[serde(tag = "source", rename_all = "snake_case", deny_unknown_fields)]
pub enum Postgres {
    System {
        root: PathBuf,
        executable: String,
    },
    Homebrew {
        formula: String,
    },
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub schema: u32,
    pub platform: String,
    pub architecture: String,
    pub uv: Artifact,
    pub python: Artifact,
    pub postgres: Postgres,
    pub uv_executable: String,
    pub python_version: String,
    pub wheel: String,
    pub wheel_sha256: String,
    pub requirements_sha256: String,
}
#[derive(Clone, Serialize, Default)]
pub struct Progress {
    pub running: bool,
    pub ready: bool,
    pub step: usize,
    pub downloaded: u64,
    pub network: Option<crate::network_stats::NetworkSnapshot>,
    pub dependencies: DependencyProgress,
    pub total: Option<u64>,
    pub error: String,
    pub directory: String,
}
#[derive(Clone, Serialize, Default)]
pub struct DependencyProgress {
    pub total: Option<usize>,
    pub installed: usize,
    pub phase: String,
    pub current: String,
    #[serde(skip)]
    active: std::collections::BTreeSet<String>,
}
impl DependencyProgress {
    fn observe(&mut self, line: &str) {
        let line = line.trim();
        let count = |prefix: &str| {
            line.strip_prefix(prefix)
                .and_then(|v| v.split_whitespace().next())
                .and_then(|v| v.parse::<usize>().ok())
        };
        if let Some(n) = count("Resolved ") {
            self.total = Some(n);
            self.phase = "preparing".into();
        } else if let Some(name) = line.strip_prefix("Downloading ") {
            self.active
                .insert(name.split_whitespace().next().unwrap_or("").to_owned());
            self.phase = "downloading".into();
            self.current = self.active.iter().cloned().collect::<Vec<_>>().join(", ");
        } else if let Some(name) = line.strip_prefix("Downloaded ") {
            self.active.remove(name.trim());
            self.current = self.active.iter().cloned().collect::<Vec<_>>().join(", ");
            if self.active.is_empty() {
                self.phase = "preparing".into();
            }
        } else if line.starts_with("Prepared ") {
            self.phase = "installing".into();
            self.current.clear();
        } else if let Some(n) = count("Installed ") {
            self.installed = n;
            self.phase = "installed".into();
            self.current.clear();
        } else if let Some(package) = line.strip_prefix("+ ") {
            self.current = package.split_whitespace().next().unwrap_or("").to_owned();
        }
    }
}
pub struct Setup {
    pub manifest: Manifest,
    pub id: String,
    pub bundle: PathBuf,
    pub root: PathBuf,
}
fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn hash_file(path: &Path) -> Result<String> {
    let mut file = File::open(path).map_err(err)?;
    let mut hash = Sha256::new();
    let mut buffer = [0; 65536];
    loop {
        let n = file.read(&mut buffer).map_err(err)?;
        if n == 0 {
            break;
        }
        hash.update(&buffer[..n]);
    }
    Ok(format!("{:x}", hash.finalize()))
}
fn safe_relative(value: &str) -> bool {
    !value.is_empty()
        && Path::new(value)
            .components()
            .all(|p| matches!(p, std::path::Component::Normal(_)))
}
fn homebrew_prefix() -> PathBuf {
    PathBuf::from(if cfg!(target_arch = "aarch64") { "/opt/homebrew" } else { "/usr/local" })
}

fn install_homebrew_postgres(
    brew: &Path, formula: &str, root: &Path, lease: &File, mut started: impl FnMut(),
) -> Result<()> {
    if !brew.is_file() {
        return Err("需要先安装 Homebrew：请访问 https://brew.sh 按官方步骤完成安装，然后点击“重试设置”。星枢将通过 Homebrew 安装 PostgreSQL 17，不会启动系统默认数据库服务。".into());
    }
    started();
    run(
        Command::new(brew)
            .args(["install", formula])
            .env("PATH", format!("{}:/usr/bin:/bin:/usr/sbin:/sbin", brew.parent().unwrap().display()))
            .env("NONINTERACTIVE", "1")
            .env("HOMEBREW_NO_AUTO_UPDATE", "1")
            .env("HOMEBREW_NO_INSTALL_CLEANUP", "1")
            .env("HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK", "1"),
        root, lease, 1800,
    ).map_err(|error| format!("Homebrew 安装 PostgreSQL 17 失败。可在终端运行 brew install postgresql@17 排查后重试。{error}"))
}

impl Setup {
    pub fn load(bundle: &Path, state: &Path) -> Result<Self> {
        let bundle = bundle.canonicalize().map_err(err)?;
        let state = std::env::current_dir().map_err(err)?.join(state);
        let bytes =
            fs::read(bundle.join("manifest.json")).map_err(|e| format!("安装清单不可用：{e}"))?;
        let manifest: Manifest =
            serde_json::from_slice(&bytes).map_err(|e| format!("不支持的安装清单：{e}"))?;
        if manifest.schema != 1
            || manifest.platform != std::env::consts::OS
            || manifest.architecture != std::env::consts::ARCH
        {
            return Err("安装清单版本或平台不受支持".into());
        }
        if !safe_relative(&manifest.wheel) || !safe_relative(&manifest.uv_executable) {
            return Err("安装清单包含非法路径".into());
        }
        match &manifest.postgres {
            Postgres::System { root, executable } => {
                if manifest.platform != "linux"
                    || root != Path::new("/usr")
                    || executable != "lib/postgresql/17/bin/postgres"
                {
                    return Err("不支持的系统数据库配置".into());
                }
            }
            Postgres::Homebrew { formula } => {
                if manifest.platform != "macos" || formula != "postgresql@17" {
                    return Err("不支持的 Homebrew 数据库配置".into());
                }
            }
        }
        for a in [&manifest.uv, &manifest.python] {
            let url = reqwest::Url::parse(&a.url).map_err(err)?;
            if url.scheme() != "https"
                || !url.username().is_empty()
                || url.password().is_some()
                || a.size == 0
                || a.sha256.len() != 64
                || !a.sha256.bytes().all(|c| c.is_ascii_hexdigit())
            {
                return Err("下载地址、大小或校验值无效".into());
            }
        }
        if hash_file(&bundle.join(&manifest.wheel))? != manifest.wheel_sha256
            || hash_file(&bundle.join("requirements.txt"))? != manifest.requirements_sha256
        {
            return Err("安装文件校验失败，请重新下载安装包".into());
        }
        let id = digest(&bytes);
        let setup = Self {
            root: state.join("runtime"),
            manifest,
            id,
            bundle: bundle.to_owned(),
        };
        setup.installed_id()?;
        Ok(setup)
    }
    fn installed_id(&self) -> Result<Option<String>> {
        let path = self.root.join("ready");
        if !path.exists() {
            return Ok(None);
        }
        let id = fs::read_to_string(path).map_err(err)?;
        if id.len() != 64
            || !id.bytes().all(|c| c.is_ascii_hexdigit())
            || !self.python().is_file()
        {
            return Err("已安装运行环境不完整或标记不受支持；保留原文件，请检查运行目录".into());
        }
        Ok(Some(id))
    }
    pub fn runtime_lease(&self) -> Result<File> {
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(self.root.parent().unwrap().join("setup.lock"))
            .map_err(err)?;
        FileExt::try_lock_shared(&lock)
            .map_err(|_| "运行环境正在更新，请等待安装完成".to_string())?;
        if !self.ready() {
            return Err("请完成本次安装，更新本机运行环境".into());
        }
        self.check_postgres()?;
        Ok(lock)
    }
    fn replace_installed_runtime(&self, lock: &File) -> Result<()> {
        let installed = self.installed_id()?;
        if installed.as_ref() == Some(&self.id) {
            return Ok(());
        }
        let state = self.root.parent().unwrap();
        // Stop the service through the current lifecycle contract before touching executables.
        // The exclusive setup lease prevents other desktop calls from starting it again.
        if installed.is_some()
            && (state.join("desktop.json").exists() || state.join("environment.json").exists())
        {
            run(
                self.command(&self.python())
                    .args(["-I", "-m", "asterion.runtime.cli", "desktop-stop", "--state"])
                    .arg(state)
                    .arg("--pg-root")
                    .arg(self.postgres_root()),
                &self.root,
                lock,
                120,
            )?;
        }
        // Remove the receipt first so interruption cannot expose a partly replaced runtime.
        if installed.is_some() {
            fs::remove_file(self.root.join("ready")).map_err(err)?;
        }
        for directory in ["environment", "interpreter", "installer"] {
            let path = self.root.join(directory);
            if path.exists() {
                fs::remove_dir_all(path).map_err(err)?;
            }
        }
        Ok(())
    }
    pub fn python(&self) -> PathBuf {
        self.root.join("environment/bin/python")
    }
    pub fn ready(&self) -> bool {
        fs::read_to_string(self.root.join("ready")).is_ok_and(|v| v == self.id)
            && self.python().is_file()
            && self.postgres_executable().is_file()
    }
    pub fn postgres_root(&self) -> PathBuf {
        match &self.manifest.postgres {
            Postgres::System { root, .. } => root.clone(),
            Postgres::Homebrew { formula } => homebrew_prefix().join("opt").join(formula),
        }
    }
    fn postgres_executable(&self) -> PathBuf {
        let executable = match &self.manifest.postgres {
            Postgres::System { executable, .. } => executable.as_str(),
            Postgres::Homebrew { .. } => "bin/postgres",
        };
        self.postgres_root().join(executable)
    }
    fn check_postgres(&self) -> Result<()> {
        let hint = match self.manifest.postgres {
            Postgres::Homebrew { .. } => "请通过 Homebrew 安装或修复：brew install postgresql@17",
            Postgres::System { .. } => "请通过系统包管理器安装或修复 postgresql-17",
        };
        let executable = self.postgres_executable();
        for tool in ["postgres", "initdb", "pg_ctl", "pg_dump", "pg_restore"] {
            if !executable.parent().unwrap().join(tool).is_file() {
                return Err(format!("PostgreSQL 17 不完整（缺少 {tool}）。{hint}"));
            }
        }
        let output = Command::new(self.postgres_executable())
            .arg("--version")
            .output()
            .map_err(|e| {
                format!("PostgreSQL 17 不可用。{hint}：{e}")
            })?;
        if !output.status.success()
            || !String::from_utf8_lossy(&output.stdout).contains("(PostgreSQL) 17.")
        {
            return Err(format!("需要 PostgreSQL 17。{hint}"));
        }
        Ok(())
    }

    fn ensure_postgres(&self, lease: &File, mut report: impl FnMut(Progress)) -> Result<()> {
        if self.check_postgres().is_ok() {
            return Ok(());
        }
        if let Postgres::Homebrew { formula } = &self.manifest.postgres {
            let brew = homebrew_prefix().join("bin/brew");
            install_homebrew_postgres(&brew, formula, &self.root, lease, || {
                report(Progress {
                    running: true,
                    dependencies: DependencyProgress {
                        phase: "system".into(), current: "PostgreSQL 17 · Homebrew".into(),
                        ..Default::default()
                    },
                    directory: self.root.display().to_string(),
                    ..Default::default()
                });
            })?;
        }
        self.check_postgres()
    }

    fn command(&self, executable: &Path) -> Command {
        let mut cmd = Command::new(executable);
        for (key, _) in std::env::vars() {
            if key.starts_with("UV_")
                || key.starts_with("PIP_")
                || key.starts_with("PYTHON")
                || key.starts_with("ASTERION_")
            {
                cmd.env_remove(key);
            }
        }
        cmd.env("UV_NO_CONFIG", "1")
            .env("UV_PYTHON_DOWNLOADS", "never")
            .env("UV_CACHE_DIR", self.root.parent().unwrap().join("cache/uv"))
            .env("PYTHONNOUSERSITE", "1")
            .env("PYTHONDONTWRITEBYTECODE", "1")
            .current_dir(&self.root);
        cmd
    }
    pub fn install(&self, mut report: impl FnMut(Progress)) -> Result<()> {
        let parent = self.root.parent().unwrap();
        fs::create_dir_all(parent).map_err(err)?;
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(parent.join("setup.lock"))
            .map_err(err)?;
        lock.try_lock_exclusive()
            .map_err(|_| "另一个窗口正在安装，请等待完成后重试".to_string())?;
        let lease = InstallationLease(lock);
        let lock = &lease.0;
        fs::create_dir_all(&self.root).map_err(err)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&self.root, fs::Permissions::from_mode(0o700)).map_err(err)?;
        }
        // System dependency failure must not revoke an existing Python environment.
        self.ensure_postgres(lock, &mut report)?;
        if self.ready() {
            return lease.finish();
        }
        self.replace_installed_runtime(lock)?;
        let cache = parent.join("cache");
        fs::create_dir_all(&cache).map_err(err)?;
        let dependency_progress = std::cell::RefCell::new(DependencyProgress::default());
        let mut stage = |step, downloaded, total| {
            report(Progress {
                running: true,
                dependencies: dependency_progress.borrow().clone(),
                step,
                downloaded,
                total,
                directory: self.root.display().to_string(),
                ..Default::default()
            })
        };
        stage(0, 0, None);
        let uv_archive = download(&self.manifest.uv, &cache, |n, t| stage(0, n, t))?;
        stage(0, self.manifest.uv.size, Some(self.manifest.uv.size));
        unpack(&uv_archive, &self.root.join("installer"))?;
        let uv = self
            .root
            .join("installer")
            .join(&self.manifest.uv_executable);
        stage(1, 0, None);
        let python_archive = download(&self.manifest.python, &cache, |n, t| stage(1, n, t))?;
        stage(
            1,
            self.manifest.python.size,
            Some(self.manifest.python.size),
        );
        unpack(&python_archive, &self.root.join("interpreter"))?;
        let python = self.root.join("interpreter/python/bin/python3");
        stage(2, 0, None);
        let environment = self.root.join("environment");
        // Only this unpublished, manifest-owned environment is rebuilt after a failed attempt.
        if environment.exists() {
            fs::remove_dir_all(&environment).map_err(err)?;
        }
        run(
            self.command(&uv)
                .args(["venv", "--no-project", "--python"])
                .arg(&python)
                .arg(&environment),
            &self.root,
            lock,
            120,
        )?;
        stage(3, 0, None);
        dependency_progress.borrow_mut().phase = "resolving".into();
        stage(3, 0, None);
        run_observed(
            self.command(&uv)
                .args([
                    "pip",
                    "sync",
                    "--color",
                    "never",
                    "--require-hashes",
                    "--only-binary",
                    ":all:",
                    "--index-url",
                    "https://pypi.org/simple",
                    "--python",
                ])
                .arg(self.python())
                .arg(self.bundle.join("requirements.txt")),
            &self.root,
            lock,
            1800,
            |line| {
                dependency_progress.borrow_mut().observe(line);
                stage(3, 0, None);
            },
        )?;
        dependency_progress.borrow_mut().phase = "application".into();
        dependency_progress.borrow_mut().current = self.manifest.wheel.clone();
        stage(3, 0, None);
        run(
            self.command(&uv)
                .args(["pip", "install", "--no-deps", "--no-index", "--python"])
                .arg(self.python())
                .arg(self.bundle.join(&self.manifest.wheel)),
            &self.root,
            lock,
            120,
        )?;
        dependency_progress.borrow_mut().phase = "verifying".into();
        dependency_progress.borrow_mut().current.clear();
        stage(4, 0, Some(3));
        run(
            self.command(&uv)
                .args(["pip", "check", "--python"])
                .arg(self.python()),
            &self.root,
            lock,
            120,
        )?;
        stage(4, 1, Some(3));
        run(self.command(&self.python()).args(["-I", "-c", "import sys, asterion.runtime.cli, asterion_plugin_sdk, pyarrow, psycopg; from openctp_ctp import mdapi; assert '.'.join(map(str, sys.version_info[:3])) == sys.argv[1]"]).arg(&self.manifest.python_version), &self.root, lock, 120)?;
        stage(4, 2, Some(3));
        self.check_postgres()?;
        stage(4, 3, Some(3));
        fs::write(self.root.join("ready.tmp"), &self.id).map_err(err)?;
        fs::rename(self.root.join("ready.tmp"), self.root.join("ready")).map_err(err)?;
        lease.finish()
    }
}

fn run(cmd: &mut Command, root: &Path, lease: &File, seconds: u64) -> Result<()> {
    run_observed(cmd, root, lease, seconds, |_| {})
}

fn run_observed(
    cmd: &mut Command,
    root: &Path,
    lease: &File,
    seconds: u64,
    mut observe: impl FnMut(&str),
) -> Result<()> {
    let log_path = root.join("setup.log");
    let log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .map_err(err)?;
    let mut reader = File::open(&log_path).map_err(err)?;
    reader.seek(SeekFrom::End(0)).map_err(err)?;
    let mut pending = String::new();
    let mut child = cmd
        // The child retains the same flock through stdin if the desktop process exits.
        .stdin(lease.try_clone().map_err(err)?)
        .stdout(log.try_clone().map_err(err)?)
        .stderr(log)
        .spawn()
        .map_err(err)?;
    let start = Instant::now();
    loop {
        let status = child.try_wait().map_err(err)?;
        let mut bytes = Vec::new();
        reader.read_to_end(&mut bytes).map_err(err)?;
        pending.push_str(&String::from_utf8_lossy(&bytes));
        while let Some(end) = pending.find(['\n', '\r']) {
            observe(&pending[..end]);
            pending.drain(..=end);
        }
        if let Some(status) = status {
            if !pending.is_empty() {
                observe(&pending);
            }
            return if status.success() {
                Ok(())
            } else {
                Err(format!(
                    "安装命令失败（{status}）。检查网络和磁盘空间后重试。日志：{}",
                    log_path.display()
                ))
            };
        }
        if start.elapsed() > Duration::from_secs(seconds) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("安装超时，请重试。日志：{}", log_path.display()));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn download(
    a: &Artifact,
    cache: &Path,
    mut progress: impl FnMut(u64, Option<u64>),
) -> Result<PathBuf> {
    let path = cache.join(&a.sha256);
    if path.is_file()
        && hash_file(&path)? == a.sha256
        && fs::metadata(&path).map_err(err)?.len() == a.size
    {
        progress(a.size, Some(a.size));
        return Ok(path);
    }
    let client = reqwest::blocking::Client::builder()
        .https_only(true)
        .connect_timeout(Duration::from_secs(30))
        .timeout(Duration::from_secs(1800))
        .build()
        .map_err(err)?;
    let mut response = client
        .get(&a.url)
        .send()
        .map_err(|e| format!("下载失败，请检查网络后重试：{e}"))?
        .error_for_status()
        .map_err(err)?;
    let temporary = cache.join(format!("{}.part", a.sha256));
    let mut output = File::create(&temporary).map_err(err)?;
    let mut hash = Sha256::new();
    let mut buffer = [0; 65536];
    let mut count = 0;
    progress(0, Some(a.size));
    loop {
        let n = response.read(&mut buffer).map_err(err)?;
        if n == 0 {
            break;
        }
        count += n as u64;
        if count > a.size {
            return Err("下载大小与发布清单不符".into());
        }
        output.write_all(&buffer[..n]).map_err(err)?;
        hash.update(&buffer[..n]);
        progress(count, Some(a.size));
    }
    output.sync_all().map_err(err)?;
    if count != a.size || format!("{:x}", hash.finalize()) != a.sha256 {
        return Err("下载不完整或 SHA-256 校验失败，请重试".into());
    }
    fs::rename(&temporary, &path).map_err(err)?;
    Ok(path)
}

fn unpack(archive: &Path, destination: &Path) -> Result<()> {
    if destination.exists() {
        fs::remove_dir_all(destination).map_err(err)?;
    }
    fs::create_dir_all(destination).map_err(err)?;
    let mut archive = tar::Archive::new(GzDecoder::new(File::open(archive).map_err(err)?));
    archive.set_mask(0o022);
    // tar's unpack_in rejects traversal and links outside this private destination.
    for item in archive.entries().map_err(err)? {
        let mut item = item.map_err(err)?;
        if let Some(link) = item.link_name().map_err(err)? {
            let entry = item.path().map_err(err)?;
            let target = if item.header().entry_type().is_symlink() {
                entry.parent().unwrap_or(Path::new("")).join(link)
            } else {
                link.into_owned()
            };
            let mut depth = 0usize;
            for part in target.components() {
                match part {
                    std::path::Component::Normal(_) => depth += 1,
                    std::path::Component::CurDir => (),
                    std::path::Component::ParentDir if depth > 0 => depth -= 1,
                    _ => return Err("运行包包含越界链接".into()),
                }
            }
        }
        if !item.unpack_in(destination).map_err(err)? {
            return Err("运行包包含不安全路径".into());
        }
    }
    Ok(())
}

#[cfg(test)]
#[path = "runtime_setup_tests.rs"]
mod tests;
