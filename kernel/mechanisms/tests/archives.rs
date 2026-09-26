use asterion_kernel::archive::{CheckedArchive, Policy};
use asterion_kernel::artifacts;
use sha2::{Digest, Sha256};
use std::io::{Cursor, Write};
use std::os::unix::fs::{PermissionsExt, symlink};
use zip::write::SimpleFileOptions;

fn policy() -> Policy {
    Policy {
        compressed_bytes: 16_000_000,
        expanded_bytes: 32_000_000,
        file_bytes: 8_000_000,
        files: 500,
        suffixes: vec![".py".into(), ".json".into(), ".txt".into()],
        required: vec!["manifest.json".into()],
    }
}
fn archive(files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    for (name, content) in files {
        writer
            .start_file(
                *name,
                SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored),
            )
            .unwrap();
        writer.write_all(content).unwrap();
    }
    writer.finish().unwrap().into_inner()
}
fn content() -> Vec<u8> {
    archive(&[("manifest.json", b"{}"), ("src/code.py", b"pass\n")])
}
fn central(bytes: &[u8]) -> usize {
    bytes
        .windows(4)
        .position(|part| part == b"PK\x01\x02")
        .unwrap()
}
fn footer(bytes: &[u8]) -> usize {
    bytes
        .windows(4)
        .rposition(|part| part == b"PK\x05\x06")
        .unwrap()
}
#[test]
fn admission_preserves_original_bytes_digest_and_nested_files() {
    let content = content();
    let checked = CheckedArchive::check(&content, &policy()).unwrap();
    assert_eq!(checked.digest(), hex::encode(Sha256::digest(&content)));
    assert_eq!(checked.read("src/code.py").unwrap(), b"pass\n");
    assert_eq!(checked.names(), ["manifest.json", "src/code.py"]);
}
#[test]
fn rejects_unsafe_names_shapes_and_conflicting_parent_paths() {
    for name in [
        "../evil.py",
        "/evil.py",
        "dir//evil.py",
        "dir/./evil.py",
        "evil\\file.py",
        "native.so",
        "space file.py",
        ".archive",
    ] {
        assert!(
            CheckedArchive::check(
                &archive(&[("manifest.json", b"{}"), (name, b"x")]),
                &policy()
            )
            .is_err(),
            "{name}"
        );
    }
    assert!(
        CheckedArchive::check(
            &archive(&[("manifest.json", b"{}"), ("x.py", b""), ("x.py/y.py", b"")]),
            &policy()
        )
        .is_err()
    );
}
#[test]
fn rejects_duplicate_original_central_directory_entries() {
    let mut bytes = archive(&[("manifest.json", b"{}")]);
    let start = central(&bytes);
    let end = footer(&bytes);
    let entry = bytes[start..end].to_vec();
    bytes.splice(end..end, entry.clone());
    let end = footer(&bytes);
    bytes[end + 8..end + 12].copy_from_slice(&[2, 0, 2, 0]);
    bytes[end + 12..end + 16].copy_from_slice(&(entry.len() as u32 * 2).to_le_bytes());
    assert!(
        CheckedArchive::check(&bytes, &policy())
            .unwrap_err()
            .to_string()
            .contains("duplicate")
    );
}
#[test]
fn rejects_declared_and_actual_resource_overflow_before_publication() {
    let bytes = content();
    let mut limits = policy();
    limits.compressed_bytes = bytes.len() as u64 - 1;
    assert!(CheckedArchive::check(&bytes, &limits).is_err());
    limits = policy();
    limits.files = 1;
    assert!(CheckedArchive::check(&bytes, &limits).is_err());
    limits = policy();
    limits.file_bytes = 4;
    assert!(CheckedArchive::check(&bytes, &limits).is_err());
    limits = policy();
    limits.expanded_bytes = 6;
    limits.file_bytes = 6;
    assert!(CheckedArchive::check(&bytes, &limits).is_err());
    let mut bytes = archive(&[("manifest.json", b"{}")]);
    let end = footer(&bytes);
    bytes[end + 8..end + 12].copy_from_slice(&[254, 255, 254, 255]);
    assert!(
        CheckedArchive::check(&bytes, &policy())
            .unwrap_err()
            .to_string()
            .contains("too many")
    );
}
#[test]
fn bounds_earlier_recovery_metadata_before_zip_library_can_allocate() {
    let mut embedded = vec![0_u8; 64];
    embedded[..4].copy_from_slice(b"PK\x05\x06");
    embedded[8..12].copy_from_slice(&[254, 255, 254, 255]);
    let bytes = archive(&[("manifest.json", b"{}"), ("data.txt", &embedded)]);
    assert!(
        CheckedArchive::check(&bytes, &policy())
            .unwrap_err()
            .to_string()
            .contains("metadata")
    );
}

