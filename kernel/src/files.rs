//! Fixed, descriptor-relative file capabilities and durable host file publication.
//!
//! A read grant holds an opened directory, never a pathname that is re-resolved
//! for each call. Every descendant is opened with O_NOFOLLOW. This is a filesystem
//! capability for trusted host code, not a sandbox against same-user processes.
use nix::dir::Dir;
use nix::errno::Errno;
use nix::fcntl::{AtFlags, Flock, FlockArg, OFlag, open, openat, renameat};
use nix::sys::stat::{Mode, SFlag, fstatat, mkdirat};
use nix::unistd::{LinkatFlags, UnlinkatFlags, linkat, unlinkat};
use sha2::{Digest, Sha256};
use std::fs::{self, File, Metadata, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{FileExt, MetadataExt, OpenOptionsExt};
use std::path::{Component, Path, PathBuf};
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};

const DIRECTORY: OFlag = OFlag::O_RDONLY
    .union(OFlag::O_CLOEXEC)
    .union(OFlag::O_DIRECTORY)
    .union(OFlag::O_NOFOLLOW);
const REGULAR: OFlag = OFlag::O_RDONLY
    .union(OFlag::O_CLOEXEC)
    .union(OFlag::O_NOFOLLOW)
    .union(OFlag::O_NONBLOCK);
const SCAN_DEPTH: usize = 128;

#[derive(Debug)]
pub enum Error {
    Invalid(&'static str),
    Io(io::Error),
    /// The target became visible, but a subsequent cleanup or durability step
    /// failed. Callers must retain the published state instead of assuming rollback.
    Published(Box<Error>),
}
impl Error {
    pub fn published(&self) -> bool {
        matches!(self, Self::Published(_))
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Invalid(message) => f.write_str(message),
            Self::Io(error) => write!(f, "{error}"),
            Self::Published(error) => {
                write!(f, "File was published, but completion failed: {error}")
            }
        }
    }
}
impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Invalid(_) => None,
            Self::Io(error) => Some(error),
            Self::Published(error) => Some(error.as_ref()),
        }
    }
}
impl From<io::Error> for Error {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}
impl From<Errno> for Error {
    fn from(value: Errno) -> Self {
        if value == Errno::ELOOP {
            Self::Invalid("File links are not allowed")
        } else {
            Self::Io(io::Error::from_raw_os_error(value as i32))
        }
    }
}
pub type Result<T> = std::result::Result<T, Error>;

pub(crate) fn path_parts(name: &str) -> Result<Vec<&str>> {
    if name.is_empty()
        || name.contains(['\\', '\0'])
        || name.split('/').any(|part| matches!(part, "" | "." | ".."))
    {
        return Err(Error::Invalid("Invalid granted file path"));
    }
    Ok(name.split('/').collect())
}
pub(crate) fn child_directory(parent: &File, name: &Path) -> Result<File> {
    match openat(parent, name, DIRECTORY, Mode::empty()) {
        Ok(fd) => Ok(fd.into()),
        Err(error) => {
            // O_DIRECTORY can report ENOTDIR for a link. This check only chooses
            // the error message: access always uses the no-follow opened handle.
            if let Ok(stat) = fstatat(parent, name, AtFlags::AT_SYMLINK_NOFOLLOW)
                && SFlag::from_bits_truncate(stat.st_mode) == SFlag::S_IFLNK
            {
                return Err(Error::Invalid("File links are not allowed"));
            }
            Err(error.into())
        }
    }
}
pub(crate) fn directory(path: &Path) -> Result<File> {
    let base = if path.is_absolute() { "/" } else { "." };
    let mut current = File::from(open(base, DIRECTORY, Mode::empty())?);
    for part in path.components() {
        match part {
            Component::Normal(name) => current = child_directory(&current, Path::new(name))?,
            Component::RootDir | Component::CurDir => {}
            _ => return Err(Error::Invalid("Invalid root directory path")),
        }
    }
    Ok(current)
}
/// Open or create (0700) each descendant without following links; a created
/// entry is made durable by syncing its parent before it is used.
pub(crate) fn ensure_directories(root: &File, parts: &[&str]) -> Result<File> {
    let mut current = root.try_clone()?;
    for part in parts {
        let name = Path::new(part);
        current = match child_directory(&current, name) {
            Ok(child) => child,
            Err(Error::Io(error)) if error.kind() == io::ErrorKind::NotFound => {
                match mkdirat(&current, name, Mode::S_IRWXU) {
                    Ok(()) => current.sync_all()?,
                    Err(Errno::EEXIST) => {}
                    Err(error) => return Err(error.into()),
                }
                child_directory(&current, name)?
            }
            Err(error) => return Err(error),
        };
    }
    Ok(current)
}
pub(crate) fn open_regular(parent: &File, name: &Path) -> Result<File> {
    regular(parent, name, REGULAR)
}
fn regular(parent: &File, name: &Path, flags: OFlag) -> Result<File> {
    let file = File::from(openat(parent, name, flags, Mode::S_IRUSR | Mode::S_IWUSR)?);
    if !file.metadata()?.is_file() {
        return Err(Error::Invalid("A regular file is required"));
    }
    Ok(file)
}
fn parent(path: &Path) -> Result<(File, &Path)> {
    let name = path
        .file_name()
        .ok_or(Error::Invalid("A file name is required"))?;
    let parent = directory(path.parent().unwrap_or(Path::new(".")))?;
    Ok((parent, Path::new(name)))
}
fn digest(mut file: File) -> Result<String> {
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; 1024 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            return Ok(hex::encode(hasher.finalize()));
        }
        hasher.update(&buffer[..count]);
    }
}

