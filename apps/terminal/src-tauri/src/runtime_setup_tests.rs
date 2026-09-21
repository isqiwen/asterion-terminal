use super::*;
use std::sync::atomic::{AtomicU64, Ordering};
static NEXT: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "asterion-setup-test-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn fixture(path: &Path) -> (PathBuf, PathBuf, serde_json::Value) {
    let bundle = path.join("setup");
    let state = path.join("user");
    fs::create_dir_all(&bundle).unwrap();
    fs::create_dir_all(&state).unwrap();
    fs::write(state.join("business-record"), "preserve").unwrap();
    fs::write(bundle.join("app.whl"), "wheel").unwrap();
    fs::write(bundle.join("requirements.txt"), "locked").unwrap();
    let artifact = serde_json::json!({"url":"https://example.com/asset.tar.gz", "sha256": "a".repeat(64), "size": 10});
    let manifest = serde_json::json!({"schema":1,"platform":std::env::consts::OS,"architecture":std::env::consts::ARCH,
        "uv":artifact,"python":artifact,"postgres": if cfg!(target_os = "linux") { serde_json::json!({"source":"system", "root":"/usr", "executable":"lib/postgresql/17/bin/postgres"}) } else { serde_json::json!({"source":"homebrew", "formula":"postgresql@17"}) },"uv_executable":"bin/uv","python_version":"3.12.12",
        "wheel":"app.whl","wheel_sha256":hash_file(&bundle.join("app.whl")).unwrap(),"requirements_sha256":hash_file(&bundle.join("requirements.txt")).unwrap()});
    fs::write(bundle.join("manifest.json"), manifest.to_string()).unwrap();
    (bundle, state, manifest)
}
#[test]
fn rejects_invalid_contracts_without_modifying_business_data() {
    let temp = Directory::new();
    let (bundle, state, manifest) = fixture(&temp.0);
    for (field, value) in [
        ("schema", serde_json::json!(2)),
        ("unexpected", serde_json::json!(true)),
        ("platform", serde_json::json!("unsupported")),
        ("wheel", serde_json::json!("../outside.whl")),
    ] {
        let mut invalid = manifest.clone();
        invalid[field] = value;
        fs::write(bundle.join("manifest.json"), invalid.to_string()).unwrap();
        assert!(Setup::load(&bundle, &state).is_err());
        assert_eq!(
            fs::read_to_string(state.join("business-record")).unwrap(),
            "preserve"
        );
        assert!(!state.join("runtime").exists());
    }
}
#[test]
fn rejects_tampered_wheel_and_unencrypted_download() {
    let temp = Directory::new();
    let (bundle, state, mut manifest) = fixture(&temp.0);
    fs::write(bundle.join("app.whl"), "modified").unwrap();
    assert!(Setup::load(&bundle, &state)
        .err()
        .unwrap()
        .contains("校验失败"));
    fs::write(bundle.join("app.whl"), "wheel").unwrap();
    manifest["python"]["url"] = serde_json::json!("http://example.com/python");
    fs::write(bundle.join("manifest.json"), manifest.to_string()).unwrap();
    assert!(Setup::load(&bundle, &state).is_err());
}
#[test]
fn unpublished_environment_is_not_ready_and_existing_receipt_is_not_repaired() {
    let temp = Directory::new();
    let (bundle, state, _) = fixture(&temp.0);
    let setup = Setup::load(&bundle, &state).unwrap();
    fs::create_dir_all(setup.python().parent().unwrap()).unwrap();
    fs::write(setup.python(), "python").unwrap();
    assert!(!setup.ready());
    fs::write(setup.root.join("ready"), "unsupported").unwrap();
    assert!(Setup::load(&bundle, &state).is_err());
    assert_eq!(
        fs::read_to_string(setup.root.join("ready")).unwrap(),
        "unsupported"
    );
    assert_eq!(
        fs::read_to_string(state.join("business-record")).unwrap(),
        "preserve"
    );
}
#[test]
fn second_installer_cannot_mutate_locked_environment() {
    let temp = Directory::new();
    let (bundle, state, _) = fixture(&temp.0);
    let setup = Setup::load(&bundle, &state).unwrap();
    let parent = setup.root.parent().unwrap();
    fs::create_dir_all(parent).unwrap();
    let lock = File::create(parent.join("setup.lock")).unwrap();
    lock.lock_exclusive().unwrap();
    assert!(setup
        .install(|_| panic!("must not start"))
        .unwrap_err()
        .contains("另一个窗口"));
    assert!(!setup.root.exists());
}
#[test]
fn verified_cache_is_reused_without_network() {
    let temp = Directory::new();
    let data = b"verified archive";
    let a = Artifact {
        url: "https://invalid.invalid/unreachable".into(),
        sha256: digest(data),
        size: data.len() as u64,
    };
    fs::write(temp.0.join(&a.sha256), data).unwrap();
    let mut observed = None;
    let path = download(&a, &temp.0, |n, t| observed = Some((n, t))).unwrap();
    assert_eq!(fs::read(path).unwrap(), data);
    assert_eq!(observed, Some((a.size, Some(a.size))));
}

