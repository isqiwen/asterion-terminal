//! Bounded, streaming filesystem archives. The host owns metadata semantics;
//! this module owns capture, verification, private extraction and publication.
use crate::{
    directories::{self, StagedDirectory},
    files,
};
use nix::errno::Errno;
use nix::fcntl::{AtFlags, OFlag};
use nix::sys::stat::{SFlag, fstatat};
use nix::unistd::{LinkatFlags, UnlinkatFlags, linkat, unlinkat};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use zip::write::SimpleFileOptions;

type Result<T> = std::result::Result<T, files::Error>;
type Check<'a> = &'a mut dyn FnMut() -> Result<()>;
const INVALID: files::Error = files::Error::Invalid("Invalid streaming archive");
const BUFFER: usize = 1024 * 1024;

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Policy {
    pub bytes: u64,
    pub files: usize,
    pub directories: usize,
    pub metadata_bytes: u64,
}
impl Policy {
    fn validate(&self) -> Result<()> {
        if self.bytes == 0
            || self.bytes > 500 * 1024_u64.pow(3)
            || self.files == 0
            || self.files > 1_000_000
            || self.directories == 0
            || self.directories > 1_000_000
            || self.metadata_bytes == 0
            || self.metadata_bytes > 128 * 1024 * 1024
        {
            return Err(files::Error::Invalid("Invalid streaming archive limits"));
        }
        Ok(())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Evidence {
    pub bytes: u64,
    pub sha256: String,
}
#[derive(Debug, Default, Serialize)]
pub struct Catalog {
    pub files: BTreeMap<String, Evidence>,
    pub directories: BTreeSet<String>,
}
#[derive(Serialize)]
pub struct Published {
    pub path: PathBuf,
    pub files: usize,
    pub bytes: u64,
    pub sha256: String,
}
fn zip_error(_: zip::result::ZipError) -> files::Error {
    INVALID
}
fn same_file(before: &std::fs::Metadata, after: &std::fs::Metadata) -> bool {
    before.dev() == after.dev()
        && before.ino() == after.ino()
        && before.len() == after.len()
        && before.mtime() == after.mtime()
        && before.mtime_nsec() == after.mtime_nsec()
        && before.ctime() == after.ctime()
        && before.ctime_nsec() == after.ctime_nsec()
}
fn digest_file(file: &mut File, check: Check<'_>) -> Result<String> {
    file.seek(SeekFrom::Start(0))?;
    let mut digest = Sha256::new();
    let mut buffer = vec![0; BUFFER];
    loop {
        check()?;
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    Ok(hex::encode(digest.finalize()))
}
struct PendingFile {
    parent: File,
    file: File,
    name: String,
}
impl PendingFile {
    fn owned(&self) -> bool {
        let Ok(current) = directories::child_file(&self.parent, &self.name, OFlag::O_RDONLY)
            .and_then(|file| file.metadata().map_err(Into::into))
        else {
            return false;
        };
        self.file
            .metadata()
            .is_ok_and(|own| own.dev() == current.dev() && own.ino() == current.ino())
    }
}
impl Drop for PendingFile {
    fn drop(&mut self) {
        if self.owned() {
            let _ = unlinkat(&self.parent, self.name.as_str(), UnlinkatFlags::NoRemoveDir);
        }
    }
}
pub struct Writer {
    pending: PendingFile,
    writer: Option<zip::ZipWriter<File>>,
    destination: PathBuf,
    destination_name: String,
    metadata_name: String,
    policy: Policy,
    catalog: Catalog,
    total: u64,
    catalog_bytes: u64,
    source: File,
    excluded: Vec<String>,
}
impl Writer {
    pub fn capture(
        source: &Path,
        destination: &Path,
        roots: &[String],
        excluded_files: &[String],
        policy: Policy,
        metadata_name: &str,
        check: Check<'_>,
    ) -> Result<Self> {
        policy.validate()?;
        directories::relative(metadata_name)?;
        if roots.is_empty() || roots.len() > 1024 || excluded_files.len() > 1024 {
            return Err(INVALID);
        }
        for name in roots.iter().chain(excluded_files) {
            directories::relative(name.trim_end_matches('/'))?;
        }
        let parent = files::directory(destination.parent().ok_or(INVALID)?)?;
        let destination_name = destination
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or(INVALID)?
            .to_owned();
        if directories::relative(&destination_name)?.len() != 1 {
            return Err(INVALID);
        }
        match fstatat(
            &parent,
            destination_name.as_str(),
            AtFlags::AT_SYMLINK_NOFOLLOW,
        ) {
            Ok(_) => return Err(io::Error::from(io::ErrorKind::AlreadyExists).into()),
            Err(Errno::ENOENT) => {}
            Err(error) => return Err(error.into()),
        }
        let name = format!(".asterion-archive-{}", uuid::Uuid::new_v4().simple());
        let file = directories::child_file(
            &parent,
            &name,
            OFlag::O_RDWR | OFlag::O_CREAT | OFlag::O_EXCL,
        )?;
        let writer = zip::ZipWriter::new(file.try_clone()?);
        let mut result = Self {
            pending: PendingFile { parent, file, name },
            writer: Some(writer),
            destination: destination.to_owned(),
            destination_name,
            metadata_name: metadata_name.to_owned(),
            policy,
            catalog: Catalog::default(),
            total: 0,
            catalog_bytes: 0,
            source: files::directory(source)?,
            excluded: excluded_files.to_vec(),
        };
        for name in roots {
            let parts = directories::relative(name)?;
            let mut parent = result.source.try_clone()?;
            for part in &parts[..parts.len() - 1] {
                parent = files::child_directory(&parent, Path::new(part))?;
            }
            result.capture_entry(&parent, parts[parts.len() - 1], name, check)?;
        }
        Ok(result)
    }
    fn budget(&mut self, name: &str, extra: u64) -> Result<()> {
        // JSON escaping and the per-file digest evidence also consume metadata budget.
        let name_bytes = serde_json::to_string(name).map_err(|_| INVALID)?.len() as u64;
        self.catalog_bytes = self
            .catalog_bytes
            .checked_add(name_bytes + extra)
            .ok_or(INVALID)?;
        if self.catalog_bytes > self.policy.metadata_bytes {
            return Err(files::Error::Invalid("Archive metadata exceeds byte limit"));
        }
        Ok(())
    }
    fn capture_entry(
        &mut self,
        parent: &File,
        child: &str,
        name: &str,
        check: Check<'_>,
    ) -> Result<()> {
        check()?;
        directories::relative(name)?;
        if name == self.metadata_name
            || self.catalog.files.contains_key(name)
            || self.catalog.directories.contains(name)
        {
            return Err(files::Error::Invalid("Archive paths overlap"));
        }
        match directories::kind(parent, child)? {
            SFlag::S_IFDIR => {
                if self.catalog.directories.len() >= self.policy.directories {
                    return Err(files::Error::Invalid("Archive directory limit exceeded"));
                }
                self.budget(name, 1)?;
                self.catalog.directories.insert(name.to_owned());
                let directory = files::child_directory(parent, Path::new(child))?;
                let before = directory.metadata()?;
                for next in directories::entries(&directory)? {
                    self.capture_entry(&directory, &next, &format!("{name}/{next}"), check)?;
                }
                if !same_file(&before, &directory.metadata()?) {
                    return Err(files::Error::Invalid(
                        "Source directory changed during capture",
                    ));
                }
            }
            SFlag::S_IFREG => {
                if self.excluded.iter().any(|prefix| name.starts_with(prefix)) {
                    return Ok(());
                }
                if self.catalog.files.len() >= self.policy.files {
                    return Err(files::Error::Invalid("Archive file limit exceeded"));
                }
                let mut file = directories::child_file(parent, child, OFlag::O_RDONLY)?;
                let before = file.metadata()?;
                if before.len() > self.policy.bytes.saturating_sub(self.total) {
                    return Err(files::Error::Invalid("Archive byte limit exceeded"));
                }
                self.budget(name, 128)?;
                let writer = self.writer.as_mut().ok_or(INVALID)?;
                writer
                    .start_file(
                        name,
                        SimpleFileOptions::default()
                            .compression_method(zip::CompressionMethod::Deflated)
                            .unix_permissions(0o600)
                            .large_file(true),
                    )
                    .map_err(zip_error)?;
                let mut digest = Sha256::new();
                let mut count = 0_u64;
                let mut buffer = vec![0; BUFFER];
                loop {
                    check()?;
                    let read = file.read(&mut buffer)?;
                    if read == 0 {
                        break;
                    }
                    count = count.checked_add(read as u64).ok_or(INVALID)?;
                    if count > before.len() {
                        return Err(files::Error::Invalid("Source file grew during capture"));
                    }
                    digest.update(&buffer[..read]);
                    writer.write_all(&buffer[..read])?;
                }
                let current =
                    directories::child_file(parent, child, OFlag::O_RDONLY)?.metadata()?;
                if count != before.len()
                    || !same_file(&before, &file.metadata()?)
                    || !same_file(&before, &current)
                {
                    return Err(files::Error::Invalid("Source file changed during capture"));
                }
                self.total += count;
                self.catalog.files.insert(
                    name.to_owned(),
                    Evidence {
                        bytes: count,
                        sha256: hex::encode(digest.finalize()),
                    },
                );
            }
            _ => {
                return Err(files::Error::Invalid(
                    "Source contains a link or special file",
                ));
            }
        }
        Ok(())
    }
    pub fn catalog(&self) -> Result<&Catalog> {
        if self.writer.is_none() {
            return Err(files::Error::Invalid("Archive writer is closed"));
        }
        Ok(&self.catalog)
    }
    pub fn commit(&mut self, metadata: &[u8], check: Check<'_>) -> Result<Published> {
        self.commit_with_sync(metadata, check, File::sync_all)
    }
    fn commit_with_sync(
        &mut self,
        metadata: &[u8],
        check: Check<'_>,
        sync_parent: impl FnOnce(&File) -> io::Result<()>,
    ) -> Result<Published> {
        let mut writer = self
            .writer
            .take()
            .ok_or(files::Error::Invalid("Archive writer is closed"))?;
        if metadata.len() as u64 > self.policy.metadata_bytes {
            return Err(files::Error::Invalid("Archive metadata exceeds byte limit"));
        }
        check()?;
        writer
            .start_file(
                &self.metadata_name,
                SimpleFileOptions::default()
                    .compression_method(zip::CompressionMethod::Deflated)
                    .unix_permissions(0o600)
                    .large_file(true),
            )
            .map_err(zip_error)?;
        writer.write_all(metadata)?;
        let mut file = writer.finish().map_err(zip_error)?;
        // The central index and compressed input must fit the same admission
        // budget as a subsequent reader before making the archive visible.
        index(&mut file, &self.policy)?;
        file.sync_all()?;
        let sha256 = digest_file(&mut file, check)?;
        let bytes = file.metadata()?.len();
        if !self.pending.owned() {
            return Err(files::Error::Invalid(
                "Archive staging identity has changed",
            ));
        }
        check()?;
        linkat(
            &self.pending.parent,
            self.pending.name.as_str(),
            &self.pending.parent,
            self.destination_name.as_str(),
            LinkatFlags::empty(),
        )?;
        unlinkat(
            &self.pending.parent,
            self.pending.name.as_str(),
            UnlinkatFlags::NoRemoveDir,
        )
        .map_err(|error| files::Error::Published(Box::new(error.into())))?;
        // Publication has happened. A durability error must not erase the target.
        sync_parent(&self.pending.parent)
            .map_err(|error| files::Error::Published(Box::new(error.into())))?;
        Ok(Published {
            path: self.destination.clone(),
            files: self.catalog.files.len(),
            bytes,
            sha256,
        })
    }
}

// During index construction expose only the admitted central-directory window.
// zip may search older footers after a malformed one; unrelated file bodies must
// never become an alternative, unbounded source of metadata allocations.
struct MetadataWindow {
    start: u64,
    bytes: Vec<u8>,
}
struct ArchiveFile {
    file: File,
    position: u64,
    length: u64,
    window: Arc<Mutex<Option<MetadataWindow>>>,
}
impl Read for ArchiveFile {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        let guard = self
            .window
            .lock()
            .map_err(|_| io::Error::other("Archive index is unavailable"))?;
        if let Some(window) = guard.as_ref() {
            let count = output
                .len()
                .min(self.length.saturating_sub(self.position) as usize);
            output[..count].fill(0);
            let begin = self.position.max(window.start);
            let end = (self.position + count as u64).min(window.start + window.bytes.len() as u64);
            if begin < end {
                output[(begin - self.position) as usize..(end - self.position) as usize]
                    .copy_from_slice(
                        &window.bytes
                            [(begin - window.start) as usize..(end - window.start) as usize],
                    );
            }
            self.position += count as u64;
            Ok(count)
        } else {
            drop(guard);
            self.file.seek(SeekFrom::Start(self.position))?;
            let count = self.file.read(output)?;
            self.position += count as u64;
            Ok(count)
        }
    }
}
impl Seek for ArchiveFile {
    fn seek(&mut self, from: SeekFrom) -> io::Result<u64> {
        let value = match from {
            SeekFrom::Start(value) => i128::from(value),
            SeekFrom::End(delta) => i128::from(self.length) + i128::from(delta),
            SeekFrom::Current(delta) => i128::from(self.position) + i128::from(delta),
        };
        self.position =
            u64::try_from(value).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
        Ok(self.position)
    }
}
fn number<const N: usize>(content: &[u8], at: usize) -> Result<[u8; N]> {
    content
        .get(at..at.saturating_add(N))
        .and_then(|part| part.try_into().ok())
        .ok_or(INVALID)
}
fn u16_at(content: &[u8], at: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(number(content, at)?))
}
fn u32_at(content: &[u8], at: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(number(content, at)?))
}
fn u64_at(content: &[u8], at: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(number(content, at)?))
}
fn read_at(file: &mut File, at: u64, length: usize) -> Result<Vec<u8>> {
    file.seek(SeekFrom::Start(at))?;
    let mut bytes = vec![0; length];
    file.read_exact(&mut bytes)?;
    Ok(bytes)
}
fn index(file: &mut File, policy: &Policy) -> Result<(MetadataWindow, usize)> {
    let length = file.metadata()?.len();
    // Deflate framing, file headers and metadata have a separate finite allowance.
    if length
        > policy
            .bytes
            .saturating_add(policy.bytes / 100)
            .saturating_add(policy.metadata_bytes * 3)
    {
        return Err(files::Error::Invalid("Archive input exceeds byte limit"));
    }
    let tail_start = length.saturating_sub(65_557);
    let tail = read_at(file, tail_start, (length - tail_start) as usize)?;
    let end = (0..tail.len().saturating_sub(21))
        .rev()
        .find(|&at| {
            tail.get(at..at + 4) == Some(b"PK\x05\x06")
                && u16_at(&tail, at + 20)
                    .is_ok_and(|comment| at + 22 + usize::from(comment) == tail.len())
        })
        .ok_or(INVALID)?;
    if u16_at(&tail, end + 4)? != 0
        || u16_at(&tail, end + 6)? != 0
        || u16_at(&tail, end + 8)? != u16_at(&tail, end + 10)?
    {
        return Err(INVALID);
    }
    let (mut count, mut size, mut start) = (
        u64::from(u16_at(&tail, end + 10)?),
        u64::from(u32_at(&tail, end + 12)?),
        u64::from(u32_at(&tail, end + 16)?),
    );
    let mut directory_end = tail_start + end as u64;
    if count == u16::MAX as u64 || size == u32::MAX as u64 || start == u32::MAX as u64 {
        let locator = read_at(file, directory_end.checked_sub(20).ok_or(INVALID)?, 20)?;
        if &locator[..4] != b"PK\x06\x07" || u32_at(&locator, 4)? != 0 || u32_at(&locator, 16)? != 1
        {
            return Err(INVALID);
        }
        let position = u64_at(&locator, 8)?;
        let record = read_at(file, position, 56)?;
        let record_size = u64_at(&record, 4)?;
        if &record[..4] != b"PK\x06\x06"
            || record_size < 44
            || record_size > policy.metadata_bytes
            || position
                .checked_add(record_size)
                .and_then(|value| value.checked_add(12))
                != directory_end.checked_sub(20)
            || u32_at(&record, 16)? != 0
            || u32_at(&record, 20)? != 0
            || u64_at(&record, 24)? != u64_at(&record, 32)?
        {
            return Err(INVALID);
        }
        count = u64_at(&record, 32)?;
        size = u64_at(&record, 40)?;
        start = u64_at(&record, 48)?;
        directory_end = position;
    }
    if count > policy.files as u64 + 1
        || size > policy.metadata_bytes
        || start.checked_add(size) != Some(directory_end)
        || length.saturating_sub(start) > policy.metadata_bytes + 65_657
    {
        return Err(files::Error::Invalid(
            "Archive index exceeds limits or has invalid offsets",
        ));
    }
    let bytes = read_at(file, start, (length - start) as usize)?;
    let mut at = 0_usize;
    let mut names = BTreeSet::new();
    for _ in 0..count {
        if bytes.get(at..at.saturating_add(4)) != Some(b"PK\x01\x02") {
            return Err(INVALID);
        }
        let name_start = at.checked_add(46).ok_or(INVALID)?;
        let name_end = name_start
            .checked_add(usize::from(u16_at(&bytes, at + 28)?))
            .ok_or(INVALID)?;
        let name = std::str::from_utf8(bytes.get(name_start..name_end).ok_or(INVALID)?)
            .map_err(|_| INVALID)?;
        directories::relative(name)?;
        if !names.insert(name) {
            return Err(files::Error::Invalid("Archive contains duplicate paths"));
        }
        let extra = usize::from(u16_at(&bytes, at + 30)?);
        let comment = usize::from(u16_at(&bytes, at + 32)?);
        at = name_end
            .checked_add(extra)
            .and_then(|value| value.checked_add(comment))
            .ok_or(INVALID)?;
        if at as u64 > size {
            return Err(INVALID);
        }
    }
    if at as u64 != size {
        return Err(INVALID);
    }
    // Every fallback footer visible to the decoder is subject to this budget.
    for (at, signature) in bytes.windows(4).enumerate() {
        if signature == b"PK\x06\x06"
            && u64_at(&bytes, at + 32).is_ok_and(|count| count > policy.files as u64 + 1)
            || signature == b"PK\x05\x06"
                && u16_at(&bytes, at + 8)
                    .is_ok_and(|count| count != u16::MAX && usize::from(count) > policy.files + 1)
        {
            return Err(files::Error::Invalid(
                "Archive recovery metadata exceeds limits",
            ));
        }
    }
    Ok((MetadataWindow { start, bytes }, count as usize))
}