/// Host-created read-only grant. Limits bound whole-file reads and scan results.
#[derive(Debug)]
pub struct ReadRoot {
    directory: File,
    read_bytes: u64,
    scan_entries: usize,
}
impl ReadRoot {
    pub fn new(root: &Path, read_bytes: u64, scan_entries: usize) -> Result<Self> {
        if read_bytes == 0 || read_bytes == u64::MAX || scan_entries == 0 {
            return Err(Error::Invalid(
                "File grant limits must be positive and bounded",
            ));
        }
        Ok(Self {
            directory: directory(root)?,
            read_bytes,
            scan_entries,
        })
    }
    fn resolve_parent<'a>(&self, name: &'a str) -> Result<(File, &'a str)> {
        let parts = path_parts(name)?;
        let mut parent = self.directory.try_clone()?;
        for part in &parts[..parts.len() - 1] {
            parent = child_directory(&parent, Path::new(part))?;
        }
        Ok((parent, parts[parts.len() - 1]))
    }
    fn open(&self, name: &str) -> Result<File> {
        let (parent, name) = self.resolve_parent(name)?;
        regular(&parent, Path::new(name), REGULAR)
    }
    /// A stable, bounded stream capability. The descriptor never escapes this
    /// module, and later reads reject observed mutation or entry replacement.
    pub fn open_file(&self, name: &str) -> Result<ReadFile> {
        let (parent, name) = self.resolve_parent(name)?;
        let file = regular(&parent, Path::new(name), REGULAR)?;
        let identity = Identity::from(&file.metadata()?);
        if identity.size > self.read_bytes {
            return Err(Error::Invalid("File exceeds granted read limit"));
        }
        let value = ReadFile(Arc::new(ReadFileInner {
            file,
            parent,
            name: name.into(),
            identity,
        }));
        value.check()?;
        Ok(value)
    }
    pub fn read(&self, name: &str) -> Result<Vec<u8>> {
        let file = self.open(name)?;
        if file.metadata()?.len() > self.read_bytes {
            return Err(Error::Invalid("File exceeds granted read limit"));
        }
        let mut content = Vec::new();
        file.take(self.read_bytes + 1).read_to_end(&mut content)?;
        if content.len() as u64 > self.read_bytes {
            return Err(Error::Invalid("File exceeds granted read limit"));
        }
        Ok(content)
    }
    pub fn digest(&self, name: &str) -> Result<String> {
        digest(self.open(name)?)
    }
    pub fn scan(&self, name: &str, suffix: &str) -> Result<Vec<String>> {
        let directory = (|| {
            let (parent, name) = self.resolve_parent(name)?;
            child_directory(&parent, Path::new(name))
        })();
        let directory = match directory {
            Ok(file) => file,
            Err(Error::Io(error)) if error.kind() == io::ErrorKind::NotFound => return Ok(vec![]),
            Err(error) => return Err(error),
        };
        let mut found = Vec::new();
        self.visit(directory, name, suffix, 0, &mut 0, &mut found)?;
        found.sort();
        Ok(found)
    }
    fn visit(
        &self,
        directory: File,
        prefix: &str,
        suffix: &str,
        depth: usize,
        visited: &mut usize,
        found: &mut Vec<String>,
    ) -> Result<()> {
        if depth >= SCAN_DEPTH {
            return Err(Error::Invalid("File scan exceeds directory depth limit"));
        }
        // Open a fresh directory description: dup/try_clone shares the scan cursor.
        let mut entries = Dir::openat(&directory, ".", DIRECTORY, Mode::empty())?;
        for entry in entries.iter() {
            let entry = entry?;
            let name = entry
                .file_name()
                .to_str()
                .map_err(|_| Error::Invalid("File names must be UTF-8"))?;
            if matches!(name, "." | "..") {
                continue;
            }
            path_parts(name)?;
            *visited += 1;
            if *visited > self.scan_entries {
                return Err(Error::Invalid("File scan exceeds granted entry limit"));
            }
            let stat = fstatat(&directory, name, AtFlags::AT_SYMLINK_NOFOLLOW)?;
            let kind = SFlag::from_bits_truncate(stat.st_mode);
            let relative = format!("{prefix}/{name}");
            if kind == SFlag::S_IFLNK {
                return Err(Error::Invalid("File links are not allowed"));
            }
            if kind == SFlag::S_IFDIR {
                self.visit(
                    child_directory(&directory, Path::new(name))?,
                    &relative,
                    suffix,
                    depth + 1,
                    visited,
                    found,
                )?;
            } else if kind == SFlag::S_IFREG {
                if name.ends_with(suffix) {
                    found.push(relative);
                }
            } else {
                return Err(Error::Invalid("A regular file or directory is required"));
            }
        }
        Ok(())
    }
}