#[test]
fn running_child_keeps_installation_lease_after_parent_handle_closes() {
    let temp = Directory::new();
    let path = temp.0.join("setup.lock");
    let lease = File::create(&path).unwrap();
    lease.lock_exclusive().unwrap();
    let mut child = Command::new("/bin/sleep")
        .arg("0.2")
        .stdin(lease.try_clone().unwrap())
        .spawn()
        .unwrap();
    drop(lease);
    let contender = File::open(path).unwrap();
    assert!(contender.try_lock_exclusive().is_err());
    child.wait().unwrap();
    assert!(contender.try_lock_exclusive().is_ok());
}
#[test]
fn archive_cannot_link_outside_destination() {
    let temp = Directory::new();
    let archive = temp.0.join("bad.tar.gz");
    let gzip = flate2::write::GzEncoder::new(
        File::create(&archive).unwrap(),
        flate2::Compression::default(),
    );
    let mut builder = tar::Builder::new(gzip);
    let mut header = tar::Header::new_gnu();
    header.set_entry_type(tar::EntryType::Symlink);
    header.set_size(0);
    header.set_mode(0o777);
    builder
        .append_link(&mut header, "escape", "../business-record")
        .unwrap();
    builder.into_inner().unwrap().finish().unwrap();
    fs::write(temp.0.join("business-record"), "preserve").unwrap();
    assert!(unpack(&archive, &temp.0.join("unpacked")).is_err());
    assert_eq!(
        fs::read_to_string(temp.0.join("business-record")).unwrap(),
        "preserve"
    );
}

#[cfg(target_os = "linux")]
#[test]
fn linux_requires_system_postgres_and_rejects_unknown_source() {
    let temp = Directory::new();
    let (bundle, state, mut manifest) = fixture(&temp.0);
    let setup = Setup::load(&bundle, &state).unwrap();
    assert_eq!(setup.postgres_root(), Path::new("/usr"));
    assert_eq!(
        setup.postgres_executable(),
        Path::new("/usr/lib/postgresql/17/bin/postgres")
    );
    manifest["postgres"] = serde_json::json!({"source":"unsupported"});
    fs::write(bundle.join("manifest.json"), manifest.to_string()).unwrap();
    assert!(Setup::load(&bundle, &state).is_err());
    assert!(!state.join("runtime").exists());
}

#[test]
fn missing_database_returns_actionable_error() {
    let temp = Directory::new();
    let (bundle, state, _) = fixture(&temp.0);
    let mut setup = Setup::load(&bundle, &state).unwrap();
    setup.manifest.postgres = Postgres::System {
        root: temp.0.join("missing"),
        executable: "postgres".into(),
    };
    assert!(setup
        .check_postgres()
        .unwrap_err()
        .contains("postgresql-17"));
    assert!(!setup.ready());
}

#[test]
fn dependency_downloads_are_not_counted_as_installations() {
    let mut state = DependencyProgress::default();
    state.observe("Resolved 3 packages in 1s");
    state.observe("Downloading pyarrow (40MiB)");
    state.observe("Downloading pydantic-core (2MiB)");
    assert_eq!(state.total, Some(3));
    assert_eq!(state.installed, 0);
    assert!(state.current.contains("pyarrow"));
    state.observe("Downloaded pyarrow");
    assert!(!state.current.contains("pyarrow"));
    state.observe("Downloaded pydantic-core");
    state.observe("Prepared 3 packages in 2s");
    assert_eq!(state.phase, "installing");
    assert_eq!(state.installed, 0);
    state.observe("Installed 3 packages in 40ms");
    state.observe(" + pyarrow==23.0.1");
    assert_eq!(state.installed, 3);
    assert_eq!(state.current, "pyarrow==23.0.1");
}

