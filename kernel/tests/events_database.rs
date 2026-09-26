use asterion_kernel::{
    database::{Connection, Database},
    events::{Journal, Topic, validate_journal},
    storage::Scope,
};
use serde_json::{Value, json};
use std::{sync::Arc, time::Duration};
fn topic() -> Topic {
    Topic {
        id: "fixture.changed".into(),
        owner: "fixture.owner".into(),
        payload: "fixture-payload".into(),
        read_path: "/fixture".into(),
    }
}
fn trace() -> Value {
    json!({"version":1,"request_id":"a".repeat(32),"correlation_id":"b".repeat(32),"causation_id":null,"deadline_ms":1})
}
fn run(conn: &Connection, sql: &str) {
    conn.execute(sql, vec![], false).unwrap();
}
fn setup(conn: &Connection, temp: bool) {
    let prefix = if temp {
        "CREATE TEMP TABLE"
    } else {
        "CREATE TABLE"
    };
    for schema in [
        "business(id BIGINT PRIMARY KEY)",
        "communication_heads(topic TEXT PRIMARY KEY,sequence BIGINT NOT NULL)",
        "communication_events(id TEXT PRIMARY KEY,topic TEXT NOT NULL,sequence BIGINT NOT NULL,stream TEXT NOT NULL,message JSON NOT NULL,UNIQUE(topic,sequence))",
    ] {
        run(conn, &format!("{prefix} {schema}"));
    }
    conn.commit().unwrap();
}
fn count(conn: &Connection, table: &str) -> Value {
    conn.execute(&format!("SELECT count(*) FROM {table}"), vec![], false)
        .unwrap()
        .fetchmany(1)
        .unwrap()[0][0]
        .clone()
}
fn sqlite() -> Database {
    Database::new(
        json!({"backend":"sqlite","path":":memory:"}).to_string(),
        1,
        1.,
    )
    .unwrap()
}
#[test]
fn business_and_journal_share_the_scope_transaction_and_rollback() {
    let pool = sqlite();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    let journal = Journal::new(pool.id(), vec![topic()]).unwrap();
    let writer = journal.publisher("fixture.owner", &[topic()]).unwrap();
    let scope = Scope::new(pool.id(), vec![1], vec![]);
    conn.begin_write().unwrap();
    let tx = scope.begin(true).unwrap();
    tx.bind(conn.transaction().unwrap()).unwrap();
    run(conn, "INSERT INTO business VALUES(1)");
    let message = writer
        .publish(&tx, topic(), "one".into(), json!({"value":1}), trace())
        .unwrap();
    assert_eq!(message["sequence"], "1");
    tx.abort();
    conn.rollback().unwrap();
    assert_eq!(count(conn, "business"), 0);
    assert_eq!(count(conn, "communication_events"), 0);
    assert_eq!(count(conn, "communication_heads"), 0);
    conn.rollback().unwrap();
    conn.begin_write().unwrap();
    let tx = scope.begin(true).unwrap();
    tx.bind(conn.transaction().unwrap()).unwrap();
    run(conn, "INSERT INTO business VALUES(2)");
    writer
        .publish(&tx, topic(), "two".into(), json!({"value":2}), trace())
        .unwrap();
    tx.commit_ready().unwrap();
    conn.commit().unwrap();
    conn.begin().unwrap();
    let page = journal
        .read(
            conn,
            journal
                .registry()
                .read_plan("fixture.changed", "0", 100)
                .unwrap(),
        )
        .unwrap();
    assert_eq!(page["items"][0]["payload"]["value"], 2);
    assert_eq!(validate_journal(conn).unwrap(), 1);
    conn.rollback().unwrap();
}
#[test]
fn physical_tickets_expire_at_commit_rollback_pool_return_and_cannot_cross_pools() {
    let pool = sqlite();
    let mut lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    assert!(conn.transaction().is_err());
    conn.begin().unwrap();
    let ticket = conn.transaction().unwrap();
    conn.commit().unwrap();
    assert!(ticket.check().is_err());
    conn.begin().unwrap();
    assert!(ticket.check().is_err());
    let next = conn.transaction().unwrap();
    conn.rollback().unwrap();
    conn.begin().unwrap();
    assert!(next.check().is_err());
    let retained = conn.transaction().unwrap();
    let scope = Scope::new(pool.id(), vec![], vec![]);
    let tx = scope.begin(true).unwrap();
    tx.bind(retained.clone()).unwrap();
    assert!(tx.bind(retained.clone()).is_err());
    lease.close().unwrap();
    let again = pool.connect().unwrap();
    again.connection().unwrap().begin().unwrap();
    assert!(retained.check().is_err());
    assert!(tx.require_bound(true).is_err());
    assert!(tx.commit_ready().is_err());
    let foreign = sqlite();
    let foreign_lease = foreign.connect().unwrap();
    let fc = foreign_lease.connection().unwrap();
    fc.begin().unwrap();
    let tx = scope.begin(true).unwrap();
    assert!(tx.bind(fc.transaction().unwrap()).is_err());
    let journal = Journal::new(pool.id(), vec![topic()]).unwrap();
    assert!(
        journal
            .publish_host(fc, topic(), "x".into(), json!({}), trace())
            .is_err()
    );
}
#[test]
fn writer_revocation_and_readonly_grants_cannot_publish_or_commit() {
    let pool = sqlite();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    let journal = Journal::new(pool.id(), vec![topic()]).unwrap();
    let writer = journal.publisher("fixture.owner", &[topic()]).unwrap();
    let a = Scope::new(pool.id(), vec![1], vec![]);
    let b = Scope::new(pool.id(), vec![2], vec![]);
    conn.begin().unwrap();
    let reader = a.begin(false).unwrap();
    reader.bind(conn.transaction().unwrap()).unwrap();
    assert!(
        writer
            .publish(&reader, topic(), "x".into(), json!({}), trace())
            .is_err()
    );
    reader.commit_ready().unwrap();
    conn.rollback().unwrap();
    conn.begin_write().unwrap();
    let root = a.begin(true).unwrap();
    root.bind(conn.transaction().unwrap()).unwrap();
    let joined = b.join(&root, true).unwrap();
    writer
        .publish(&joined, topic(), "x".into(), json!({}), trace())
        .unwrap();
    joined.close();
    b.close();
    assert!(root.commit_ready().is_err());
    conn.rollback().unwrap();
    assert_eq!(count(conn, "communication_events"), 0);
}
#[test]
fn caught_invalid_head_or_insert_failure_poison_the_same_business_transaction() {
    let pool = sqlite();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    let journal = Journal::new(pool.id(), vec![topic()]).unwrap();
    run(
        conn,
        "INSERT INTO communication_heads VALUES('fixture.changed',9223372036854775807)",
    );
    conn.commit().unwrap();
    run(conn, "INSERT INTO business VALUES(1)");
    assert!(
        journal
            .publish_host(conn, topic(), "x".into(), json!({}), trace())
            .is_err()
    );
    assert!(conn.commit().is_err());
    conn.rollback().unwrap();
    assert_eq!(count(conn, "business"), 0);
    conn.rollback().unwrap();
    run(conn, "UPDATE communication_heads SET sequence=0");
    run(
        conn,
        "CREATE TRIGGER prevent_event BEFORE INSERT ON communication_events BEGIN SELECT RAISE(ABORT,'fixture'); END",
    );
    conn.commit().unwrap();
    run(conn, "INSERT INTO business VALUES(1)");
    assert!(
        journal
            .publish_host(conn, topic(), "x".into(), json!({}), trace())
            .is_err()
    );
    assert!(conn.commit().is_err());
    conn.rollback().unwrap();
    assert_eq!(count(conn, "business"), 0);
    assert_eq!(
        conn.execute("SELECT sequence FROM communication_heads", vec![], false)
            .unwrap()
            .fetchmany(1)
            .unwrap()[0][0],
        0
    );
}
#[test]
fn sqlite_writer_reservation_waits_then_reads_committed_state_without_upgrade_deadlock() {
    let path = std::env::temp_dir().join(format!("asterion-events-{}.db", uuid::Uuid::new_v4()));
    let config = json!({"backend":"sqlite","path":path,"timeout_ms":2000}).to_string();
    let left = Connection::open(&config).unwrap();
    setup(&left, false);
    let right = Connection::open(&config).unwrap();
    left.begin_write().unwrap();
    run(&left, "INSERT INTO business VALUES(1)");
    let (started, wait) = std::sync::mpsc::channel();
    let (entered, acquired) = std::sync::mpsc::channel();
    let worker = std::thread::spawn(move || {
        started.send(()).unwrap();
        right.begin_write().unwrap();
        entered.send(()).unwrap();
        assert_eq!(count(&right, "business"), 1);
        run(&right, "INSERT INTO business VALUES(2)");
        right.rollback().unwrap();
        right.close().unwrap();
    });
    wait.recv().unwrap();
    assert!(acquired.recv_timeout(Duration::from_millis(50)).is_err());
    left.commit().unwrap();
    acquired.recv_timeout(Duration::from_secs(2)).unwrap();
    worker.join().unwrap();
    assert_eq!(count(&left, "business"), 1);
    left.close().unwrap();
    std::fs::remove_file(path).unwrap();
}
#[test]
#[ignore = "requires isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn postgres_journal_uses_existing_transaction_and_validates_multiple_fetch_batches() {
    let conn =
        Connection::open(&std::env::var("ASTERION_NATIVE_DATABASE_TEST_CONFIG").unwrap()).unwrap();
    setup(&conn, true);
    let journal = Arc::new(Journal::new(conn.database_id(), vec![topic()]).unwrap());
    conn.begin().unwrap();
    for number in 0..1005 {
        journal
            .publish_host(
                &conn,
                topic(),
                number.to_string(),
                json!({"value":number}),
                trace(),
            )
            .unwrap();
    }
    conn.commit().unwrap();
    conn.begin().unwrap();
    assert_eq!(validate_journal(&conn).unwrap(), 1005);
    let page = journal
        .read(
            &conn,
            journal
                .registry()
                .read_plan("fixture.changed", "1000", 10)
                .unwrap(),
        )
        .unwrap();
    assert_eq!(page["cursor"], "1005");
    assert_eq!(page["items"].as_array().unwrap().len(), 5);
    conn.rollback().unwrap();
    run(&conn, "INSERT INTO business VALUES(1)");
    journal
        .publish_host(&conn, topic(), "rollback".into(), json!({}), trace())
        .unwrap();
    conn.rollback().unwrap();
    assert_eq!(count(&conn, "business"), 0);
    conn.begin().unwrap();
    assert_eq!(validate_journal(&conn).unwrap(), 1005);
    conn.rollback().unwrap();
    run(
        &conn,
        "UPDATE communication_events SET stream='corrupt' WHERE sequence=1005",
    );
    conn.commit().unwrap();
    conn.begin().unwrap();
    assert!(validate_journal(&conn).is_err());
    conn.rollback().unwrap();
    assert_eq!(
        conn.execute(
            "SELECT stream FROM communication_events WHERE sequence=1005",
            vec![],
            false
        )
        .unwrap()
        .fetchmany(1)
        .unwrap()[0][0],
        "corrupt"
    );
    conn.close().unwrap();
}