#[derive(Debug)]
struct ReadFileInner {
    file: File,
    parent: File,
    name: PathBuf,
    identity: Identity,
}

/// Shared read-only ownership with positional I/O; clones do not share a seek
/// cursor. This detects observed changes, not an atomic filesystem snapshot.
#[derive(Debug, Clone)]
pub struct ReadFile(Arc<ReadFileInner>);
impl ReadFile {
    pub fn len(&self) -> u64 {
        self.0.identity.size
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn check(&self) -> Result<()> {
        let current = Identity::from(&self.0.file.metadata()?);
        let entry = fstatat(
            &self.0.parent,
            self.0.name.as_path(),
            AtFlags::AT_SYMLINK_NOFOLLOW,
        )?;
        if current != self.0.identity
            || i128::from(entry.st_dev) != i128::from(current.device)
            || i128::from(entry.st_ino) != i128::from(current.inode)
            || SFlag::from_bits_truncate(entry.st_mode) != SFlag::S_IFREG
        {
            return Err(Error::Invalid("Granted file changed during reading"));
        }
        Ok(())
    }
    /// Each individual transfer is capped at 64 MiB, independently of the
    /// whole-file grant. Callers can stream larger files through bounded buffers.
    pub fn read_at(&self, offset: u64, output: &mut [u8]) -> Result<usize> {
        if output.len() > 64 * 1024 * 1024 || offset > self.len() {
            return Err(Error::Invalid("File read exceeds transfer bounds"));
        }
        self.check()?;
        let length = output
            .len()
            .min((self.len() - offset).min(usize::MAX as u64) as usize);
        let count = self.0.file.read_at(&mut output[..length], offset)?;
        self.check()?;
        if count == 0 && length != 0 {
            return Err(Error::Invalid(
                "Granted file ended before its declared length",
            ));
        }
        Ok(count)
    }
    /// Verify using the same descriptor that serves subsequent reads. A fixed
    /// cancellation flag lets hosts revoke long reads without invoking callbacks.
    pub fn verify_sha256(&self, expected: &str, cancelled: &AtomicBool) -> Result<()> {
        if expected.len() != 64
            || !expected
                .bytes()
                .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
        {
            return Err(Error::Invalid("Invalid SHA-256 checksum"));
        }
        let mut digest = Sha256::new();
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut offset = 0;
        loop {
            if cancelled.load(Ordering::Acquire) {
                return Err(Error::Io(io::Error::new(
                    io::ErrorKind::Interrupted,
                    "File read cancelled",
                )));
            }
            if offset == self.len() {
                break;
            }
            let count = self.read_at(offset, &mut buffer)?;
            digest.update(&buffer[..count]);
            offset += count as u64;
        }
        self.check()?;
        if hex::encode(digest.finalize()) != expected {
            return Err(Error::Invalid("File checksum mismatch"));
        }
        Ok(())
    }
}

/// Stream SHA-256 without loading a whole host file into memory.
pub fn file_digest(path: &Path) -> Result<String> {
    let (parent, name) = parent(path)?;
    digest(regular(&parent, name, REGULAR)?)
}

#[derive(Debug, PartialEq, Eq)]
struct Identity {
    device: u64,
    inode: u64,
    mode: u32,
    size: u64,
    modified: (i64, i64),
    changed: (i64, i64),
}
impl From<&Metadata> for Identity {
    fn from(value: &Metadata) -> Self {
        Self {
            device: value.dev(),
            inode: value.ino(),
            mode: value.mode(),
            size: value.size(),
            modified: (value.mtime(), value.mtime_nsec()),
            changed: (value.ctime(), value.ctime_nsec()),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
struct TreeEntry {
    source: PathBuf,
    name: Option<String>,
    source_identity: Identity,
    target_identity: Identity,
    target: PathBuf,
}
impl TreeEntry {
    fn observe(source: PathBuf, name: Option<String>) -> Result<Self> {
        let metadata = fs::symlink_metadata(&source)?;
        let target = fs::canonicalize(&source)?;
        let target_metadata = fs::metadata(&target)?;
        if !target_metadata.is_dir() && !target_metadata.is_file() {
            return Err(Error::Invalid(
                "Trusted trees require regular files or directories",
            ));
        }
        Ok(Self {
            source,
            name,
            source_identity: Identity::from(&metadata),
            target_identity: Identity::from(&target_metadata),
            target,
        })
    }
    fn check(&self) -> Result<()> {
        let current = Self::observe(self.source.clone(), self.name.clone())?;
        if &current != self {
            return Err(Error::Invalid("Trusted tree changed while hashing"));
        }
        Ok(())
    }
}

struct TreePlan<'a> {
    roots: &'a [PathBuf],
    excluded_components: &'a [String],
    excluded_suffixes: &'a [String],
    max_entries: usize,
    max_bytes: u64,
}
impl TreePlan<'_> {
    fn validate(&self) -> Result<()> {
        if self.roots.is_empty()
            || self.roots.len() > 128
            || self.excluded_components.len() > 128
            || self.excluded_suffixes.len() > 128
            || self.max_entries == 0
            || self.max_entries > 1_000_000
            || self.max_bytes == 0
            || self.max_bytes > 1024 * 1024 * 1024 * 1024
        {
            return Err(Error::Invalid(
                "Trusted tree budgets must be positive and bounded",
            ));
        }
        for component in self.excluded_components {
            if path_parts(component)?.len() != 1 {
                return Err(Error::Invalid(
                    "Tree exclusions must be single path components",
                ));
            }
        }
        for suffix in self.excluded_suffixes {
            if !suffix.starts_with('.') || suffix.len() < 2 || suffix.contains(['/', '\\', '\0']) {
                return Err(Error::Invalid("Invalid tree suffix exclusion"));
            }
        }
        Ok(())
    }
    fn capture(&self) -> Result<Vec<TreeEntry>> {
        let mut entries = Vec::new();
        let mut visited = 0;
        for (index, root) in self.roots.iter().enumerate() {
            visited += 1;
            if visited > self.max_entries {
                return Err(Error::Invalid("Trusted tree exceeds entry budget"));
            }
            let root_entry = TreeEntry::observe(root.clone(), None)?;
            if root_entry.target_identity.mode & 0o170000 == 0o100000 {
                let name = root
                    .file_name()
                    .and_then(|part| part.to_str())
                    .ok_or(Error::Invalid("Tree file names must be UTF-8"))?;
                if !self.excluded(name) {
                    entries.push(TreeEntry {
                        name: Some(format!("{index}/{name}")),
                        ..root_entry
                    });
                }
            } else {
                entries.push(root_entry);
                self.visit(root, &mut vec![], index, 0, &mut visited, &mut entries)?;
            }
        }
        Ok(entries)
    }
    fn excluded(&self, name: &str) -> bool {
        self.excluded_components.iter().any(|part| part == name)
            || name.rfind('.').is_some_and(|index| {
                index > 0
                    && self
                        .excluded_suffixes
                        .iter()
                        .any(|suffix| &name[index..] == suffix)
            })
    }
    fn visit(
        &self,
        directory: &Path,
        relative: &mut Vec<String>,
        root_index: usize,
        depth: usize,
        visited: &mut usize,
        entries: &mut Vec<TreeEntry>,
    ) -> Result<()> {
        if depth >= SCAN_DEPTH {
            return Err(Error::Invalid("Trusted tree exceeds directory depth limit"));
        }
        let mut children = Vec::new();
        for child in fs::read_dir(directory)? {
            let child = child?;
            *visited += 1;
            if *visited > self.max_entries {
                return Err(Error::Invalid("Trusted tree exceeds entry budget"));
            }
            let name = child
                .file_name()
                .into_string()
                .map_err(|_| Error::Invalid("Tree file names must be UTF-8"))?;
            children.push(name);
        }
        children.sort();
        for name in children {
            if self.excluded_components.iter().any(|part| part == &name) {
                continue;
            }
            let source = directory.join(&name);
            let mut entry = TreeEntry::observe(source.clone(), None)?;
            relative.push(name.clone());
            if entry.target_identity.mode & 0o170000 == 0o100000 {
                if !self.excluded(&name) {
                    entry.name = Some(format!("{root_index}/{}", relative.join("/")));
                    entries.push(entry);
                }
            } else {
                let is_alias = entry.source_identity.mode & 0o170000 == 0o120000;
                entries.push(entry);
                if !is_alias {
                    self.visit(&source, relative, root_index, depth + 1, visited, entries)?;
                }
            }
            relative.pop();
        }
        Ok(())
    }
    fn digest(&self, entries: Vec<TreeEntry>) -> Result<String> {
        let mut digest = Sha256::new();
        let mut buffer = vec![0_u8; 1024 * 1024];
        let mut bytes = 0_u64;
        for entry in &entries {
            let Some(name) = &entry.name else { continue };
            entry.check()?;
            // This trusted-host operation deliberately follows file aliases. The
            // separate ReadRoot capability continues to reject every symlink.
            let mut file = OpenOptions::new()
                .read(true)
                .custom_flags((OFlag::O_CLOEXEC | OFlag::O_NONBLOCK).bits())
                .open(&entry.source)?;
            if Identity::from(&file.metadata()?) != entry.target_identity {
                return Err(Error::Invalid("Trusted tree changed while hashing"));
            }
            if entry.target_identity.size > self.max_bytes - bytes {
                return Err(Error::Invalid("Trusted tree exceeds byte budget"));
            }
            let mut content = Sha256::new();
            loop {
                let remaining = ((self.max_bytes - bytes + 1).min(buffer.len() as u64)) as usize;
                let count = file.read(&mut buffer[..remaining])?;
                if count == 0 {
                    break;
                }
                bytes = bytes
                    .checked_add(count as u64)
                    .filter(|value| *value <= self.max_bytes)
                    .ok_or(Error::Invalid("Trusted tree exceeds byte budget"))?;
                content.update(&buffer[..count]);
            }
            if Identity::from(&file.metadata()?) != entry.target_identity {
                return Err(Error::Invalid("Trusted tree changed while hashing"));
            }
            entry.check()?;
            digest.update((name.len() as u64).to_be_bytes());
            digest.update(name.as_bytes());
            digest.update(content.finalize());
        }
        if entries != self.capture()? {
            return Err(Error::Invalid("Trusted tree changed while hashing"));
        }
        Ok(hex::encode(digest.finalize()))
    }
}

/// Stable content identity for host-selected, trusted installation roots.
///
/// Roots and regular-file symlinks are followed, including interpreter aliases;
/// descendant directory symlinks are validated but not traversed. Every file
/// alias contributes under its original relative name. Empty directories and
/// metadata do not affect the hash. Broken links and special files fail. Two tree
/// observations and descriptor metadata checks reject concurrent replacement or
/// writes; this is not an atomic filesystem snapshot or an untrusted read grant.
/// Budgets count visited entries and bytes read, including repeated file aliases.
pub fn trusted_tree_digest(
    roots: &[PathBuf],
    excluded_components: &[String],
    excluded_suffixes: &[String],
    max_entries: usize,
    max_bytes: u64,
) -> Result<String> {
    let plan = TreePlan {
        roots,
        excluded_components,
        excluded_suffixes,
        max_entries,
        max_bytes,
    };
    plan.validate()?;
    plan.digest(plan.capture()?)
}

/// Publish complete bytes through an opened parent directory. The temporary file
/// and directory are synced. No-replace publication fails if a destination exists.
/// Errors after successful publication have `Error::published() == true`;
/// the destination is already visible even though durability is not confirmed.
pub fn atomic_write(path: &Path, content: &[u8], replace: bool) -> Result<()> {
    let (parent, name) = parent(path)?;
    atomic_write_at(&parent, name, content, replace)
}

pub(crate) fn atomic_write_at(
    parent: &File,
    name: &Path,
    content: &[u8],
    replace: bool,
) -> Result<()> {
    atomic_write_with_sync(parent, name, content, replace, File::sync_all)
}

fn atomic_write_with_sync(
    parent: &File,
    name: &Path,
    content: &[u8],
    replace: bool,
    sync_directory: impl FnOnce(&File) -> io::Result<()>,
) -> Result<()> {
    match fstatat(parent, name, AtFlags::AT_SYMLINK_NOFOLLOW) {
        Ok(stat) if SFlag::from_bits_truncate(stat.st_mode) != SFlag::S_IFREG => {
            return Err(Error::Invalid(
                "Atomic write requires a regular destination",
            ));
        }
        Ok(_) | Err(Errno::ENOENT) => {}
        Err(error) => return Err(error.into()),
    }
    let temporary = format!(".asterion-write-{}", uuid::Uuid::new_v4().simple());
    let mut file = regular(
        parent,
        Path::new(&temporary),
        OFlag::O_WRONLY | OFlag::O_CREAT | OFlag::O_EXCL | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
    )?;
    let mut published = false;
    let result = (|| {
        file.write_all(content)?;
        file.sync_all()?;
        if replace {
            renameat(parent, temporary.as_str(), parent, name)?;
            published = true;
        } else {
            linkat(
                parent,
                temporary.as_str(),
                parent,
                name,
                LinkatFlags::empty(),
            )?;
            published = true;
            unlinkat(parent, temporary.as_str(), UnlinkatFlags::NoRemoveDir)?;
        }
        sync_directory(parent)?;
        Ok(())
    })();
    // The UUID name belongs solely to this invocation; never remove a user path.
    let _ = unlinkat(parent, temporary.as_str(), UnlinkatFlags::NoRemoveDir);
    result.map_err(|error| {
        if published {
            Error::Published(Box::new(error))
        } else {
            error
        }
    })
}

/// Exclusive advisory lock, released by dropping its owning handle.
#[derive(Debug)]
pub struct FileLock {
    _file: Flock<File>,
}
impl FileLock {
    fn open(path: &Path) -> Result<File> {
        let (parent, name) = parent(path)?;
        regular(
            &parent,
            name,
            OFlag::O_RDWR
                | OFlag::O_CREAT
                | OFlag::O_CLOEXEC
                | OFlag::O_NOFOLLOW
                | OFlag::O_NONBLOCK,
        )
    }
    pub fn acquire(path: &Path, blocking: bool) -> Result<Self> {
        let file = Self::open(path)?;
        let mode = if blocking {
            FlockArg::LockExclusive
        } else {
            FlockArg::LockExclusiveNonblock
        };
        let file = Flock::lock(file, mode).map_err(|(_, error)| Error::from(error))?;
        Ok(Self { _file: file })
    }

