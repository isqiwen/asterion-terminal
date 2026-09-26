//! Bounded, data-only ZIP admission. Format policy is supplied by the host;
//! this module never interprets manifests, extension points or source code.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{Cursor, Read};

#[derive(Debug)]
pub enum Error {
    Invalid(&'static str),
    Files(crate::files::Error),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Invalid(message) => f.write_str(message),
            Self::Files(error) => write!(f, "{error}"),
        }
    }
}
impl std::error::Error for Error {}
impl From<crate::files::Error> for Error {
    fn from(error: crate::files::Error) -> Self {
        Self::Files(error)
    }
}
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::Files(error.into())
    }
}
pub type Result<T> = std::result::Result<T, Error>;
const INVALID: Error = Error::Invalid("Invalid or unsupported ZIP archive");

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Policy {
    pub compressed_bytes: u64,
    pub expanded_bytes: u64,
    pub file_bytes: u64,
    pub files: usize,
    pub suffixes: Vec<String>,
    pub required: Vec<String>,
}
impl Policy {
    pub fn validate(&self) -> Result<()> {
        if self.compressed_bytes == 0
            || self.compressed_bytes > 64_000_000
            || self.expanded_bytes == 0
            || self.expanded_bytes > 256_000_000
            || self.file_bytes == 0
            || self.file_bytes > self.expanded_bytes
            || self.files == 0
            || self.files > 10_000
            || self.suffixes.is_empty()
            || self.suffixes.len() > 64
            || self.required.len() > self.files
            || self.suffixes.iter().any(|suffix| {
                !suffix.starts_with('.')
                    || suffix.len() < 2
                    || suffix.len() > 32
                    || !suffix[1..].bytes().all(|byte| byte.is_ascii_alphanumeric())
            })
            || self.required.iter().any(|name| !safe_name(name))
        {
            return Err(Error::Invalid("Invalid bounded archive policy"));
        }
        Ok(())
    }
}
fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 4096
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"_./-".contains(&byte))
        && name.split('/').count() <= 64
        && name
            .split('/')
            .all(|part| !matches!(part, "" | "." | "..") && part.len() <= 255)
        && name != ".archive"
}
fn number<const N: usize>(content: &[u8], offset: usize) -> Result<[u8; N]> {
    content
        .get(offset..offset.saturating_add(N))
        .and_then(|value| value.try_into().ok())
        .ok_or(INVALID)
}
fn u16_at(content: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(number(content, offset)?))
}
fn u32_at(content: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(number(content, offset)?))
}
fn u64_at(content: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(number(content, offset)?))
}

fn bound_candidate_metadata(content: &[u8], limit: usize) -> Result<()> {
    // A damaged latest directory makes zip search earlier end records. Bound
    // every possible allocation source, not just the selected final record.
    // No recovery candidate may evade the same host metadata budget.
    for (position, signature) in content.windows(4).enumerate() {
        let count = if signature == b"PK\x05\x06" {
            let Ok(comment) = u16_at(content, position + 20) else {
                continue;
            };
            if position + 22 + usize::from(comment) > content.len() {
                continue;
            }
            // ZIP64 sentinels are checked through their 64-bit end record.
            u16_at(content, position + 8)
                .ok()
                .filter(|&count| count != u16::MAX)
                .map(u64::from)
        } else if signature == b"PK\x06\x06" {
            let Ok(size) = u64_at(content, position + 4) else {
                continue;
            };
            if size < 44
                || size
                    .checked_add(position as u64 + 12)
                    .is_none_or(|end| end > content.len() as u64)
            {
                continue;
            }
            u64_at(content, position + 32).ok()
        } else {
            None
        };
        if count.is_some_and(|count| count > limit as u64) {
            return Err(Error::Invalid("Archive contains too many metadata entries"));
        }
    }
    Ok(())
}

