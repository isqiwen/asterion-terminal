use asterion_kernel::environments::{Action, AdapterError, Error, Layout, Lease};
use asterion_kernel::files::FileLock;
use serde_json::{Value, json};
use std::{
    fs,
    os::unix::fs::{PermissionsExt, symlink},
    path::Path,
};

fn layout() -> Layout {
    Layout {
        required_files: vec!["configuration".into()],
        required_directories: vec!["records".into()],
        offline_locks: vec!["owner.lock".into()],
    }
}
fn state(root: &Path) {
    fs::create_dir_all(root.join("records")).unwrap();
    fs::write(root.join("configuration"), "fixture").unwrap();
    fs::write(root.join("records/history"), "immutable fact").unwrap();
}
fn lease(host: &Path, backups: &Path) -> Lease {
    Lease::acquire(host, backups, layout(), false).unwrap()
}
fn ok(_: Action<'_>) -> Result<Value, AdapterError<&'static str>> {
    Ok(json!({"verified": true}))
}
fn name(action: &Action<'_>) -> &'static str {
    match action {
        Action::ValidateTarget { .. } => "validate",
        Action::Stop { .. } => "stop",
        Action::Start { .. } => "start",
        Action::Protect { .. } => "protect",
        Action::Verify { .. } => "verify",
    }
}

#[test]
fn selection_and_rollback_preserve_both_environments() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    let target = root.path().join("target");
    let backups = root.path().join("backups");
    state(&host);
    state(&target);
    let manager = lease(&host, &backups);
    let mut calls = vec![];
    let mut adapter = |action: Action<'_>| {
        calls.push(name(&action));
        ok(action)
    };
    let result = manager.switch(Some(&target), &mut adapter).unwrap();
    assert_eq!(calls, ["validate", "stop", "protect", "verify", "start"]);
    assert_eq!(result["active"], json!(target));
    assert_eq!(manager.active().unwrap(), target);
    assert_eq!(
        fs::metadata(host.join("environment.json"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    fs::write(target.join("records/new"), "new fact").unwrap();
    manager.switch(None, &mut ok).unwrap();
    assert_eq!(manager.active().unwrap(), host);
    assert_eq!(manager.read().unwrap().previous, Some(target.clone()));
    assert_eq!(
        fs::read_to_string(target.join("records/new")).unwrap(),
        "new fact"
    );
    assert_eq!(
        fs::read_to_string(host.join("records/history")).unwrap(),
        "immutable fact"
    );
}

#[test]
fn each_precommit_failure_recovers_source() {
    for failure in ["stop", "protect", "verify", "start"] {
        let root = tempfile::tempdir().unwrap();
        let host = root.path().join("host");
        let target = root.path().join("target");
        state(&host);
        state(&target);
        let manager = lease(&host, &root.path().join("backups"));
        let mut failed = false;
        let mut calls = vec![];
        let mut adapter = |action: Action<'_>| {
            let operation = name(&action);
            calls.push(operation);
            if !failed && operation == failure {
                failed = true;
                Err(AdapterError {
                    error: "injected",
                    interrupted: false,
                })
            } else {
                ok(action)
            }
        };
        assert!(manager.switch(Some(&target), &mut adapter).is_err());
        assert_eq!(manager.active().unwrap(), host);
        assert_eq!(manager.read().unwrap().pending, None);
        assert_eq!(&calls[calls.len() - 3..], ["stop", "stop", "start"]);
        assert!(target.join("records/history").exists());
    }
}

#[test]
fn interruption_and_failed_recovery_keep_durable_pending_until_retry() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    let target = root.path().join("target");
    state(&host);
    state(&target);
    let backups = root.path().join("backups");
    {
        let manager = lease(&host, &backups);
        let mut adapter = |action: Action<'_>| {
            if matches!(action, Action::Verify { .. }) {
                Err(AdapterError {
                    error: "interrupted",
                    interrupted: true,
                })
            } else {
                ok(action)
            }
        };
        assert!(matches!(
            manager.switch(Some(&target), &mut adapter),
            Err(Error::Adapter(_))
        ));
        assert_eq!(manager.read().unwrap().pending, Some(target.clone()));
        assert_eq!(manager.active().unwrap(), host);
    }
    let manager = lease(&host, &backups);
    let mut unavailable = |_: Action<'_>| {
        Err(AdapterError {
            error: "offline",
            interrupted: false,
        })
    };
    assert!(manager.recover(&mut unavailable).is_err());
    assert_eq!(manager.read().unwrap().pending, Some(target));
    manager.recover(&mut ok).unwrap();
    assert_eq!(manager.read().unwrap().pending, None);
    assert_eq!(manager.active().unwrap(), host);
}

