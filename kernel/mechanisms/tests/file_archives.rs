use asterion_kernel::{
    directories::StagedDirectory,
    file_archives::{Catalog, Evidence, Policy, Reader, Writer},
    files,
};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::{Cursor, Write},
    os::unix::fs::{PermissionsExt, symlink},
    path::Path,
};
use zip::write::SimpleFileOptions;
fn policy() -> Policy {
    Policy {
        bytes: 16 * 1024 * 1024,
        files: 100,
        directories: 100,
        metadata_bytes: 1024 * 1024,
    }
}
fn source(root: &Path) -> std::path::PathBuf {
    let source = root.join("source");
    fs::create_dir_all(source.join("objects/empty")).unwrap();
    fs::write(source.join("state.json"), b"private state").unwrap();
    fs::write(source.join("objects/报告.txt"), vec![17; 3 * 1024 * 1024]).unwrap();
    source
}
fn capture(root: &Path) -> (std::path::PathBuf, BTreeMap<String, Evidence>, Vec<String>) {
    let source = source(root);
    let archive = root.join("snapshot.zip");
    let mut writer = Writer::capture(
        &source,
        &archive,
        &["state.json".into(), "objects".into()],
        &[],
        policy(),
        "index.json",
        &mut || Ok(()),
    )
    .unwrap();
    let Catalog { files, directories } = writer.catalog().unwrap();
    let files = files.clone();
    let directories: Vec<_> = directories.iter().cloned().collect();
    let report = writer
        .commit(b"opaque host metadata", &mut || Ok(()))
        .unwrap();
    assert_eq!(report.sha256, files::file_digest(&archive).unwrap());
    assert_eq!(report.files, 2);
    (archive, files, directories)
}
#[test]
fn streaming_roundtrip_preserves_private_bytes_empty_directories_and_unicode() {
    let root = tempfile::tempdir().unwrap();
    let (archive, files, directories) = capture(root.path());
    let mut reader = Reader::open(&archive, policy(), "index.json", &mut || Ok(())).unwrap();
    assert_eq!(
        reader.metadata(&mut || Ok(())).unwrap(),
        b"opaque host metadata"
    );
    let target = root.path().join("restored");
    let mut stage = StagedDirectory::new(&target, 1000).unwrap();
    reader
        .extract(&stage, &files, &directories, &mut || Ok(()))
        .unwrap();
    assert!(!target.exists());
    let temporary = stage.path().to_owned();
    stage.commit().unwrap();
    drop(stage);
    assert!(!temporary.exists());
    assert!(target.join("objects/empty").is_dir());
    assert_eq!(
        fs::read(target.join("state.json")).unwrap(),
        b"private state"
    );
    assert_eq!(
        fs::read(target.join("objects/报告.txt")).unwrap(),
        vec![17; 3 * 1024 * 1024]
    );
    assert_eq!(
        fs::metadata(&archive).unwrap().permissions().mode() & 0o777,
        0o600
    );
    assert_eq!(
        fs::metadata(target.join("state.json"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    assert_eq!(
        reader.digest(&mut || Ok(())).unwrap(),
        files::file_digest(&archive).unwrap()
    );
}
#[test]
fn metadata_write_budget_prevents_publication_and_removes_own_temporary() {
    let root = tempfile::tempdir().unwrap();
    let source = source(root.path());
    let archive = root.path().join("snapshot.zip");
    let mut writer = Writer::capture(
        &source,
        &archive,
        &["state.json".into()],
        &[],
        policy(),
        "index.json",
        &mut || Ok(()),
    )
    .unwrap();
    assert!(
        writer
            .commit(&vec![0; 1024 * 1024 + 1], &mut || Ok(()))
            .is_err()
    );
    drop(writer);
    assert!(!archive.exists());
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 1);
}
#[test]
fn writer_uses_reader_index_budget_before_publication() {
    let root = tempfile::tempdir().unwrap();
    let source = source(root.path());
    let archive = root.path().join("snapshot.zip");
    let mut limits = policy();
    limits.metadata_bytes = 160;
    let mut writer = Writer::capture(
        &source,
        &archive,
        &["state.json".into()],
        &[],
        limits,
        &"x".repeat(150),
        &mut || Ok(()),
    )
    .unwrap();
    assert!(writer.commit(b"{}", &mut || Ok(())).is_err());
    drop(writer);
    assert!(!archive.exists());
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 1);
}
#[test]
fn source_links_changes_and_cancellation_never_publish() {
    for mode in ["link", "changed", "cancel"] {
        let root = tempfile::tempdir().unwrap();
        let source = source(root.path());
        let archive = root.path().join("snapshot.zip");
        if mode == "link" {
            symlink(root.path(), source.join("objects/link")).unwrap();
        }
        let mut calls = 0;
        let result = Writer::capture(
            &source,
            &archive,
            &["objects".into()],
            &[],
            policy(),
            "index.json",
            &mut || {
                calls += 1;
                if mode == "cancel" && calls > 2 {
                    return Err(std::io::Error::from(std::io::ErrorKind::Interrupted).into());
                }
                if mode == "changed" && calls == 6 {
                    fs::write(source.join("objects/报告.txt"), b"changed").unwrap();
                }
                Ok(())
            },
        );
        assert!(result.is_err(), "{mode}");
        assert!(!archive.exists());
        assert_eq!(fs::read_dir(root.path()).unwrap().count(), 1);
    }
}
#[test]
fn concurrent_file_target_is_retained_without_replacement() {
    let root = tempfile::tempdir().unwrap();
    let source = source(root.path());
    let archive = root.path().join("snapshot.zip");
    let mut writer = Writer::capture(
        &source,
        &archive,
        &["state.json".into()],
        &[],
        policy(),
        "index.json",
        &mut || Ok(()),
    )
    .unwrap();
    fs::write(&archive, b"existing backup").unwrap();
    assert!(writer.commit(b"metadata", &mut || Ok(())).is_err());
    drop(writer);
    assert_eq!(fs::read(&archive).unwrap(), b"existing backup");
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 2);
}
#[test]
fn concurrent_directory_target_survives_failed_commit_and_stage_cleanup() {
    let root = tempfile::tempdir().unwrap();
    let target = root.path().join("result");
    let mut stage = StagedDirectory::new(&target, 100).unwrap();
    let temporary = stage.path().to_owned();
    fs::write(temporary.join("new"), b"new").unwrap();
    fs::create_dir(&target).unwrap();
    fs::write(target.join("existing"), b"keep").unwrap();
    assert!(stage.commit().is_err());
    drop(stage);
    assert_eq!(fs::read(target.join("existing")).unwrap(), b"keep");
    assert!(!temporary.exists());
}
#[test]
fn publication_survives_later_failure_and_links_are_never_synced_or_followed() {
    let root = tempfile::tempdir().unwrap();
    let target = root.path().join("result");
    let mut stage = StagedDirectory::new(&target, 100).unwrap();
    fs::write(stage.path().join("data"), b"keep").unwrap();
    stage.commit().unwrap();
    assert!(stage.commit().is_err());
    drop(stage);
    assert_eq!(fs::read(target.join("data")).unwrap(), b"keep");
    let mut stage = StagedDirectory::new(&root.path().join("blocked"), 100).unwrap();
    symlink(&target, stage.path().join("link")).unwrap();
    assert!(stage.commit().is_err());
    drop(stage);
    assert_eq!(fs::read(target.join("data")).unwrap(), b"keep");
}
#[test]
fn extraction_evidence_failure_and_cancellation_never_publish_target() {
    for defect in ["checksum", "length", "extra", "directory", "cancel"] {
        let root = tempfile::tempdir().unwrap();
        let (archive, mut files, mut directories) = capture(root.path());
        let mut reader = Reader::open(&archive, policy(), "index.json", &mut || Ok(())).unwrap();
        match defect {
            "checksum" => files.get_mut("state.json").unwrap().sha256 = "0".repeat(64),
            "length" => files.get_mut("state.json").unwrap().bytes += 1,
            "extra" => {
                files.remove("state.json");
            }
            "directory" => directories.push("../outside".into()),
            _ => {}
        }
        let target = root.path().join("restored");
        let stage = StagedDirectory::new(&target, 1000).unwrap();
        let temporary = stage.path().to_owned();
        let mut calls = 0;
        assert!(
            reader
                .extract(&stage, &files, &directories, &mut || {
                    calls += 1;
                    if defect == "cancel" && calls > 4 {
                        Err(std::io::Error::from(std::io::ErrorKind::Interrupted).into())
                    } else {
                        Ok(())
                    }
                })
                .is_err(),
            "{defect}"
        );
        drop(stage);
        assert!(!target.exists());
        assert!(!temporary.exists());
    }
}
#[test]
fn archive_handle_remains_anchored_but_concurrent_content_change_is_rejected() {
    let root = tempfile::tempdir().unwrap();
    let (archive, files, directories) = capture(root.path());
    let input = root.path().join("input");
    fs::create_dir(&input).unwrap();
    fs::rename(&archive, input.join("snapshot.zip")).unwrap();
    let archive = input.join("snapshot.zip");
    let mut reader = Reader::open(&archive, policy(), "index.json", &mut || Ok(())).unwrap();
    let original = files::file_digest(&archive).unwrap();
    // Moving the containing directory changes path resolution without changing
    // the held file's ctime (which independently detects concurrent file writes).
    fs::rename(&input, root.path().join("retained")).unwrap();
    fs::create_dir(&input).unwrap();
    fs::write(&archive, b"replacement").unwrap();
    assert_eq!(reader.digest(&mut || Ok(())).unwrap(), original);
    let stage = StagedDirectory::new(&root.path().join("restored"), 1000).unwrap();
    reader
        .extract(&stage, &files, &directories, &mut || Ok(()))
        .unwrap();
    fs::OpenOptions::new()
        .append(true)
        .open(root.path().join("retained/snapshot.zip"))
        .unwrap()
        .write_all(b"changed")
        .unwrap();
    assert!(reader.digest(&mut || Ok(())).is_err());
}
#[test]
fn input_metadata_limit_and_bounded_decompression_reject_forged_size() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("forged.zip");
    let content = vec![5; 1024 * 1024];
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer
        .start_file(
            "data",
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored),
        )
        .unwrap();
    writer.write_all(&content).unwrap();
    writer
        .start_file("index.json", SimpleFileOptions::default())
        .unwrap();
    writer.write_all(b"metadata").unwrap();
    let mut bytes = writer.finish().unwrap().into_inner();
    let at = bytes
        .windows(4)
        .position(|part| part == b"PK\x01\x02")
        .unwrap();
    bytes[at + 24..at + 28].copy_from_slice(&1_u32.to_le_bytes());
    fs::write(&path, bytes).unwrap();
    let mut reader = Reader::open(&path, policy(), "index.json", &mut || Ok(())).unwrap();
    let evidence = BTreeMap::from([(
        "data".into(),
        Evidence {
            bytes: 1,
            sha256: hex::encode(Sha256::digest(&content)),
        },
    )]);
    let stage = StagedDirectory::new(&root.path().join("restored"), 100).unwrap();
    assert!(
        reader
            .extract(&stage, &evidence, &[], &mut || Ok(()))
            .is_err()
    );
    drop(stage);
    assert!(!root.path().join("restored").exists());
}

