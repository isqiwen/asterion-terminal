use asterion_kernel::diagnostics::{self, Component, EventCode, Observation};
use rusqlite::{Connection, params};
use std::{fs, path::PathBuf};
use uuid::Uuid;

struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        Self(std::env::temp_dir().join(format!("asterion-diagnostics-{}", Uuid::new_v4())))
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
const DIGEST: &str = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
const SCHEMA: &str = "CREATE TABLE executions (id TEXT PRIMARY KEY, digest TEXT NOT NULL, started REAL NOT NULL, duration_ms INTEGER NOT NULL, phase TEXT NOT NULL, code TEXT NOT NULL, calls INTEGER NOT NULL)";

#[test]
fn current_execution_format_is_read_without_rewriting_and_remains_available_after_append() {
    let root = Directory::new();
    fs::create_dir(&root.0).unwrap();
    let file = root.0.join("diagnostics.sqlite");
    let conn = Connection::open(&file).unwrap();
    conn.execute_batch(SCHEMA).unwrap();
    conn.execute(
        "INSERT INTO executions VALUES (?1,?2,1.0,9,'probe','success',1)",
        params!["a".repeat(32), DIGEST],
    )
    .unwrap();
    drop(conn);
    let before = fs::read(&file).unwrap();
    let rows = diagnostics::recent(&root.0, DIGEST).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].duration_ms, 9);
    assert_eq!(fs::read(&file).unwrap(), before);
    let mut observation = Observation::new(&root.0, DIGEST);
    observation.call("strategy.close");
    observation.finish("success");
    let rows = diagnostics::recent(&root.0, DIGEST).unwrap();
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[1].id, "a".repeat(32));
    assert_eq!(rows[1].duration_ms, 9);
}

#[test]
fn unsupported_schema_and_corrupt_database_are_never_migrated_or_repaired() {
    for corrupt in [false, true] {
        let root = Directory::new();
        fs::create_dir(&root.0).unwrap();
        let file = root.0.join("diagnostics.sqlite");
        if corrupt {
            fs::write(&file, b"private unsupported database bytes").unwrap();
        } else {
            let conn = Connection::open(&file).unwrap();
            conn.execute_batch(
                "CREATE TABLE executions (id TEXT PRIMARY KEY, private TEXT NOT NULL)",
            )
            .unwrap();
            conn.execute(
                "INSERT INTO executions VALUES ('record','private input')",
                [],
            )
            .unwrap();
        }
        let before = fs::read(&file).unwrap();
        Observation::new(&root.0, DIGEST).finish("success");
        diagnostics::record_event(&root.0, Component::Worker, EventCode::ExecutionFailed, None);
        assert!(diagnostics::recent(&root.0, DIGEST).is_err());
        assert_eq!(fs::read(&file).unwrap(), before);
    }
}

#[test]
fn completed_observation_is_once_only_and_never_stores_free_form_error_data() {
    let root = Directory::new();
    let mut observation = Observation::new(&root.0, DIGEST);
    observation.call("private request body: bearer token");
    observation.finish("timeout");
    observation.call("probe");
    observation.finish("success");
    let rows = diagnostics::recent(&root.0, DIGEST).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].phase, "request");
    assert_eq!(rows[0].code, "timeout");
    assert_eq!(rows[0].calls, 1);
    Observation::new(&root.0, DIGEST).finish("private exception string");
    assert_eq!(diagnostics::recent(&root.0, DIGEST).unwrap().len(), 1);
    assert!(
        !String::from_utf8_lossy(&fs::read(root.0.join("diagnostics.sqlite")).unwrap())
            .contains("private")
    );
}