#[test]
fn maintenance_and_target_locks_exclude_other_owners() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    let target = root.path().join("target");
    state(&host);
    state(&target);
    let backups = root.path().join("backups");
    let manager = lease(&host, &backups);
    assert!(Lease::acquire(&host, &backups, layout(), false).is_err());
    let target_lock = FileLock::acquire(&target.join("owner.lock"), false).unwrap();
    let mut calls = 0;
    assert!(
        manager
            .switch(Some(&target), &mut |action| {
                calls += 1;
                ok(action)
            })
            .is_err()
    );
    assert_eq!(calls, 0);
    assert!(!host.join("environment.json").exists());
    drop(target_lock);
    drop(manager);
    assert!(Lease::acquire(&host, &backups, layout(), false).is_ok());
}

#[test]
fn corrupt_missing_unknown_or_relative_fields_never_rewrite_manifest() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    state(&host);
    let manager = lease(&host, &root.path().join("backups"));
    let selection = serde_json::to_value(manager.read().unwrap()).unwrap();
    for mode in 0..5 {
        let mut value = selection.clone();
        match mode {
            0 => {
                value.as_object_mut().unwrap().remove("pending");
            }
            1 => {
                value["extra"] = json!(true);
            }
            2 => {
                value["active"] = json!("relative");
            }
            _ => {}
        }
        let raw = match mode {
            3 => vec![b' '; 65537],
            4 => b"{invalid".to_vec(),
            _ => serde_json::to_vec(&value).unwrap(),
        };
        fs::write(host.join("environment.json"), &raw).unwrap();
        assert!(manager.read().is_err(), "mode {mode}");
        assert_eq!(fs::read(host.join("environment.json")).unwrap(), raw);
    }
}

#[test]
fn missing_active_is_not_created_and_invalid_targets_never_stop_source() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    let target = root.path().join("target");
    state(&host);
    state(&target);
    let manager = lease(&host, &root.path().join("backups"));
    let mut calls = 0;
    for invalid in [&host, root.path(), &host.join("nested")] {
        assert!(
            manager
                .switch(Some(invalid), &mut |action| {
                    calls += 1;
                    ok(action)
                })
                .is_err()
        );
    }
    symlink(host.join("records/history"), target.join("records/link")).unwrap();
    assert!(
        manager
            .switch(Some(&target), &mut |action| {
                calls += 1;
                ok(action)
            })
            .is_err()
    );
    assert_eq!(calls, 0);
    let missing = root.path().join("missing");
    let mut selection = manager.read().unwrap();
    selection.active = missing.clone();
    fs::write(
        host.join("environment.json"),
        serde_json::to_vec(&selection).unwrap(),
    )
    .unwrap();
    assert!(manager.active().is_err());
    assert!(!missing.exists());
}

#[test]
fn invalid_recovery_relationship_never_runs_service_callbacks() {
    let root = tempfile::tempdir().unwrap();
    let host = root.path().join("host");
    state(&host);
    let manager = lease(&host, &root.path().join("backups"));
    let initial = manager.read().unwrap();
    for invalid in [root.path().to_path_buf(), host.join("nested"), host.clone()] {
        let mut selection = initial.clone();
        selection.pending = Some(invalid);
        let raw = serde_json::to_vec(&selection).unwrap();
        fs::write(host.join("environment.json"), &raw).unwrap();
        let mut called = false;
        assert!(
            manager
                .recover(&mut |action| {
                    called = true;
                    ok(action)
                })
                .is_err()
        );
        assert!(!called);
        assert_eq!(fs::read(host.join("environment.json")).unwrap(), raw);
    }
}