    /// Acquire within a monotonic deadline, retaining one opened lock file.
    pub fn acquire_for(path: &Path, timeout: Duration) -> Result<Self> {
        if timeout > Duration::from_secs(86400) {
            return Err(Error::Invalid("File lock timeout exceeds one day"));
        }
        let deadline = Instant::now() + timeout;
        let mut file = Self::open(path)?;
        let mut first_attempt = true;
        loop {
            if !first_attempt && Instant::now() >= deadline {
                return Err(Error::Io(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "File lock acquisition timed out",
                )));
            }
            first_attempt = false;
            match Flock::lock(file, FlockArg::LockExclusiveNonblock) {
                Ok(file) => return Ok(Self { _file: file }),
                Err((handle, Errno::EWOULDBLOCK)) => file = handle,
                Err((_, error)) => return Err(error.into()),
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(Error::Io(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "File lock acquisition timed out",
                )));
            }
            std::thread::sleep(remaining.min(Duration::from_millis(20)));
        }
    }
}

#[cfg(test)]
mod tree_stability_tests {
    use super::*;
    use std::os::unix::fs::symlink;

    #[test]
    fn changed_content_and_same_name_replacement_fail_before_returning_an_identity() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("payload");
        fs::write(&path, b"first").unwrap();
        let roots = [temp.path().to_owned()];
        let plan = TreePlan {
            roots: &roots,
            excluded_components: &[],
            excluded_suffixes: &[],
            max_entries: 100,
            max_bytes: 1024,
        };
        let before = plan.capture().unwrap();
        let original = fs::metadata(&path).unwrap().modified().unwrap();
        // Filesystem timestamp granularity can exceed a scheduler tick.
        std::thread::sleep(Duration::from_millis(20));
        fs::write(&path, b"other").unwrap();
        File::options()
            .write(true)
            .open(&path)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(original))
            .unwrap();
        assert!(matches!(
            plan.digest(before),
            Err(Error::Invalid("Trusted tree changed while hashing"))
        ));
        let before = plan.capture().unwrap();
        atomic_write(&path, b"other", true).unwrap();
        assert!(matches!(
            plan.digest(before),
            Err(Error::Invalid("Trusted tree changed while hashing"))
        ));
    }

    #[test]
    fn tree_membership_and_directory_alias_retargeting_are_observed() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("root");
        fs::create_dir(&root).unwrap();
        fs::create_dir(temp.path().join("first")).unwrap();
        fs::create_dir(temp.path().join("second")).unwrap();
        symlink("../first", root.join("alias")).unwrap();
        let roots = [root.clone()];
        let plan = TreePlan {
            roots: &roots,
            excluded_components: &[],
            excluded_suffixes: &[],
            max_entries: 100,
            max_bytes: 1024,
        };
        let before = plan.capture().unwrap();
        fs::write(root.join("added"), b"added").unwrap();
        assert!(matches!(
            plan.digest(before),
            Err(Error::Invalid("Trusted tree changed while hashing"))
        ));
        let before = plan.capture().unwrap();
        fs::remove_file(root.join("alias")).unwrap();
        symlink("../second", root.join("alias")).unwrap();
        assert!(matches!(
            plan.digest(before),
            Err(Error::Invalid("Trusted tree changed while hashing"))
        ));
    }
}

