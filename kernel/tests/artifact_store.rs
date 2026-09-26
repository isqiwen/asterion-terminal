use asterion_kernel::artifacts::{ArtifactRef, ArtifactStore};
use asterion_kernel::files::Error;
use sha2::{Digest, Sha256};
use std::fs;
use std::os::unix::fs::{PermissionsExt, symlink};

fn sha(content: &[u8]) -> String {
    hex::encode(Sha256::digest(content))
}

#[test]
fn write_once_round_trip_and_identical_retry() {
    let temp = tempfile::tempdir().unwrap();
    let store = ArtifactStore::new(temp.path(), 1024).unwrap();
    let reference = store.put("imports/a/source.csv", b"x,y\n").unwrap();
    assert_eq!(
        reference,
        ArtifactRef {
            name: "imports/a/source.csv".into(),
            sha256: sha(b"x,y\n"),
            bytes: 4
        }
    );
    let mode = fs::metadata(temp.path().join("imports/a"))
        .unwrap()
        .permissions()
        .mode();
    assert_eq!(mode & 0o777, 0o700);
    assert_eq!(
        store.put("imports/a/source.csv", b"x,y\n").unwrap(),
        reference
    );
    assert_eq!(
        store
            .read(&reference.name, &reference.sha256, Some(reference.bytes))
            .unwrap(),
        b"x,y\n"
    );
    assert_eq!(
        store
            .read(&reference.name, &reference.sha256, None)
            .unwrap(),
        b"x,y\n"
    );
}

#[test]
fn existing_name_with_other_content_is_never_replaced() {
    let temp = tempfile::tempdir().unwrap();
    let store = ArtifactStore::new(temp.path(), 1024).unwrap();
    store.put("published/fixed.parquet", b"first").unwrap();
    assert!(matches!(
        store.put("published/fixed.parquet", b"second"),
        Err(Error::Invalid(
            "Immutable artifact already exists with different content"
        ))
    ));
    assert_eq!(
        fs::read(temp.path().join("published/fixed.parquet")).unwrap(),
        b"first"
    );
    assert_eq!(
        fs::read_dir(temp.path().join("published")).unwrap().count(),
        1
    );
}

#[test]
fn addressed_names_separate_changed_retries() {
    let temp = tempfile::tempdir().unwrap();
    let store = ArtifactStore::new(temp.path(), 1024).unwrap();
    let first = store
        .put_addressed("datasets/job/evidence", ".json", b"{}")
        .unwrap();
    let second = store
        .put_addressed("datasets/job/evidence", ".json", b"[]")
        .unwrap();
    assert_eq!(
        first.name,
        format!("datasets/job/evidence/{}.json", sha(b"{}"))
    );
    assert_ne!(first.name, second.name);
    assert_eq!(
        store
            .put_addressed("datasets/job/evidence", ".json", b"{}")
            .unwrap(),
        first
    );
    for suffix in ["/x", "a\\b", "\0"] {
        assert!(store.put_addressed("datasets", suffix, b"{}").is_err());
    }
}

#[test]
fn reads_reject_tampering_size_mismatch_and_missing_files() {
    let temp = tempfile::tempdir().unwrap();
    let store = ArtifactStore::new(temp.path(), 1024).unwrap();
    let reference = store.put("artifacts/part.parquet", b"content").unwrap();
    assert!(matches!(
        store.read(&reference.name, &reference.sha256, Some(3)),
        Err(Error::Invalid("Artifact size mismatch"))
    ));
    fs::write(temp.path().join("artifacts/part.parquet"), b"changed").unwrap();
    assert!(matches!(
        store.read(&reference.name, &reference.sha256, Some(7)),
        Err(Error::Invalid("Artifact checksum mismatch"))
    ));
    assert!(matches!(
        store.read("artifacts/absent.parquet", &reference.sha256, None),
        Err(Error::Io(error)) if error.kind() == std::io::ErrorKind::NotFound
    ));
    assert!(store.read(&reference.name, "ABC", None).is_err());
}

#[test]
fn names_links_and_budgets_stay_inside_the_grant() {
    let temp = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let root = temp.path().join("data");
    fs::create_dir(&root).unwrap();
    let store = ArtifactStore::new(&root, 8).unwrap();
    for name in ["", "/abs", "../escape", "a//b", "a/./b", "a\\b"] {
        assert!(store.put(name, b"x").is_err(), "{name}");
    }
    symlink(outside.path(), root.join("linked")).unwrap();
    assert!(matches!(
        store.put("linked/file", b"x"),
        Err(Error::Invalid("File links are not allowed"))
    ));
    fs::write(outside.path().join("target"), b"x").unwrap();
    symlink(outside.path().join("target"), root.join("file")).unwrap();
    assert!(store.read("file", &sha(b"x"), None).is_err());
    assert!(fs::read_dir(outside.path()).unwrap().count() == 1);
    assert!(matches!(
        store.put("big", b"123456789"),
        Err(Error::Invalid("Artifact exceeds granted size limit"))
    ));
    fs::write(root.join("large"), b"123456789").unwrap();
    assert!(store.read("large", &sha(b"123456789"), None).is_err());
    assert!(ArtifactStore::new(&root, 0).is_err());
}

#[test]
fn reader_grants_verify_and_read_but_never_write() {
    let temp = tempfile::tempdir().unwrap();
    let writer = ArtifactStore::new(temp.path(), 1024).unwrap();
    let reference = writer.put("artifacts/part.parquet", b"content").unwrap();
    let reader = ArtifactStore::reader(temp.path(), 1024).unwrap();
    assert!(matches!(
        reader.put("artifacts/other.parquet", b"x"),
        Err(Error::Invalid("Artifact grant is read-only"))
    ));
    assert!(matches!(
        reader.put_addressed("artifacts", ".parquet", b"x"),
        Err(Error::Invalid("Artifact grant is read-only"))
    ));
    assert!(!temp.path().join("artifacts/other.parquet").exists());
    reader
        .verify(&reference.name, &reference.sha256, Some(reference.bytes))
        .unwrap();
    assert_eq!(
        reader
            .read(&reference.name, &reference.sha256, None)
            .unwrap(),
        b"content"
    );
    assert!(matches!(
        reader.verify(&reference.name, &reference.sha256, Some(8)),
        Err(Error::Invalid("Artifact size mismatch"))
    ));
    fs::write(temp.path().join(&reference.name), b"changed").unwrap();
    assert!(matches!(
        reader.verify(&reference.name, &reference.sha256, None),
        Err(Error::Invalid("Artifact checksum mismatch"))
    ));
}
