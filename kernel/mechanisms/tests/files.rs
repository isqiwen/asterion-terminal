use asterion_kernel::files::{
    Error, FileLock, ReadRoot, atomic_write, file_digest, trusted_tree_digest,
};
use sha2::{Digest, Sha256};
use std::fs;
use std::os::unix::fs::{PermissionsExt, symlink};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

#[test]
fn grants_read_hash_and_deterministic_scan_without_following_links() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path().join("root");
    fs::create_dir(&root).unwrap();
    fs::create_dir(root.join("parts")).unwrap();
    fs::create_dir(root.join("parts/nested")).unwrap();
    fs::write(root.join("parts/z.bin"), b"abc").unwrap();
    fs::write(root.join("parts/nested/a.bin"), b"def").unwrap();
    fs::write(root.join("parts/skip.txt"), b"ignore").unwrap();
    let grant = ReadRoot::new(&root, 10, 100).unwrap();
    assert_eq!(grant.read("parts/z.bin").unwrap(), b"abc");
    assert_eq!(
        grant.digest("parts/z.bin").unwrap(),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    assert_eq!(
        grant.scan("parts", ".bin").unwrap(),
        ["parts/nested/a.bin", "parts/z.bin"]
    );
    assert!(grant.scan("missing/nested", "").unwrap().is_empty());
    for name in [
        "",
        "../secret",
        "/secret",
        "./parts/z.bin",
        "parts/../z.bin",
        "parts//z.bin",
        "a\\b",
        "parts/z.bin\0",
    ] {
        assert!(grant.read(name).is_err(), "{name:?}");
        assert!(grant.scan(name, "").is_err(), "{name:?}");
    }
    symlink(temp.path(), root.join("link")).unwrap();
    assert!(matches!(grant.read("link/outside"), Err(Error::Invalid(_))));
    symlink(root.join("parts/z.bin"), root.join("parts/link")).unwrap();
    assert!(matches!(grant.digest("parts/link"), Err(Error::Invalid(_))));
    assert!(matches!(
        grant.scan("parts", ".bin"),
        Err(Error::Invalid(_))
    ));
}

#[test]
fn root_handle_stays_anchored_after_path_replacement() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path().join("grant");
    fs::create_dir(&root).unwrap();
    fs::write(root.join("item"), b"original").unwrap();
    let grant = ReadRoot::new(&root, 100, 100).unwrap();
    fs::rename(&root, temp.path().join("moved")).unwrap();
    fs::create_dir(&root).unwrap();
    fs::write(root.join("item"), b"replacement").unwrap();
    assert_eq!(grant.read("item").unwrap(), b"original");
}

#[test]
fn read_and_scan_have_explicit_limits_and_reject_non_regular_files() {
    let temp = tempfile::tempdir().unwrap();
    let directory = temp.path().join("directory");
    fs::create_dir(&directory).unwrap();
    fs::write(directory.join("one"), b"too large").unwrap();
    fs::write(directory.join("two"), b"two").unwrap();
    let grant = ReadRoot::new(temp.path(), 3, 1).unwrap();
    assert!(matches!(
        grant.read("directory/one"),
        Err(Error::Invalid(_))
    ));
    assert!(grant.digest("directory/one").is_ok());
    assert!(matches!(
        grant.scan("directory", "no-matches"),
        Err(Error::Invalid(_))
    ));
    assert!(matches!(grant.read("directory"), Err(Error::Invalid(_))));
    nix::unistd::mkfifo(&temp.path().join("pipe"), nix::sys::stat::Mode::S_IRUSR).unwrap();
    assert!(matches!(grant.read("pipe"), Err(Error::Invalid(_))));
    assert!(ReadRoot::new(temp.path(), 0, 1).is_err());
    assert!(ReadRoot::new(temp.path(), u64::MAX, 1).is_err());
}

#[test]
fn concurrent_scans_do_not_share_directory_cursors() {
    let temp = tempfile::tempdir().unwrap();
    fs::create_dir(temp.path().join("parts")).unwrap();
    for i in 0..40 {
        fs::write(temp.path().join(format!("parts/{i:02}")), b"").unwrap();
    }
    let grant = Arc::new(ReadRoot::new(temp.path(), 10, 100).unwrap());
    let barrier = Arc::new(Barrier::new(4));
    let threads: Vec<_> = (0..4)
        .map(|_| {
            let grant = grant.clone();
            let barrier = barrier.clone();
            std::thread::spawn(move || {
                barrier.wait();
                for _ in 0..20 {
                    assert_eq!(grant.scan("parts", "").unwrap().len(), 40);
                }
            })
        })
        .collect();
    for thread in threads {
        thread.join().unwrap();
    }
}