#[cfg(test)]
mod publication_tests {
    use super::*;

    #[test]
    fn directory_sync_failures_report_publication_and_preserve_the_new_target() {
        for replace in [false, true] {
            let temp = tempfile::tempdir().unwrap();
            let path = temp.path().join("state");
            if replace {
                fs::write(&path, b"before").unwrap();
            }
            let parent = directory(temp.path()).unwrap();
            let error =
                atomic_write_with_sync(&parent, Path::new("state"), b"published", replace, |_| {
                    Err(io::Error::from_raw_os_error(Errno::EIO as i32))
                })
                .unwrap_err();
            assert!(error.published());
            assert!(
                matches!(&error, Error::Published(cause) if matches!(cause.as_ref(), Error::Io(error) if error.raw_os_error() == Some(Errno::EIO as i32)))
            );
            assert_eq!(fs::read(&path).unwrap(), b"published");
            assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 1);
        }
    }

    #[test]
    fn failed_no_replace_publication_keeps_the_old_target_and_is_not_published() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("state");
        fs::write(&path, b"before").unwrap();
        let parent = directory(temp.path()).unwrap();
        let error = atomic_write_with_sync(&parent, Path::new("state"), b"after", false, |_| {
            panic!("unpublished write must not sync the directory")
        })
        .unwrap_err();
        assert!(!error.published());
        assert_eq!(fs::read(&path).unwrap(), b"before");
        assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 1);
    }
}