#[test]
fn command_observer_ignores_prior_logs_and_preserves_failed_progress() {
    let temp = Directory::new();
    fs::write(temp.0.join("setup.log"), "Installed 99 packages in 1s\n").unwrap();
    let lease = File::create(temp.0.join("lease")).unwrap();
    let mut state = DependencyProgress::default();
    let result = run_observed(
        Command::new("/bin/sh").args([
            "-c",
            "printf 'Resolved 2 packages in 1s\\nDownloading pyarrow (40MiB)\\n' >&2; exit 1",
        ]),
        &temp.0,
        &lease,
        5,
        |line| state.observe(line),
    );
    assert!(result.is_err());
    assert_eq!(state.total, Some(2));
    assert_eq!(state.installed, 0);
    assert_eq!(state.current, "pyarrow");
}

#[test]
fn fixed_runtime_directory_still_rejects_a_different_install_receipt() {
    let temp = Directory::new();
    let (bundle, state, _) = fixture(&temp.0);
    let setup = Setup::load(&bundle, &state).unwrap();
    assert_eq!(setup.root, state.join("runtime"));
    fs::create_dir_all(&setup.root).unwrap();
    fs::write(setup.root.join("ready"), "different-installation").unwrap();
    assert!(Setup::load(&bundle, &state).is_err());
    assert_eq!(fs::read_to_string(setup.root.join("ready")).unwrap(), "different-installation");
}

fn installed_fixture(path: &Path, exit_code: i32) -> (Setup, PathBuf) {
    use std::os::unix::fs::PermissionsExt;
    let (bundle, state, _) = fixture(path);
    let mut setup = Setup::load(&bundle, &state).unwrap();
    // Keep a platform-independent fake PostgreSQL executable outside the runtime payload.
    let pg = path.join("system-postgres");
    fs::create_dir_all(&pg).unwrap();
    fs::write(pg.join("postgres"), "#!/bin/sh\necho 'postgres (PostgreSQL) 17.11'\n").unwrap();
    fs::set_permissions(pg.join("postgres"), fs::Permissions::from_mode(0o700)).unwrap();
    for tool in ["initdb", "pg_ctl", "pg_dump", "pg_restore"] {
        fs::write(pg.join(tool), "system-tool").unwrap();
    }
    setup.manifest.postgres = Postgres::System { root: pg, executable: "postgres".into() };
    fs::create_dir_all(setup.python().parent().unwrap()).unwrap();
    fs::write(setup.python(), format!("#!/bin/sh\nexit {exit_code}\n")).unwrap();
    fs::set_permissions(setup.python(), fs::Permissions::from_mode(0o700)).unwrap();
    fs::write(setup.root.join("ready"), "a".repeat(64)).unwrap();
    fs::write(state.join("desktop.json"), "preserve-config").unwrap();
    (setup, state)
}

#[test]
fn runtime_replacement_removes_only_code_after_successful_stop() {
    let temp = Directory::new();
    let (setup, state) = installed_fixture(&temp.0, 0);
    assert!(setup.installed_id().unwrap().is_some());
    assert!(!setup.ready());
    let lease = File::create(state.join("setup.lock")).unwrap();
    lease.lock_exclusive().unwrap();
    setup.replace_installed_runtime(&lease).unwrap();
    assert!(!setup.root.join("ready").exists());
    assert!(!setup.root.join("environment").exists());
    assert_eq!(fs::read_to_string(state.join("business-record")).unwrap(), "preserve");
    assert_eq!(fs::read_to_string(state.join("desktop.json")).unwrap(), "preserve-config");
    assert!(setup.postgres_executable().exists());
    // Interrupted, unpublished replacement can be retried in the same directory.
    fs::create_dir_all(setup.root.join("interpreter")).unwrap();
    fs::write(setup.root.join("interpreter/stale-module"), "partial").unwrap();
    setup.replace_installed_runtime(&lease).unwrap();
    assert!(!setup.root.join("interpreter").exists());
}