// zip's metadata map replaces duplicate names. Inspect the original directory
// before invoking it, and reject oversized counts before it allocates metadata.
fn directory(content: &[u8], policy: &Policy) -> Result<(usize, usize)> {
    let end = (content.len().saturating_sub(65_557)..content.len().saturating_sub(21))
        .rev()
        .find(|&position| {
            content.get(position..position + 4) == Some(b"PK\x05\x06")
                && u16_at(content, position + 20)
                    .is_ok_and(|length| position + 22 + usize::from(length) == content.len())
        })
        .ok_or(INVALID)?;
    if u16_at(content, end + 4)? != 0 || u16_at(content, end + 6)? != 0 {
        return Err(INVALID);
    }
    let count = u16_at(content, end + 10)?;
    if u16_at(content, end + 8)? != count {
        return Err(INVALID);
    }
    let size = u32_at(content, end + 12)?;
    let offset = u32_at(content, end + 16)?;
    let (count, size, offset, directory_end) =
        if count == u16::MAX || size == u32::MAX || offset == u32::MAX {
            let locator = end.checked_sub(20).ok_or(INVALID)?;
            if content.get(locator..locator + 4) != Some(b"PK\x06\x07")
                || u32_at(content, locator + 4)? != 0
                || u32_at(content, locator + 16)? != 1
            {
                return Err(INVALID);
            }
            let position = (0..locator.saturating_sub(55))
                .rev()
                .find(|&position| {
                    content.get(position..position + 4) == Some(b"PK\x06\x06")
                        && u64_at(content, position + 4).is_ok_and(|size| {
                            size >= 44
                                && size.checked_add(position as u64 + 12) == Some(locator as u64)
                        })
                })
                .ok_or(INVALID)?;
            if u32_at(content, position + 16)? != 0
                || u32_at(content, position + 20)? != 0
                || u64_at(content, position + 24)? != u64_at(content, position + 32)?
            {
                return Err(INVALID);
            }
            (
                u64_at(content, position + 32)?,
                u64_at(content, position + 40)?,
                u64_at(content, position + 48)?,
                position,
            )
        } else {
            (u64::from(count), u64::from(size), u64::from(offset), end)
        };
    if count > policy.files as u64 {
        return Err(Error::Invalid("Archive contains too many files"));
    }
    let size = usize::try_from(size).map_err(|_| INVALID)?;
    let start = directory_end.checked_sub(size).ok_or(INVALID)?;
    if offset > start as u64 {
        return Err(INVALID);
    }
    let mut position = start;
    let mut names = BTreeSet::new();
    for _ in 0..count {
        if content.get(position..position.saturating_add(4)) != Some(b"PK\x01\x02") {
            return Err(INVALID);
        }
        let name_length = usize::from(u16_at(content, position + 28)?);
        let name_start = position.checked_add(46).ok_or(INVALID)?;
        let name_end = name_start.checked_add(name_length).ok_or(INVALID)?;
        let name = std::str::from_utf8(content.get(name_start..name_end).ok_or(INVALID)?)
            .map_err(|_| INVALID)?;
        if !safe_name(name) || !names.insert(name) {
            return Err(Error::Invalid(
                "Archive contains an unsafe or duplicate path",
            ));
        }
        position = name_end
            .checked_add(usize::from(u16_at(content, position + 30)?))
            .and_then(|value| value.checked_add(usize::from(u16_at(content, position + 32).ok()?)))
            .ok_or(INVALID)?;
        if position > directory_end {
            return Err(INVALID);
        }
    }
    if position != directory_end {
        return Err(INVALID);
    }
    Ok((start, count as usize))
}