#[test]
fn concurrent_writers_keep_atomic_record_limits_and_private_files() {
    let root = Directory::new();
    std::thread::scope(|scope| {
        for _ in 0..8 {
            let path = &root.0;
            scope.spawn(move || {
                for _ in 0..30 {
                    let mut observation = Observation::new(path, DIGEST);
                    observation.call("probe");
                    observation.finish("success");
                    diagnostics::record_event(
                        path,
                        Component::Worker,
                        EventCode::ExecutionFailed,
                        None,
                    );
                }
            });
        }
    });
    let conn = Connection::open(root.0.join("diagnostics.sqlite")).unwrap();
    for table in ["executions", "runtime_events"] {
        let count: i64 = conn
            .query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
                row.get(0)
            })
            .unwrap();
        assert_eq!(count, 200);
    }
    assert_eq!(diagnostics::recent(&root.0, DIGEST).unwrap().len(), 30);
    assert_eq!(diagnostics::events(&root.0).unwrap().len(), 30);
    assert!(
        fs::metadata(root.0.join("diagnostics.sqlite"))
            .unwrap()
            .len()
            <= diagnostics::MAX_DATABASE_BYTES
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            fs::metadata(&root.0).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            fs::metadata(root.0.join("diagnostics.sqlite"))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
    }
}

#[test]
fn missing_reads_and_failed_writes_do_not_create_or_modify_unrelated_files() {
    let root = Directory::new();
    assert!(diagnostics::recent(&root.0, DIGEST).unwrap().is_empty());
    assert!(!root.0.exists());
    Observation::new(&root.0, "private invalid identity").finish("success");
    assert!(!root.0.exists());
    fs::write(&root.0, b"preserve unrelated file").unwrap();
    Observation::new(&root.0, DIGEST).finish("success");
    diagnostics::record_event(
        &root.0,
        Component::Supervisor,
        EventCode::SubprocessFailed,
        None,
    );
    assert_eq!(fs::read(&root.0).unwrap(), b"preserve unrelated file");
    fs::remove_file(&root.0).unwrap();
}

#[test]
fn runtime_events_bind_only_valid_context_ids_even_after_deadline() {
    let root = Directory::new();
    let context = asterion_kernel::communication::context_at(None, 1.0, 1000).unwrap();
    diagnostics::record_event(
        &root.0,
        Component::Worker,
        EventCode::ExecutionFailed,
        Some(&context),
    );
    let rows = diagnostics::events(&root.0).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].request_id.as_ref(), Some(&context.request_id));
    assert_eq!(
        rows[0].correlation_id.as_ref(),
        Some(&context.correlation_id)
    );
    let mut invalid = context;
    invalid.request_id = "private invalid trace".into();
    diagnostics::record_event(
        &root.0,
        Component::Worker,
        EventCode::ExecutionFailed,
        Some(&invalid),
    );
    assert_eq!(diagnostics::events(&root.0).unwrap().len(), 1);
    assert!(
        !String::from_utf8_lossy(&fs::read(root.0.join("diagnostics.sqlite")).unwrap())
            .contains("private")
    );
}

#[test]
fn valid_nonhex_unicode_context_does_not_poison_journal_reads_or_writes() {
    let root = Directory::new();
    let mut context = asterion_kernel::communication::context_at(None, 1.0, 1000).unwrap();
    context.request_id = "z".repeat(32);
    context.correlation_id = "界".repeat(32);
    assert!(asterion_kernel::communication::parse_context(serde_json::json!(context)).is_ok());
    diagnostics::record_event(
        &root.0,
        Component::Worker,
        EventCode::ExecutionFailed,
        Some(&context),
    );
    let rows = diagnostics::events(&root.0).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].request_id.as_ref(), Some(&context.request_id));
    assert_eq!(
        rows[0].correlation_id.as_ref(),
        Some(&context.correlation_id)
    );
    diagnostics::record_event(
        &root.0,
        Component::Supervisor,
        EventCode::SubprocessFailed,
        None,
    );
    Observation::new(&root.0, DIGEST).finish("success");
    assert_eq!(diagnostics::events(&root.0).unwrap().len(), 2);
    assert_eq!(diagnostics::recent(&root.0, DIGEST).unwrap().len(), 1);
}

#[test]
fn oversized_journal_is_rejected_without_truncation() {
    let root = Directory::new();
    fs::create_dir(&root.0).unwrap();
    let file = root.0.join("diagnostics.sqlite");
    let bytes = vec![b'x'; diagnostics::MAX_DATABASE_BYTES as usize + 1];
    fs::write(&file, &bytes).unwrap();
    Observation::new(&root.0, DIGEST).finish("success");
    assert!(diagnostics::recent(&root.0, DIGEST).is_err());
    assert_eq!(fs::read(file).unwrap(), bytes);
}
