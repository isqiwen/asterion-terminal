//! Immutable artifacts. Publication never replaces an existing object; all reads
//! and writes reject links and stay directory-relative. Archive objects are
//! content-addressed directories; `ArtifactStore` holds single write-once files
//! whose meaning, naming and consumers belong to the owning domain.
use crate::archive::{CheckedArchive, Error, Policy, Result};
use crate::directories::StagedDirectory;
use crate::files::{self, ReadRoot};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::Path;

fn valid_digest(digest: &str) -> Result<()> {
    if digest.len() != 64
        || !digest
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(Error::Invalid("Invalid immutable object digest"));
    }
    Ok(())
}
fn verify(root: &Path, expected: &CheckedArchive) -> Result<()> {
    let digest = expected.digest();
    let bound = expected
        .original
        .len()
        .max(expected.files.values().map(Vec::len).max().unwrap_or(0))
        .max(1);
    let grant = ReadRoot::new(
        root,
        bound as u64,
        expected.files.len().saturating_mul(65) + 1,
    )?;
    if grant.read(&format!("{digest}/.archive"))? != expected.original {
        return Err(Error::Invalid("Immutable archive content has changed"));
    }
    let actual: BTreeSet<_> = grant.scan(digest, "")?.into_iter().collect();
    let mut required: BTreeSet<_> = expected
        .names()
        .into_iter()
        .map(|name| format!("{digest}/{name}"))
        .collect();
    required.insert(format!("{digest}/.archive"));
    if actual != required {
        return Err(Error::Invalid("Immutable object file list has changed"));
    }
    for (name, content) in &expected.files {
        if grant.read(&format!("{digest}/{name}"))? != *content {
            return Err(Error::Invalid("Immutable object content has changed"));
        }
    }
    Ok(())
}
pub fn load(root: &Path, digest: &str, policy: &Policy) -> Result<CheckedArchive> {
    valid_digest(digest)?;
    policy.validate()?;
    let grant = ReadRoot::new(root, policy.compressed_bytes, policy.files * 65 + 1)?;
    let content = grant.read(&format!("{digest}/.archive"))?;
    let archive = CheckedArchive::check(&content, policy)?;
    if archive.digest() != digest {
        return Err(Error::Invalid("Immutable archive digest has changed"));
    }
    verify(root, &archive)?;
    Ok(archive)
}

/// Publish the validated bytes using the shared owned staging mechanism.
pub fn publish(root: &Path, archive: &CheckedArchive) -> Result<()> {
    let destination = root.join(archive.digest());
    let mut stage = match StagedDirectory::new(&destination, archive.files.len() * 65 + 1) {
        Ok(stage) => stage,
        Err(files::Error::Io(error)) if error.kind() == std::io::ErrorKind::AlreadyExists => {
            return verify(root, archive);
        }
        Err(error) => return Err(error.into()),
    };
    for (name, content) in &archive.files {
        stage.create_file(name)?.write_all(content)?;
    }
    stage
        .create_file(".archive")?
        .write_all(&archive.original)?;
    match stage.commit() {
        Ok(()) => {}
        Err(files::Error::Io(error)) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
        Err(error) => return Err(error.into()),
    }
    verify(root, archive)
}

/// A verifiable reference to write-once bytes below one granted artifact root.
/// The digest proves content identity only, not source authenticity.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ArtifactRef {
    pub name: String,
    pub sha256: String,
    pub bytes: u64,
}

fn sha256(content: &[u8]) -> String {
    hex::encode(Sha256::digest(content))
}

