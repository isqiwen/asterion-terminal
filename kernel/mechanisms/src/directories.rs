//! Owned private staging and durable, no-replace directory publication.
use crate::files::{self, Error};
use nix::dir::Dir;
use nix::errno::Errno;
use nix::fcntl::{AtFlags, OFlag, openat};
use nix::sys::stat::{Mode, SFlag, fstatat, mkdirat};
use nix::unistd::{UnlinkatFlags, unlinkat};
use rustix::fs::{RenameFlags, renameat_with};
use std::fs::File;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

type Result<T> = std::result::Result<T, Error>;
const MAX_ENTRIES: usize = 2_000_001;
const DIRECTORY: OFlag = OFlag::O_RDONLY
    .union(OFlag::O_DIRECTORY)
    .union(OFlag::O_NOFOLLOW)
    .union(OFlag::O_CLOEXEC);

pub(crate) fn relative(name: &str) -> Result<Vec<&str>> {
    let parts: Vec<_> = name.split('/').collect();
    if name.is_empty()
        || name.len() > 4096
        || name.contains(['\\', '\0'])
        || parts.len() > 128
        || parts
            .iter()
            .any(|part| matches!(*part, "" | "." | "..") || part.len() > 255)
    {
        return Err(Error::Invalid("Invalid relative file path"));
    }
    Ok(parts)
}
pub(crate) fn child_file(parent: &File, name: &str, flags: OFlag) -> Result<File> {
    let file = File::from(openat(
        parent,
        name,
        flags | OFlag::O_NOFOLLOW | OFlag::O_CLOEXEC | OFlag::O_NONBLOCK,
        Mode::S_IRUSR | Mode::S_IWUSR,
    )?);
    if !file.metadata()?.is_file() {
        return Err(Error::Invalid("A regular file is required"));
    }
    Ok(file)
}
pub(crate) fn entries(directory: &File) -> Result<Vec<String>> {
    let mut stream = Dir::openat(directory, ".", DIRECTORY, Mode::empty())?;
    let mut names = Vec::new();
    for entry in stream.iter() {
        let entry = entry?;
        let name = entry
            .file_name()
            .to_str()
            .map_err(|_| Error::Invalid("File names must be UTF-8"))?;
        if !matches!(name, "." | "..") {
            names.push(name.to_owned());
        }
        if names.len() > MAX_ENTRIES {
            return Err(Error::Invalid("Directory entry limit exceeded"));
        }
    }
    names.sort();
    Ok(names)
}
pub(crate) fn kind(directory: &File, name: &str) -> Result<SFlag> {
    Ok(SFlag::from_bits_truncate(
        fstatat(directory, name, AtFlags::AT_SYMLINK_NOFOLLOW)?.st_mode,
    ))
}
fn visit(
    directory: &File,
    depth: usize,
    remaining: &mut usize,
    remove: bool,
    check: &mut dyn FnMut() -> Result<()>,
) -> Result<()> {
    if depth >= 128 {
        return Err(Error::Invalid("Directory depth limit exceeded"));
    }
    for name in entries(directory)? {
        check()?;
        *remaining = remaining
            .checked_sub(1)
            .ok_or(Error::Invalid("Directory entry limit exceeded"))?;
        match kind(directory, &name)? {
            SFlag::S_IFDIR => {
                visit(
                    &files::child_directory(directory, Path::new(&name))?,
                    depth + 1,
                    remaining,
                    remove,
                    check,
                )?;
                if remove {
                    unlinkat(directory, name.as_str(), UnlinkatFlags::RemoveDir)?;
                }
            }
            SFlag::S_IFREG => {
                if remove {
                    unlinkat(directory, name.as_str(), UnlinkatFlags::NoRemoveDir)?;
                } else {
                    child_file(directory, &name, OFlag::O_RDONLY)?.sync_all()?;
                }
            }
            _ if remove => {
                unlinkat(directory, name.as_str(), UnlinkatFlags::NoRemoveDir)?;
            }
            _ => return Err(Error::Invalid("Staging contains a link or special file")),
        }
    }
    if !remove {
        directory.sync_all()?;
    }
    Ok(())
}