#[test]
fn atomic_writes_publish_complete_private_files_and_never_clobber_no_replace() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("state.json");
    atomic_write(&path, b"first", false).unwrap();
    assert_eq!(
        fs::metadata(&path).unwrap().permissions().mode() & 0o777,
        0o600
    );
    assert!(
        matches!(atomic_write(&path, b"second", false), Err(Error::Io(error)) if error.kind() == std::io::ErrorKind::AlreadyExists)
    );
    assert_eq!(fs::read(&path).unwrap(), b"first");
    atomic_write(&path, b"complete", true).unwrap();
    assert_eq!(fs::read(&path).unwrap(), b"complete");
    assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 1);
    fs::remove_file(&path).unwrap();
    let outside = temp.path().join("outside");
    fs::write(&outside, b"private").unwrap();
    symlink(&outside, &path).unwrap();
    assert!(atomic_write(&path, b"replaced", true).is_err());
    assert_eq!(fs::read(&outside).unwrap(), b"private");
    assert!(file_digest(&path).is_err());
}

#[test]
fn competing_no_replace_publishers_have_exactly_one_winner() {
    let temp = tempfile::tempdir().unwrap();
    let path = Arc::new(temp.path().join("published"));
    let barrier = Arc::new(Barrier::new(4));
    let threads: Vec<_> = (0..4)
        .map(|i| {
            let path = path.clone();
            let barrier = barrier.clone();
            std::thread::spawn(move || {
                barrier.wait();
                atomic_write(&path, &[i; 128], false).is_ok()
            })
        })
        .collect();
    assert_eq!(
        threads
            .into_iter()
            .filter_map(|thread| thread.join().unwrap().then_some(()))
            .count(),
        1
    );
    let bytes = fs::read(&*path).unwrap();
    assert_eq!(bytes.len(), 128);
    assert!(bytes.iter().all(|byte| *byte == bytes[0]));
    assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 1);
}

#[test]
fn file_lock_conflicts_and_releases_on_drop_without_truncating() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("lock");
    fs::write(&path, b"preserved").unwrap();
    let lock = FileLock::acquire(&path, false).unwrap();
    assert!(
        matches!(FileLock::acquire(&path, false), Err(Error::Io(error)) if error.kind() == std::io::ErrorKind::WouldBlock)
    );
    drop(lock);
    assert!(FileLock::acquire(&path, false).is_ok());
    assert_eq!(fs::read(&path).unwrap(), b"preserved");
    symlink(&path, temp.path().join("link")).unwrap();
    assert!(FileLock::acquire(&temp.path().join("link"), true).is_err());
}

#[test]
fn trusted_installation_tree_keeps_file_aliases_and_excludes_directory_aliases() {
    let temp = tempfile::tempdir().unwrap();
    let environment = temp.path().join("environment");
    let interpreter = temp.path().join("interpreter");
    fs::create_dir_all(environment.join("bin")).unwrap();
    fs::create_dir_all(environment.join("lib/__pycache__")).unwrap();
    fs::create_dir_all(environment.join("a")).unwrap();
    fs::create_dir(&interpreter).unwrap();
    fs::write(interpreter.join("python"), b"executable").unwrap();
    fs::write(environment.join("a/z"), b"nested").unwrap();
    fs::write(environment.join("a.txt"), b"sibling").unwrap();
    fs::write(environment.join("lib/module.py"), b"module").unwrap();
    fs::write(environment.join("lib/cache.pyc"), b"ignored").unwrap();
    fs::write(environment.join("lib/__pycache__/ignored"), b"ignored").unwrap();
    symlink("../../interpreter/python", environment.join("bin/python")).unwrap();
    symlink("python", environment.join("bin/python3")).unwrap();
    symlink("lib", environment.join("lib64")).unwrap();
    let mut expected = Sha256::new();
    for (name, bytes) in [
        ("0/a/z", b"nested".as_slice()),
        ("0/a.txt", b"sibling".as_slice()),
        ("0/bin/python", b"executable".as_slice()),
        ("0/bin/python3", b"executable".as_slice()),
        ("0/lib/module.py", b"module".as_slice()),
        ("1/python", b"executable".as_slice()),
    ] {
        expected.update((name.len() as u64).to_be_bytes());
        expected.update(name);
        expected.update(Sha256::digest(bytes));
    }
    let roots = [environment.clone(), interpreter];
    assert_eq!(
        trusted_tree_digest(&roots, &["__pycache__".into()], &[".pyc".into()], 100, 4096).unwrap(),
        hex::encode(expected.finalize())
    );
    symlink(&environment, temp.path().join("environment-alias")).unwrap();
    assert_eq!(
        trusted_tree_digest(&[environment], &[], &[], 100, 4096).unwrap(),
        trusted_tree_digest(
            &[temp.path().join("environment-alias")],
            &[],
            &[],
            100,
            4096
        )
        .unwrap()
    );
}