#[test]
fn raw_transaction_control_cannot_bypass_the_physical_epoch() {
    let pool = sqlite();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    conn.begin_write().unwrap();
    let ticket = conn.transaction().unwrap();
    run(conn, "INSERT INTO business VALUES(1)");
    for sql in [
        "COMMIT",
        " /* comment */ COMMIT",
        ";;-- comment\nROLLBACK",
        "/* nested /* comment */ x */ END",
        "BEGIN",
        "SAVEPOINT x",
        "RELEASE x",
        "START TRANSACTION",
        "PREPARE TRANSACTION 'x'",
        "ABORT",
    ] {
        assert_eq!(
            conn.execute(sql, vec![], false).err().unwrap().code,
            "programming"
        );
        assert!(ticket.check().is_ok());
        assert_eq!(
            conn.executemany(sql, vec![vec![]]).err().unwrap().code,
            "programming"
        );
    }
    conn.rollback().unwrap();
    assert!(ticket.check().is_err());
    assert_eq!(count(conn, "business"), 0);
}

#[test]
fn host_lifecycle_guards_publish_and_remain_commit_participants() {
    use asterion_kernel::plugins::{Grants, Host, Manifest};
    let manifest: Manifest = serde_json::from_value(json!({"id":"fixture.owner","layer":"L3","api_version":1,"requires":[],"type_requires":[],"provides":[],"consumes":[],"resources":[],"observes":[]})).unwrap();
    let mut host = Host::new(vec![manifest]).unwrap();
    assert!(host.context_handle("fixture.owner").is_err());
    host.begin(&Grants::new()).unwrap();
    assert!(host.context_handle("fixture.owner").is_err());
    host.next_activation().unwrap();
    let handle = host.context_handle("fixture.owner").unwrap();
    assert!(host.context_handle("another.owner").is_err());
    let pool = sqlite();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    setup(conn, false);
    let writer = Journal::new(pool.id(), vec![topic()])
        .unwrap()
        .publisher("fixture.owner", &[topic()])
        .unwrap()
        .guarded(handle.clone())
        .unwrap();
    assert!(writer.guarded(handle).is_err());
    let scope = Scope::new(pool.id(), vec![], vec![]);
    conn.begin_write().unwrap();
    let tx = scope.begin(true).unwrap();
    tx.bind(conn.transaction().unwrap()).unwrap();
    writer
        .publish(&tx, topic(), "pending".into(), json!({}), trace())
        .unwrap();
    host.close();
    assert!(
        writer
            .publish(&tx, topic(), "revoked".into(), json!({}), trace())
            .is_err()
    );
    assert!(tx.commit_ready().is_err());
    conn.rollback().unwrap();
    assert_eq!(count(conn, "communication_events"), 0);
    assert_eq!(count(conn, "communication_heads"), 0);
}