pub struct StagedDirectory {
    parent: File,
    root: File,
    temporary: String,
    destination: String,
    path: PathBuf,
    limit: usize,
    published: bool,
    retained: bool,
}
impl StagedDirectory {
    pub fn new(destination: &Path, max_entries: usize) -> Result<Self> {
        if max_entries == 0 || max_entries > MAX_ENTRIES {
            return Err(Error::Invalid("Invalid staging entry limit"));
        }
        let name = destination
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or(Error::Invalid("Invalid directory destination"))?;
        if relative(name)?.len() != 1 {
            return Err(Error::Invalid("Invalid directory destination"));
        }
        let parent_path = destination
            .parent()
            .ok_or(Error::Invalid("A destination parent is required"))?;
        let parent = files::directory(parent_path)?;
        match fstatat(&parent, name, AtFlags::AT_SYMLINK_NOFOLLOW) {
            Ok(_) => return Err(io::Error::from(io::ErrorKind::AlreadyExists).into()),
            Err(Errno::ENOENT) => {}
            Err(error) => return Err(error.into()),
        }
        let temporary = format!(".asterion-stage-{}", uuid::Uuid::new_v4().simple());
        mkdirat(&parent, temporary.as_str(), Mode::S_IRWXU)?;
        let root = files::child_directory(&parent, Path::new(&temporary))?;
        Ok(Self {
            parent,
            root,
            path: parent_path.join(&temporary),
            temporary,
            destination: name.to_owned(),
            limit: max_entries,
            published: false,
            retained: false,
        })
    }
    pub fn path(&self) -> &Path {
        &self.path
    }
    /// Relinquish cleanup ownership when an external user of this private tree
    /// cannot safely be stopped. Consuming the handle also closes its descriptors.
    pub fn preserve(mut self) -> Result<PathBuf> {
        self.root()?;
        self.retained = true;
        Ok(self.path.clone())
    }
    pub(crate) fn root(&self) -> Result<&File> {
        if self.published {
            return Err(Error::Invalid("Staging is already published"));
        }
        Ok(&self.root)
    }
    pub(crate) fn directory(&self, parts: &[&str]) -> Result<File> {
        let mut current = self.root()?.try_clone()?;
        for part in parts {
            match mkdirat(&current, *part, Mode::S_IRWXU) {
                Ok(()) | Err(Errno::EEXIST) => {}
                Err(error) => return Err(error.into()),
            }
            current = files::child_directory(&current, Path::new(part))?;
        }
        Ok(current)
    }
    pub(crate) fn create_directory(&self, name: &str) -> Result<()> {
        self.directory(&relative(name)?)?;
        Ok(())
    }
    pub(crate) fn create_file(&self, name: &str) -> Result<File> {
        let parts = relative(name)?;
        let parent = self.directory(&parts[..parts.len() - 1])?;
        child_file(
            &parent,
            parts[parts.len() - 1],
            OFlag::O_WRONLY | OFlag::O_CREAT | OFlag::O_EXCL,
        )
    }
    fn still_owned(&self) -> Result<()> {
        let current =
            files::child_directory(&self.parent, Path::new(&self.temporary))?.metadata()?;
        let own = self.root.metadata()?;
        if current.dev() != own.dev() || current.ino() != own.ino() {
            return Err(Error::Invalid("Staging directory identity has changed"));
        }
        Ok(())
    }

    pub fn commit(&mut self) -> Result<()> {
        self.commit_checked(&mut || Ok(()))
    }
    pub fn commit_checked(&mut self, check: &mut dyn FnMut() -> Result<()>) -> Result<()> {
        self.commit_with_sync(check, File::sync_all)
    }
    fn commit_with_sync(
        &mut self,
        check: &mut dyn FnMut() -> Result<()>,
        sync_parent: impl FnOnce(&File) -> io::Result<()>,
    ) -> Result<()> {
        self.root()?;
        self.still_owned()?;
        let mut remaining = self.limit;
        visit(&self.root, 0, &mut remaining, false, check)?;
        check()?;
        self.still_owned()?;
        renameat_with(
            &self.parent,
            self.temporary.as_str(),
            &self.parent,
            self.destination.as_str(),
            RenameFlags::NOREPLACE,
        )
        .map_err(io::Error::from)?;
        // A following directory fsync failure must never remove a published tree.
        self.published = true;
        sync_parent(&self.parent).map_err(|error| Error::Published(Box::new(error.into())))?;
        Ok(())
    }
}

impl Drop for StagedDirectory {
    fn drop(&mut self) {
        if self.published || self.retained || self.still_owned().is_err() {
            return;
        }
        let mut remaining = MAX_ENTRIES;
        let _ = visit(&self.root, 0, &mut remaining, true, &mut || Ok(()));
        let _ = unlinkat(
            &self.parent,
            self.temporary.as_str(),
            UnlinkatFlags::RemoveDir,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_directory_survives_parent_sync_failure() {
        let root = tempfile::tempdir().unwrap();
        let target = root.path().join("result");
        let mut stage = StagedDirectory::new(&target, 10).unwrap();
        std::fs::write(stage.path().join("data"), b"published").unwrap();
        let error = stage
            .commit_with_sync(&mut || Ok(()), |_| {
                Err(io::Error::from_raw_os_error(Errno::EIO as i32))
            })
            .unwrap_err();
        assert!(error.published());
        drop(stage);
        assert_eq!(std::fs::read(target.join("data")).unwrap(), b"published");
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 1);
    }

    #[test]
    fn preserving_private_staging_releases_ownership_without_publication() {
        let root = tempfile::tempdir().unwrap();
        let target = root.path().join("result");
        let stage = StagedDirectory::new(&target, 10).unwrap();
        let original = stage.path().to_owned();
        std::fs::write(original.join("data"), b"retained").unwrap();
        let retained = stage.preserve().unwrap();
        assert_eq!(retained, original);
        assert!(!target.exists());
        assert_eq!(std::fs::read(retained.join("data")).unwrap(), b"retained");
        assert_eq!(retained.metadata().unwrap().mode() & 0o777, 0o700);
    }
}