#[derive(Debug)]
pub struct CheckedArchive {
    pub(crate) original: Vec<u8>,
    pub(crate) files: BTreeMap<String, Vec<u8>>,
    digest: String,
}
impl CheckedArchive {
    pub fn check(content: &[u8], policy: &Policy) -> Result<Self> {
        policy.validate()?;
        if content.len() as u64 > policy.compressed_bytes {
            return Err(Error::Invalid("Archive exceeds compressed byte limit"));
        }
        bound_candidate_metadata(content, policy.files)?;
        let (start, count) = directory(content, policy)?;
        let mut archive = zip::ZipArchive::new(Cursor::new(content)).map_err(|_| INVALID)?;
        if archive.len() != count || archive.central_directory_start() != start as u64 {
            return Err(INVALID);
        }
        let mut files = BTreeMap::new();
        let mut ranges = Vec::<std::ops::Range<u64>>::new();
        let mut expanded = 0_u64;
        for index in 0..archive.len() {
            let mut file = archive.by_index(index).map_err(|_| INVALID)?;
            let name = file.name().to_owned();
            let local = usize::try_from(file.header_start()).map_err(|_| INVALID)?;
            let central = usize::try_from(file.central_header_start()).map_err(|_| INVALID)?;
            let name_start = local.checked_add(30).ok_or(INVALID)?;
            let name_end = name_start
                .checked_add(usize::from(u16_at(content, local + 26)?))
                .ok_or(INVALID)?;
            if content.get(name_start..name_end) != Some(file.name_raw())
                || u16_at(content, local + 6)? != u16_at(content, central + 8)?
                || u16_at(content, local + 8)? != u16_at(content, central + 10)?
            {
                return Err(Error::Invalid(
                    "Archive local header disagrees with its directory",
                ));
            }
            let end = file
                .data_start()
                .and_then(|offset| offset.checked_add(file.compressed_size()))
                .ok_or(INVALID)?;
            let range = file.header_start()..end;
            if end > start as u64
                || ranges
                    .iter()
                    .any(|other| range.start < other.end && other.start < range.end)
            {
                return Err(Error::Invalid(
                    "Archive entries overlap or exceed the content boundary",
                ));
            }
            ranges.push(range);
            // ZIP LZMA embeds its dictionary allocation in the compressed
            // stream, independently of the declared expanded size.
            if file.compression() == zip::CompressionMethod::Lzma {
                let start =
                    usize::try_from(file.data_start().ok_or(INVALID)?).map_err(|_| INVALID)?;
                if file.compressed_size() < 9
                    || u16_at(content, start.checked_add(2).ok_or(INVALID)?)? != 5
                    || u32_at(content, start.checked_add(5).ok_or(INVALID)?)? > 32 * 1024 * 1024
                {
                    return Err(Error::Invalid("Archive decoder memory exceeds limit"));
                }
            }
            let kind = file.unix_mode().unwrap_or(0) & 0o170000;
            if !safe_name(&name)
                || file.name_raw() != name.as_bytes()
                || !matches!(kind, 0 | 0o100000)
            {
                return Err(Error::Invalid(
                    "Archive contains a link, special file or unsafe path",
                ));
            }
            if !policy.suffixes.iter().any(|suffix| name.ends_with(suffix)) {
                return Err(Error::Invalid("Archive contains an unsupported file type"));
            }
            if file.size() > policy.file_bytes {
                return Err(Error::Invalid("Archive file exceeds byte limit"));
            }
            expanded = expanded.checked_add(file.size()).ok_or(INVALID)?;
            if expanded > policy.expanded_bytes {
                return Err(Error::Invalid("Archive exceeds expanded byte limit"));
            }
            let expected = file.size();
            let mut content = Vec::new();
            (&mut file)
                .take(policy.file_bytes + 1)
                .read_to_end(&mut content)
                .map_err(|_| INVALID)?;
            if content.len() as u64 != expected {
                return Err(Error::Invalid(
                    "Archive file size does not match its content",
                ));
            }
            if files.insert(name, content).is_some() {
                return Err(Error::Invalid("Archive contains duplicate paths"));
            }
        }
        for name in files.keys() {
            for (position, _) in name.match_indices('/') {
                if files.contains_key(&name[..position]) {
                    return Err(Error::Invalid("Archive paths conflict with a parent file"));
                }
            }
        }
        if policy.required.iter().any(|name| !files.contains_key(name)) {
            return Err(Error::Invalid("Archive is missing a required file"));
        }
        Ok(Self {
            original: content.to_vec(),
            files,
            digest: hex::encode(Sha256::digest(content)),
        })
    }
    pub fn digest(&self) -> &str {
        &self.digest
    }
    pub fn names(&self) -> Vec<String> {
        self.files.keys().cloned().collect()
    }
    pub fn read(&self, name: &str) -> Result<&[u8]> {
        self.files
            .get(name)
            .map(Vec::as_slice)
            .ok_or(Error::Invalid("Archive file does not exist"))
    }
}