#[test]
fn trusted_tree_rejects_broken_links_special_files_and_exhausted_budgets() {
    let temp = tempfile::tempdir().unwrap();
    let roots = [temp.path().to_owned()];
    fs::write(temp.path().join("data"), b"three").unwrap();
    assert!(matches!(
        trusted_tree_digest(&roots, &[], &[], 10, 4),
        Err(Error::Invalid("Trusted tree exceeds byte budget"))
    ));
    assert!(matches!(
        trusted_tree_digest(&roots, &[], &[], 1, 100),
        Err(Error::Invalid("Trusted tree exceeds entry budget"))
    ));
    symlink("missing", temp.path().join("broken")).unwrap();
    assert!(matches!(
        trusted_tree_digest(&roots, &[], &[], 10, 100),
        Err(Error::Io(_))
    ));
    fs::remove_file(temp.path().join("broken")).unwrap();
    nix::unistd::mkfifo(&temp.path().join("pipe"), nix::sys::stat::Mode::S_IRUSR).unwrap();
    assert!(matches!(
        trusted_tree_digest(&roots, &[], &[], 10, 100),
        Err(Error::Invalid(_))
    ));
    assert!(trusted_tree_digest(&roots, &[], &[], 0, 100).is_err());
    assert!(trusted_tree_digest(&roots, &[], &[], 10, 0).is_err());
}

#[test]
fn bounded_file_lock_times_out_or_acquires_after_the_owner_releases() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("lock");
    fs::write(&path, b"preserved").unwrap();
    let owner = FileLock::acquire(&path, false).unwrap();
    let started = Instant::now();
    assert!(
        matches!(FileLock::acquire_for(&path, Duration::from_millis(40)), Err(Error::Io(error)) if error.kind() == std::io::ErrorKind::TimedOut)
    );
    assert!(started.elapsed() >= Duration::from_millis(40));
    let waiter_path = path.clone();
    let waiter =
        std::thread::spawn(move || FileLock::acquire_for(&waiter_path, Duration::from_secs(2)));
    drop(owner);
    let acquired = waiter.join().unwrap().unwrap();
    assert!(FileLock::acquire(&path, false).is_err());
    drop(acquired);
    assert!(FileLock::acquire_for(&path, Duration::ZERO).is_ok());
    assert_eq!(fs::read(&path).unwrap(), b"preserved");
}

#[test]
fn stream_grant_uses_positional_reads_and_rejects_replacement_and_mutation() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("data");
    fs::write(&path, b"abcdef").unwrap();
    let grant = ReadRoot::new(temp.path(), 1024, 100).unwrap();
    let file = grant.open_file("data").unwrap();
    let other = file.clone();
    let mut bytes = [0; 3];
    assert_eq!(file.read_at(3, &mut bytes).unwrap(), 3);
    assert_eq!(&bytes, b"def");
    other.read_at(0, &mut bytes).unwrap();
    assert_eq!(&bytes, b"abc");
    file.verify_sha256(
        &hex::encode(Sha256::digest(b"abcdef")),
        &std::sync::atomic::AtomicBool::new(false),
    )
    .unwrap();
    atomic_write(&path, b"abcdef", true).unwrap();
    assert!(file.check().is_err());
    assert!(file.read_at(0, &mut bytes).is_err());
    let file = grant.open_file("data").unwrap();
    std::thread::sleep(Duration::from_millis(20));
    fs::write(&path, b"ghijkl").unwrap();
    assert!(file.check().is_err());
}

#[test]
fn stream_grant_enforces_limits_links_and_cancellation() {
    let temp = tempfile::tempdir().unwrap();
    fs::write(temp.path().join("data"), b"abc").unwrap();
    symlink("data", temp.path().join("link")).unwrap();
    let grant = ReadRoot::new(temp.path(), 3, 10).unwrap();
    assert!(grant.open_file("link").is_err());
    assert!(grant.open_file("../data").is_err());
    let file = grant.open_file("data").unwrap();
    assert!(file.read_at(4, &mut [0; 1]).is_err());
    assert!(
        file.verify_sha256(
            &hex::encode(Sha256::digest(b"abc")),
            &std::sync::atomic::AtomicBool::new(true),
        )
        .is_err()
    );
    assert!(
        ReadRoot::new(temp.path(), 2, 10)
            .unwrap()
            .open_file("data")
            .is_err()
    );
}