pub struct Reader {
    archive: zip::ZipArchive<ArchiveFile>,
    witness: File,
    policy: Policy,
    metadata_name: String,
    initial: std::fs::Metadata,
}
impl Reader {
    pub fn open(
        path: &Path,
        policy: Policy,
        metadata_name: &str,
        check: Check<'_>,
    ) -> Result<Self> {
        policy.validate()?;
        directories::relative(metadata_name)?;
        check()?;
        let parent = files::directory(path.parent().ok_or(INVALID)?)?;
        let mut file = directories::child_file(
            &parent,
            path.file_name()
                .and_then(|name| name.to_str())
                .ok_or(INVALID)?,
            OFlag::O_RDONLY,
        )?;
        let initial = file.metadata()?;
        let (metadata, count) = index(&mut file, &policy)?;
        let start = metadata.start;
        let mut witness = file.try_clone()?;
        let window = Arc::new(Mutex::new(Some(metadata)));
        let mut archive = zip::ZipArchive::new(ArchiveFile {
            file,
            position: 0,
            length: initial.len(),
            window: Arc::clone(&window),
        })
        .map_err(zip_error)?;
        window.lock().map_err(|_| INVALID)?.take();
        if archive.len() != count || archive.central_directory_start() != start {
            return Err(INVALID);
        }
        let mut ranges = BTreeMap::<u64, u64>::new();
        let mut expanded = 0_u64;
        for index in 0..archive.len() {
            check()?;
            let mut file = archive.by_index_raw(index).map_err(zip_error)?;
            let name = file.name().to_owned();
            directories::relative(&name)?;
            if file.name_raw() != name.as_bytes()
                || !matches!(file.unix_mode().unwrap_or(0) & 0o170000, 0 | 0o100000)
            {
                return Err(files::Error::Invalid(
                    "Archive contains a link or special file",
                ));
            }
            if name == metadata_name {
                if file.size() > policy.metadata_bytes {
                    return Err(files::Error::Invalid("Archive metadata exceeds byte limit"));
                }
            } else {
                expanded = expanded.checked_add(file.size()).ok_or(INVALID)?;
                if expanded > policy.bytes {
                    return Err(files::Error::Invalid(
                        "Archive expanded byte limit exceeded",
                    ));
                }
            }
            let begin = file.header_start();
            let data = file.data_start().ok_or(INVALID)?;
            let local = read_at(&mut witness, begin, 30)?;
            let central = read_at(&mut witness, file.central_header_start(), 46)?;
            let local_name = read_at(
                &mut witness,
                begin.checked_add(30).ok_or(INVALID)?,
                usize::from(u16_at(&local, 26)?),
            )?;
            if local_name != file.name_raw()
                || u16_at(&local, 6)? != u16_at(&central, 8)?
                || u16_at(&local, 8)? != u16_at(&central, 10)?
            {
                return Err(files::Error::Invalid(
                    "Archive local header disagrees with its directory",
                ));
            }
            let end = data.checked_add(file.compressed_size()).ok_or(INVALID)?;
            if end > start
                || ranges
                    .range(..=begin)
                    .next_back()
                    .is_some_and(|(_, &previous)| previous > begin)
                || ranges
                    .range(begin..)
                    .next()
                    .is_some_and(|(&next, _)| next < end)
            {
                return Err(files::Error::Invalid("Archive file ranges overlap"));
            }
            ranges.insert(begin, end);
            // Raw reads do not instantiate a decoder. Bound LZMA dictionary first.
            if file.compression() == zip::CompressionMethod::Lzma {
                let mut header = [0; 9];
                file.read_exact(&mut header)?;
                if u16_at(&header, 2)? != 5 || u32_at(&header, 5)? > 32 * 1024 * 1024 {
                    return Err(files::Error::Invalid(
                        "Archive decoder memory exceeds limit",
                    ));
                }
            }
        }
        if archive.index_for_name(metadata_name).is_none() {
            return Err(files::Error::Invalid("Archive metadata is missing"));
        }
        Ok(Self {
            archive,
            witness,
            policy,
            metadata_name: metadata_name.to_owned(),
            initial,
        })
    }
    pub fn metadata(&mut self, check: Check<'_>) -> Result<Vec<u8>> {
        let mut file = self
            .archive
            .by_name(&self.metadata_name)
            .map_err(zip_error)?;
        let expected = file.size();
        let mut bytes = Vec::new();
        let mut buffer = vec![0; BUFFER];
        loop {
            check()?;
            let count = file.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            if bytes.len() as u64 + count as u64 > self.policy.metadata_bytes {
                return Err(files::Error::Invalid("Archive metadata exceeds byte limit"));
            }
            bytes.extend_from_slice(&buffer[..count]);
        }
        if bytes.len() as u64 != expected {
            return Err(INVALID);
        }
        Ok(bytes)
    }
    pub fn digest(&mut self, check: Check<'_>) -> Result<String> {
        let result = digest_file(&mut self.witness, check)?;
        if !same_file(&self.initial, &self.witness.metadata()?) {
            return Err(files::Error::Invalid("Archive changed during verification"));
        }
        Ok(result)
    }
    pub fn extract(
        &mut self,
        stage: &StagedDirectory,
        expected: &BTreeMap<String, Evidence>,
        directories: &[String],
        check: Check<'_>,
    ) -> Result<()> {
        if expected.len() > self.policy.files || directories.len() > self.policy.directories {
            return Err(INVALID);
        }
        let mut declared: BTreeSet<_> = expected.keys().map(String::as_str).collect();
        declared.insert(&self.metadata_name);
        let actual: BTreeSet<_> = self.archive.file_names().collect();
        if declared != actual || expected.contains_key(&self.metadata_name) {
            return Err(files::Error::Invalid(
                "Archive file list does not match metadata",
            ));
        }
        let mut total = 0_u64;
        let mut directory_names = BTreeSet::new();
        for (name, evidence) in expected {
            let parts = directories::relative(name)?;
            for end in 1..parts.len() {
                if expected.contains_key(&parts[..end].join("/")) {
                    return Err(files::Error::Invalid(
                        "Archive path conflicts with a parent file",
                    ));
                }
            }
            if evidence.sha256.len() != 64
                || !evidence
                    .sha256
                    .bytes()
                    .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
            {
                return Err(INVALID);
            }
            total = total.checked_add(evidence.bytes).ok_or(INVALID)?;
            if total > self.policy.bytes
                || self.archive.by_name(name).map_err(zip_error)?.size() != evidence.bytes
            {
                return Err(files::Error::Invalid(
                    "Archive file length does not match metadata",
                ));
            }
        }
        for name in directories {
            let parts = directories::relative(name)?;
            for end in 1..=parts.len() {
                if expected.contains_key(&parts[..end].join("/")) {
                    return Err(files::Error::Invalid(
                        "Archive directory conflicts with a file",
                    ));
                }
            }
            if !directory_names.insert(name) {
                return Err(files::Error::Invalid(
                    "Archive directories contain duplicates",
                ));
            }
        }
        // All declarations are checked before writing any staged bytes.
        for name in directories {
            check()?;
            stage.create_directory(name)?;
        }
        let mut buffer = vec![0; BUFFER];
        for (name, evidence) in expected {
            check()?;
            let mut source = self.archive.by_name(name).map_err(zip_error)?;
            let mut output = stage.create_file(name)?;
            let mut digest = Sha256::new();
            let mut size = 0_u64;
            loop {
                check()?;
                let count = source.read(&mut buffer)?;
                if count == 0 {
                    break;
                }
                size = size.checked_add(count as u64).ok_or(INVALID)?;
                if size > evidence.bytes {
                    return Err(files::Error::Invalid(
                        "Archive expanded file exceeds declared length",
                    ));
                }
                digest.update(&buffer[..count]);
                output.write_all(&buffer[..count])?;
            }
            if size != evidence.bytes || hex::encode(digest.finalize()) != evidence.sha256 {
                return Err(files::Error::Invalid(
                    "Archive checksum does not match metadata",
                ));
            }
            output.sync_all()?;
        }
        // Retain the open input identity; replacement of its pathname cannot
        // redirect extraction. Concurrent writes to that input still fail closed.
        if !same_file(&self.initial, &self.witness.metadata()?) {
            return Err(files::Error::Invalid("Archive changed during extraction"));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_archive_survives_parent_sync_failure() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("input");
        std::fs::create_dir(&source).unwrap();
        std::fs::write(source.join("data"), b"retained").unwrap();
        let target = root.path().join("result.zip");
        let policy = Policy {
            bytes: 1024,
            files: 10,
            directories: 10,
            metadata_bytes: 1024,
        };
        let mut writer = Writer::capture(
            &source,
            &target,
            &["data".into()],
            &[],
            policy.clone(),
            "index.json",
            &mut || Ok(()),
        )
        .unwrap();
        let error = writer
            .commit_with_sync(b"metadata", &mut || Ok(()), |_| {
                Err(io::Error::from_raw_os_error(Errno::EIO as i32))
            })
            .err()
            .unwrap();
        assert!(error.published());
        drop(writer);
        let mut reader = Reader::open(&target, policy, "index.json", &mut || Ok(())).unwrap();
        assert_eq!(reader.metadata(&mut || Ok(())).unwrap(), b"metadata");
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 2);
    }
}
