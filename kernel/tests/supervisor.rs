use asterion_kernel::supervisor::{Configuration, Error, Policy, ProcessSpec, Supervisor};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    fs,
    path::Path,
    time::{Duration, Instant},
};

fn config(root: &Path, script: &str) -> Configuration {
    Configuration {
        processes: vec![ProcessSpec {
            name: "example".into(),
            program: "/bin/sh".into(),
            arguments: vec!["-c".into(), script.into()],
            environment: BTreeMap::new(),
            cwd: root.into(),
        }],
        status_file: root.join("status.json"),
        build_id: "a".repeat(64),
        policy: Policy {
            restart_millis: 80,
            snapshot_millis: 5,
            shutdown_millis: 100,
        },
    }
}

#[test]
fn restarts_are_throttled_and_snapshot_uses_declared_roles() {
    let root = tempfile::tempdir().unwrap();
    let supervisor = Supervisor::new(config(root.path(), "echo start >> starts; exit 3")).unwrap();
    let started = Instant::now();
    let mut observed = false;
    supervisor
        .run(&mut || {
            if let Ok(raw) = fs::read(root.path().join("status.json")) {
                let value: Value = serde_json::from_slice(&raw).unwrap();
                assert_eq!(value["build_id"], "a".repeat(64));
                assert!(value["observed_at"].as_f64().unwrap() > 0.0);
                assert!(matches!(
                    value["example"].as_str(),
                    Some("running" | "stopped")
                ));
                observed = true;
            }
            if started.elapsed() > Duration::from_millis(230) {
                supervisor.request_stop();
            }
            Ok::<_, ()>(())
        })
        .unwrap();
    assert!(observed);
    let launches = fs::read_to_string(root.path().join("starts"))
        .unwrap()
        .lines()
        .count();
    assert!((2..=3).contains(&launches), "{launches}");
    assert!(!root.path().join("status.json").exists());
    assert!(supervisor.run(&mut || Ok::<_, ()>(())).is_err());
}

#[test]
fn startup_failure_reaps_already_started_children_and_removes_status() {
    let root = tempfile::tempdir().unwrap();
    let mut configuration = config(root.path(), "exec /bin/sleep 60");
    configuration.processes.push(ProcessSpec {
        name: "missing".into(),
        program: root.path().join("missing"),
        arguments: vec![],
        environment: BTreeMap::new(),
        cwd: root.path().into(),
    });
    let supervisor = Supervisor::new(configuration).unwrap();
    let started = Instant::now();
    assert!(matches!(
        supervisor.run(&mut || Ok::<_, ()>(())),
        Err(Error::Mechanism(_))
    ));
    assert!(started.elapsed() < Duration::from_secs(2));
    assert!(!root.path().join("status.json").exists());
}

#[test]
fn stop_signals_entire_group_and_kills_children_ignoring_term() {
    let root = tempfile::tempdir().unwrap();
    let supervisor = Supervisor::new(config(root.path(), "trap '' TERM; echo $$ > leader; /bin/sh -c 'trap \"\" TERM; echo $$ > descendant; exec /bin/sleep 60' & wait")).unwrap();
    let started = Instant::now();
    supervisor
        .run(&mut || {
            if root.path().join("leader").exists() && root.path().join("descendant").exists() {
                supervisor.request_stop();
            }
            assert!(started.elapsed() < Duration::from_secs(5));
            Ok::<_, ()>(())
        })
        .unwrap();
    assert!(started.elapsed() < Duration::from_secs(2));
    let leader: i32 = fs::read_to_string(root.path().join("leader"))
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    assert!(nix::sys::signal::kill(nix::unistd::Pid::from_raw(leader), None).is_err());
    // SIGKILL delivery and the leader's wait are not a scheduling barrier for
    // its descendants. Allow the kernel to finish their exit independently.
    let descendant = fs::read_to_string(root.path().join("descendant")).unwrap();
    let process = std::path::PathBuf::from(format!("/proc/{}/stat", descendant.trim()));
    let exit_deadline = Instant::now() + Duration::from_secs(2);
    while let Ok(stat) = fs::read_to_string(&process) {
        if stat.split_whitespace().nth(2) == Some("Z") {
            break;
        }
        assert!(
            Instant::now() < exit_deadline,
            "Descendant survived group shutdown: {stat}"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn callback_failure_reclaims_children_and_preserves_error() {
    let root = tempfile::tempdir().unwrap();
    let supervisor =
        Supervisor::new(config(root.path(), "echo $$ > leader; exec /bin/sleep 60")).unwrap();
    let mut ticks = 0;
    let result = supervisor.run(&mut || {
        ticks += 1;
        if ticks > 3 { Err("interrupt") } else { Ok(()) }
    });
    assert!(matches!(result, Err(Error::Callback("interrupt"))));
    assert!(!root.path().join("status.json").exists());
    let leader: i32 = fs::read_to_string(root.path().join("leader"))
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    assert!(nix::sys::signal::kill(nix::unistd::Pid::from_raw(leader), None).is_err());
}

#[test]
fn invalid_process_declarations_and_timing_are_rejected() {
    let root = tempfile::tempdir().unwrap();
    let mut declaration = config(root.path(), "exit");
    declaration.processes[0].name = "observed_at".into();
    assert!(Supervisor::new(declaration).is_err());
    let mut declaration = config(root.path(), "exit");
    declaration.policy.restart_millis = 0;
    assert!(Supervisor::new(declaration).is_err());
    let mut declaration = config(root.path(), "exit");
    declaration.processes[0].program = "relative".into();
    assert!(Supervisor::new(declaration).is_err());
}

#[test]
fn stop_before_start_preserves_unowned_status_file() {
    let root = tempfile::tempdir().unwrap();
    let configuration = config(root.path(), "exit");
    fs::write(&configuration.status_file, "existing record").unwrap();
    let supervisor = Supervisor::new(configuration).unwrap();
    supervisor.request_stop();
    supervisor.run(&mut || Ok::<_, ()>(())).unwrap();
    assert_eq!(
        fs::read_to_string(root.path().join("status.json")).unwrap(),
        "existing record"
    );
}
