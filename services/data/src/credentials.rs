//! Encrypted, fingerprinted configuration snapshots of data sources. Files are
//! local to the service (`<data root>/.credentials/configurations/<ref>.enc`,
//! directories 0700, files 0600) and never enter published artifacts; the
//! reference binds the owner, schema, revision, values and this machine's key.
use asterion_kernel::files::atomic_write;
use asterion_kernel::secrets::Secrets;
use regex::Regex;
use serde_json::{Map, Value, json};
use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
use std::path::{Path, PathBuf};

const ENCRYPTION_SCOPE: &[u8] = b"asterion.provider.credentials.v1";
const FINGERPRINT_SCOPE: &[u8] = b"configuration-snapshot-v1";
const UNREADABLE: &str = "固定配置无法读取或校验失败，请检查本机配置与运行密钥";

pub struct Credentials {
    root: PathBuf,
    secrets: Secrets,
}

fn private_directory(path: &Path) -> Result<(), String> {
    let failed = |_| "无法写入本机配置目录".to_string();
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(failed)?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).map_err(failed)
}

/// Canonical JSON: sorted keys, compact separators, unescaped non-ASCII text.
fn canonical(
    owner: &str,
    schema_version: u32,
    values: &Map<String, Value>,
    revision: i64,
) -> Vec<u8> {
    serde_json::to_vec(&json!({
        "provider": owner,
        "revision": revision,
        "schema_version": schema_version,
        "values": values,
    }))
    .expect("serializable configuration")
}

impl Credentials {
    pub fn new(master: &str, data_root: &Path) -> Self {
        Self {
            root: data_root.join(".credentials"),
            secrets: Secrets::new(master.as_bytes(), ENCRYPTION_SCOPE, FINGERPRINT_SCOPE),
        }
    }

    fn path(&self, reference: &str) -> PathBuf {
        self.root
            .join("configurations")
            .join(format!("{reference}.enc"))
    }

    /// Store an immutable snapshot and return its reference; an existing
    /// snapshot of the same reference must hold the same values.
    pub fn freeze(
        &self,
        owner: &str,
        schema_version: u32,
        values: &Map<String, Value>,
        revision: i64,
    ) -> Result<String, String> {
        if !Regex::new(r"^[a-z][a-z0-9_]{0,40}$")
            .expect("pattern")
            .is_match(owner)
        {
            return Err("Invalid provider identifier".into());
        }
        let content = canonical(owner, schema_version, values, revision);
        let reference = self.secrets.fingerprint(&content);
        private_directory(&self.root)?;
        private_directory(&self.root.join("configurations"))?;
        let path = self.path(&reference);
        if path.exists() {
            if &self.read(&reference, owner, schema_version, revision)? != values {
                return Err("配置快照校验失败".into());
            }
        } else {
            atomic_write(&path, &self.secrets.encrypt(&content), true)
                .map_err(|_| "无法写入本机配置快照".to_string())?;
        }
        Ok(reference)
    }

    /// The values of a snapshot, verified against its reference and owner.
    pub fn read(
        &self,
        reference: &str,
        owner: &str,
        schema_version: u32,
        revision: i64,
    ) -> Result<Map<String, Value>, String> {
        if !Regex::new(r"^[0-9a-f]{64}$")
            .expect("pattern")
            .is_match(reference)
        {
            return Err("配置快照引用无效".into());
        }
        let content = std::fs::read(self.path(reference))
            .ok()
            .and_then(|sealed| self.secrets.decrypt(&sealed).ok())
            .ok_or(UNREADABLE)?;
        let value: Value = serde_json::from_slice(&content).map_err(|_| UNREADABLE)?;
        let number = |name: &str| value.get(name).and_then(Value::as_f64);
        match value.get("values") {
            Some(Value::Object(values))
                if self.secrets.fingerprint(&content) == reference
                    && value.get("provider").and_then(Value::as_str) == Some(owner)
                    && number("schema_version") == Some(f64::from(schema_version))
                    && number("revision") == Some(revision as f64) =>
            {
                Ok(values.clone())
            }
            _ => Err(UNREADABLE.into()),
        }
    }

    /// Whether sealed bytes open with this machine's key (backup checks).
    pub fn opens(&self, sealed: &[u8]) -> bool {
        self.secrets.decrypt(sealed).is_ok()
    }
}