#[test]
fn failed_stop_preserves_published_runtime_and_receipt() {
    let temp = Directory::new();
    let (setup, state) = installed_fixture(&temp.0, 1);
    let lease = File::create(state.join("setup.lock")).unwrap();
    lease.lock_exclusive().unwrap();
    assert!(setup.replace_installed_runtime(&lease).is_err());
    assert!(setup.python().exists());
    assert_eq!(fs::read_to_string(setup.root.join("ready")).unwrap(), "a".repeat(64));
    assert_eq!(fs::read_to_string(state.join("business-record")).unwrap(), "preserve");
}

#[test]
fn repeat_install_is_noop_and_runtime_calls_block_replacement() {
    let temp = Directory::new();
    let (setup, state) = installed_fixture(&temp.0, 1);
    fs::write(setup.root.join("ready"), &setup.id).unwrap();
    setup.install(|_| panic!("same installation must not run")).unwrap();
    let reader = setup.runtime_lease().unwrap();
    assert!(setup.install(|_| panic!("leased runtime must not change")).is_err());
    drop(reader);
    let writer = File::open(state.join("setup.lock")).unwrap();
    writer.lock_exclusive().unwrap();
    assert!(setup.runtime_lease().is_err());
    assert!(setup.ready());
}

#[cfg(target_os = "macos")]
#[test]
fn macos_requires_homebrew_formula_and_uses_system_root() {
    let temp = Directory::new();
    let (bundle, state, mut manifest) = fixture(&temp.0);
    let setup = Setup::load(&bundle, &state).unwrap();
    assert_eq!(setup.postgres_root(), homebrew_prefix().join("opt/postgresql@17"));
    assert!(!setup.postgres_root().starts_with(&state));
    manifest["postgres"]["formula"] = serde_json::json!("unsupported-formula");
    fs::write(bundle.join("manifest.json"), manifest.to_string()).unwrap();
    assert!(Setup::load(&bundle, &state).is_err());
    assert!(!state.join("runtime").exists());
}

#[test]
fn missing_homebrew_explains_installation_without_running_remote_script() {
    let temp = Directory::new();
    let lease = File::create(temp.0.join("lease")).unwrap();
    let error = install_homebrew_postgres(&temp.0.join("missing-brew"), "postgresql@17", &temp.0, &lease,
        || panic!("must not start an installer without Homebrew")).unwrap_err();
    assert!(error.contains("https://brew.sh"));
    assert!(error.contains("重试"));
    assert!(!temp.0.join("setup.log").exists());
}

#[test]
fn homebrew_command_installs_only_requested_formula_and_reports_failure() {
    use std::os::unix::fs::PermissionsExt;
    let temp = Directory::new();
    let brew = temp.0.join("brew");
    let lease = File::create(temp.0.join("lease")).unwrap();
    fs::write(&brew, "#!/bin/sh\nprintf '%s\\n' \"$@\" \"$HOMEBREW_NO_INSTALL_CLEANUP\" \"$HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK\"\n").unwrap();
    fs::set_permissions(&brew, fs::Permissions::from_mode(0o700)).unwrap();
    let mut reported = false;
    install_homebrew_postgres(&brew, "postgresql@17", &temp.0, &lease, || reported = true).unwrap();
    assert!(reported);
    assert_eq!(fs::read_to_string(temp.0.join("setup.log")).unwrap(), "install\npostgresql@17\n1\n1\n");
    fs::write(&brew, "#!/bin/sh\nexit 1\n").unwrap();
    assert!(install_homebrew_postgres(&brew, "postgresql@17", &temp.0, &lease, || {})
        .unwrap_err().contains("Homebrew 安装 PostgreSQL 17 失败"));
}

#[test]
fn missing_system_database_does_not_erase_published_python_environment() {
    let temp = Directory::new();
    let (setup, state) = installed_fixture(&temp.0, 0);
    fs::remove_file(setup.postgres_executable()).unwrap();
    assert!(setup.installed_id().unwrap().is_some());
    assert!(!setup.ready());
    assert!(setup.install(|_| {}).is_err());
    assert!(setup.python().is_file());
    assert_eq!(fs::read_to_string(setup.root.join("ready")).unwrap(), "a".repeat(64));
    assert_eq!(fs::read_to_string(state.join("business-record")).unwrap(), "preserve");
}