#[test]
fn input_admission_rejects_metadata_duplicates_links_headers_and_index_overflow() {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    for name in ["data", "same", "index.json"] {
        writer
            .start_file(name, SimpleFileOptions::default())
            .unwrap();
        writer.write_all(b"content").unwrap();
    }
    let original = writer.finish().unwrap().into_inner();
    let central: Vec<_> = original
        .windows(4)
        .enumerate()
        .filter_map(|(at, signature)| (signature == b"PK\x01\x02").then_some(at))
        .collect();
    for defect in ["duplicate", "link", "local", "range", "metadata", "count"] {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("invalid.zip");
        let mut content = original.clone();
        match defect {
            "duplicate" => {
                content[central[1] + 46..central[1] + 50].copy_from_slice(b"data");
            }
            "link" => {
                content[central[0] + 38..central[0] + 42]
                    .copy_from_slice(&(0o120777_u32 << 16).to_le_bytes());
            }
            "local" => content[30..34].copy_from_slice(b"fake"),
            "range" => {
                content[central[0] + 20..central[0] + 24]
                    .copy_from_slice(&(central[0] as u32).to_le_bytes());
            }
            "metadata" => {
                content[central[2] + 24..central[2] + 28]
                    .copy_from_slice(&(policy().metadata_bytes as u32 + 1).to_le_bytes());
            }
            "count" => {
                let footer = content.len() - 22;
                content[footer + 8..footer + 10].copy_from_slice(&102_u16.to_le_bytes());
                content[footer + 10..footer + 12].copy_from_slice(&102_u16.to_le_bytes());
            }
            _ => unreachable!(),
        }
        fs::write(&path, &content).unwrap();
        assert!(
            Reader::open(&path, policy(), "index.json", &mut || Ok(())).is_err(),
            "{defect}"
        );
        assert_eq!(fs::read(&path).unwrap(), content);
    }
}