#[test]
fn rejects_links_special_modes_corrupt_crc_and_malformed_archives() {
    for mode in [0o120777_u32, 0o010600, 0o060600, 0o040700] {
        let mut bytes = content();
        let start = central(&bytes);
        bytes[start + 5] = 3;
        bytes[start + 38..start + 42].copy_from_slice(&(mode << 16).to_le_bytes());
        assert!(CheckedArchive::check(&bytes, &policy()).is_err());
    }
    let mut bytes = content();
    let first_data = 30 + "manifest.json".len();
    bytes[first_data] ^= 1;
    assert!(CheckedArchive::check(&bytes, &policy()).is_err());
    for bytes in [b"".as_slice(), b"not an archive", &content()[..20]] {
        assert!(CheckedArchive::check(bytes, &policy()).is_err());
    }
}
#[test]
fn accepts_bounded_zip64_metadata_without_rewriting() {
    let mut bytes = content();
    let end = footer(&bytes);
    let start = central(&bytes);
    let mut extra = Vec::new();
    extra.extend_from_slice(b"PK\x06\x06");
    extra.extend_from_slice(&44_u64.to_le_bytes());
    extra.extend_from_slice(&45_u16.to_le_bytes());
    extra.extend_from_slice(&45_u16.to_le_bytes());
    extra.extend_from_slice(&0_u32.to_le_bytes());
    extra.extend_from_slice(&0_u32.to_le_bytes());
    extra.extend_from_slice(&2_u64.to_le_bytes());
    extra.extend_from_slice(&2_u64.to_le_bytes());
    extra.extend_from_slice(&((end - start) as u64).to_le_bytes());
    extra.extend_from_slice(&(start as u64).to_le_bytes());
    extra.extend_from_slice(b"PK\x06\x07");
    extra.extend_from_slice(&0_u32.to_le_bytes());
    extra.extend_from_slice(&(end as u64).to_le_bytes());
    extra.extend_from_slice(&1_u32.to_le_bytes());
    bytes.splice(end..end, extra);
    let end = footer(&bytes);
    bytes[end + 8..end + 20].fill(255);
    assert_eq!(
        CheckedArchive::check(&bytes, &policy())
            .unwrap()
            .read("src/code.py")
            .unwrap(),
        b"pass\n"
    );
}
#[test]
fn immutable_publication_is_private_durable_and_idempotent() {
    let root = tempfile::tempdir().unwrap();
    let bytes = content();
    let checked = CheckedArchive::check(&bytes, &policy()).unwrap();
    artifacts::publish(root.path(), &checked).unwrap();
    artifacts::publish(root.path(), &checked).unwrap();
    let target = root.path().join(checked.digest());
    assert_eq!(std::fs::read(target.join(".archive")).unwrap(), bytes);
    assert_eq!(
        std::fs::metadata(target.join("src/code.py"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    assert_eq!(
        std::fs::metadata(&target).unwrap().permissions().mode() & 0o777,
        0o700
    );
    assert_eq!(
        artifacts::load(root.path(), checked.digest(), &policy())
            .unwrap()
            .digest(),
        checked.digest()
    );
    assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 1);
}
#[test]
fn concurrent_publication_never_replaces_existing_object() {
    let root = tempfile::tempdir().unwrap();
    std::thread::scope(|scope| {
        let handles: Vec<_> = (0..6)
            .map(|_| {
                scope.spawn(|| {
                    let checked = CheckedArchive::check(&content(), &policy()).unwrap();
                    artifacts::publish(root.path(), &checked).unwrap();
                })
            })
            .collect();
        for handle in handles {
            handle.join().unwrap();
        }
    });
    assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 1);
}
#[test]
fn object_validation_failure_preserves_all_existing_bytes() {
    for defect in ["content", "extra", "missing", "link", "archive"] {
        let root = tempfile::tempdir().unwrap();
        let checked = CheckedArchive::check(&content(), &policy()).unwrap();
        artifacts::publish(root.path(), &checked).unwrap();
        let target = root.path().join(checked.digest());
        match defect {
            "content" => std::fs::write(target.join("src/code.py"), b"changed").unwrap(),
            "extra" => std::fs::write(target.join("extra.txt"), b"keep").unwrap(),
            "missing" => std::fs::remove_file(target.join("src/code.py")).unwrap(),
            "link" => symlink(target.join("manifest.json"), target.join("linked.json")).unwrap(),
            "archive" => std::fs::write(target.join(".archive"), b"changed").unwrap(),
            _ => unreachable!(),
        }
        assert!(
            artifacts::load(root.path(), checked.digest(), &policy()).is_err(),
            "{defect}"
        );
        assert!(
            artifacts::publish(root.path(), &checked).is_err(),
            "{defect}"
        );
        assert!(target.exists());
        if defect == "content" {
            assert_eq!(
                std::fs::read(target.join("src/code.py")).unwrap(),
                b"changed"
            );
        }
        if defect == "extra" {
            assert_eq!(std::fs::read(target.join("extra.txt")).unwrap(), b"keep");
        }
    }
}
#[test]
fn publication_rejects_existing_empty_directory_and_links_without_repair() {
    let root = tempfile::tempdir().unwrap();
    let checked = CheckedArchive::check(&content(), &policy()).unwrap();
    let target = root.path().join(checked.digest());
    std::fs::create_dir(&target).unwrap();
    assert!(artifacts::publish(root.path(), &checked).is_err());
    assert_eq!(std::fs::read_dir(&target).unwrap().count(), 0);
    let linked = root.path().join("linked");
    symlink(root.path(), &linked).unwrap();
    assert!(artifacts::publish(&linked, &checked).is_err());
    assert!(artifacts::load(root.path(), "../bad", &policy()).is_err());
}

#[test]
fn python_zip_codecs_are_accepted_and_lzma_dictionary_is_bounded() {
    let archives = [
        "504b03041400000000000000210043bfa6a302000000020000000d0000006d616e69666573742e6a736f6e7b7d504b010214031400000000000000210043bfa6a302000000020000000d00000000000000000000008001000000006d616e69666573742e6a736f6e504b050600000000010001003b0000002d0000000000",
        "504b03041400000008000000210043bfa6a304000000020000000d0000006d616e69666573742e6a736f6eabae0500504b010214031400000008000000210043bfa6a304000000020000000d00000000000000000000008001000000006d616e69666573742e6a736f6e504b050600000000010001003b0000002f0000000000",
        "504b03042e0000000c000000210043bfa6a325000000020000000d0000006d616e69666573742e6a736f6e425a6839314159265359a726dd4e0000000080000a2000210082b177245385090a726dd4e0504b01022e032e0000000c000000210043bfa6a325000000020000000d00000000000000000000008001000000006d616e69666573742e6a736f6e504b050600000000010001003b000000500000000000",
        "504b03043f0002000e000000210043bfa6a315000000020000000d0000006d616e69666573742e6a736f6e090405005d00008000003d9f5cfffffffff0000000504b01023f033f0002000e000000210043bfa6a315000000020000000d00000000000000000000008001000000006d616e69666573742e6a736f6e504b050600000000010001003b000000400000000000",
    ];
    for (index, content) in archives.iter().enumerate() {
        let mut bytes = hex::decode(content).unwrap();
        assert_eq!(
            CheckedArchive::check(&bytes, &policy())
                .unwrap()
                .read("manifest.json")
                .unwrap(),
            b"{}"
        );
        if index == 3 {
            let data_start = 30 + "manifest.json".len();
            bytes[data_start + 5..data_start + 9].copy_from_slice(&(1_u32 << 30).to_le_bytes());
            assert!(
                CheckedArchive::check(&bytes, &policy())
                    .unwrap_err()
                    .to_string()
                    .contains("decoder memory")
            );
        }
    }
}

#[test]
fn rejects_local_header_name_different_from_central_directory() {
    let mut bytes = content();
    bytes[30..43].copy_from_slice(b"../escape1.py");
    assert!(CheckedArchive::check(&bytes, &policy()).is_err());
    for offset in [6, 8] {
        let mut bytes = content();
        bytes[offset] ^= 2;
        assert!(CheckedArchive::check(&bytes, &policy()).is_err());
    }
}

#[test]
fn rejects_compressed_ranges_overlapping_another_entry() {
    let inner = archive(&[("inner.txt", b"text")]);
    let mut bytes = archive(&[
        ("manifest.json", b"{}"),
        ("outer.txt", &inner),
        ("inner.txt", b"text"),
    ]);
    let mut parsed = zip::ZipArchive::new(Cursor::new(&bytes)).unwrap();
    let offset = parsed.by_name("outer.txt").unwrap().data_start().unwrap();
    let central = parsed.by_name("inner.txt").unwrap().central_header_start() as usize;
    drop(parsed);
    bytes[central + 42..central + 46].copy_from_slice(&(offset as u32).to_le_bytes());
    assert!(CheckedArchive::check(&bytes, &policy()).is_err());
}