/// Host-created write-once file grant. An existing name is never replaced:
/// identical bytes make a retry idempotent, different bytes are rejected.
/// A reader grant (for example over a restored copy) only verifies and reads.
#[derive(Debug)]
pub struct ArtifactStore {
    directory: File,
    max_bytes: u64,
    writable: bool,
}
impl ArtifactStore {
    pub fn new(root: &Path, max_bytes: u64) -> files::Result<Self> {
        Self::open(root, max_bytes, true)
    }
    pub fn reader(root: &Path, max_bytes: u64) -> files::Result<Self> {
        Self::open(root, max_bytes, false)
    }
    fn open(root: &Path, max_bytes: u64, writable: bool) -> files::Result<Self> {
        if max_bytes == 0 || max_bytes == u64::MAX {
            return Err(files::Error::Invalid(
                "Artifact size limit must be positive and bounded",
            ));
        }
        Ok(Self {
            directory: files::directory(root)?,
            max_bytes,
            writable,
        })
    }
    pub fn put(&self, name: &str, content: &[u8]) -> files::Result<ArtifactRef> {
        if !self.writable {
            return Err(files::Error::Invalid("Artifact grant is read-only"));
        }
        let reference = ArtifactRef {
            name: name.into(),
            sha256: sha256(content),
            bytes: content.len() as u64,
        };
        if reference.bytes > self.max_bytes {
            return Err(files::Error::Invalid("Artifact exceeds granted size limit"));
        }
        let parts = files::path_parts(name)?;
        let (leaf, parents) = parts.split_last().expect("path has one part");
        let parent = files::ensure_directories(&self.directory, parents)?;
        match files::atomic_write_at(&parent, Path::new(leaf), content, false) {
            Ok(()) => Ok(reference),
            Err(files::Error::Io(error)) if error.kind() == io::ErrorKind::AlreadyExists => {
                match self.read(name, &reference.sha256, Some(reference.bytes)) {
                    Ok(_) => Ok(reference),
                    Err(files::Error::Invalid(_)) => Err(files::Error::Invalid(
                        "Immutable artifact already exists with different content",
                    )),
                    Err(error) => Err(error),
                }
            }
            Err(error) => Err(error),
        }
    }
    /// Store under `<directory>/<sha256><suffix>`, so a changed retry cannot
    /// collide with bytes an earlier transaction may already reference.
    pub fn put_addressed(
        &self,
        directory: &str,
        suffix: &str,
        content: &[u8],
    ) -> files::Result<ArtifactRef> {
        if suffix.len() > 32 || suffix.contains(['/', '\\', '\0']) {
            return Err(files::Error::Invalid("Invalid artifact suffix"));
        }
        files::path_parts(directory)?;
        self.put(&format!("{directory}/{}{suffix}", sha256(content)), content)
    }
    fn locate(
        &self,
        name: &str,
        sha256_hex: &str,
        bytes: Option<u64>,
    ) -> files::Result<(File, u64)> {
        if sha256_hex.len() != 64
            || !sha256_hex
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
        {
            return Err(files::Error::Invalid("Invalid SHA-256 checksum"));
        }
        let limit = bytes.unwrap_or(self.max_bytes);
        if limit > self.max_bytes {
            return Err(files::Error::Invalid("Artifact exceeds granted size limit"));
        }
        let parts = files::path_parts(name)?;
        let (leaf, parents) = parts.split_last().expect("path has one part");
        let mut parent = self.directory.try_clone()?;
        for part in parents {
            parent = files::child_directory(&parent, Path::new(part))?;
        }
        let file = files::open_regular(&parent, Path::new(leaf))?;
        let size = file.metadata()?.len();
        if size > limit || bytes.is_some_and(|expected| expected != size) {
            return Err(files::Error::Invalid("Artifact size mismatch"));
        }
        Ok((file, limit))
    }
    /// Read complete bytes and verify digest and, when recorded, the size.
    pub fn read(&self, name: &str, sha256_hex: &str, bytes: Option<u64>) -> files::Result<Vec<u8>> {
        let (file, limit) = self.locate(name, sha256_hex, bytes)?;
        let mut content = Vec::new();
        file.take(limit + 1).read_to_end(&mut content)?;
        if content.len() as u64 > limit || bytes.is_some_and(|size| size != content.len() as u64) {
            return Err(files::Error::Invalid("Artifact size mismatch"));
        }
        if sha256(&content) != sha256_hex {
            return Err(files::Error::Invalid("Artifact checksum mismatch"));
        }
        Ok(content)
    }
    /// Stream-verify recorded bytes without retaining the content.
    pub fn verify(&self, name: &str, sha256_hex: &str, bytes: Option<u64>) -> files::Result<()> {
        let (file, limit) = self.locate(name, sha256_hex, bytes)?;
        let mut digest = Sha256::new();
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut total = 0_u64;
        let mut reader = file.take(limit + 1);
        loop {
            let count = reader.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            total += count as u64;
            digest.update(&buffer[..count]);
        }
        if total > limit || bytes.is_some_and(|size| size != total) {
            return Err(files::Error::Invalid("Artifact size mismatch"));
        }
        if hex::encode(digest.finalize()) != sha256_hex {
            return Err(files::Error::Invalid("Artifact checksum mismatch"));
        }
        Ok(())
    }
}
