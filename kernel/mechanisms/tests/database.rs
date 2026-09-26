use asterion_kernel::database::{Connection, Database};
use serde_json::{Value, json};
use std::time::{Duration, Instant};

fn config() -> String {
    json!({"backend":"sqlite", "path":":memory:"}).to_string()
}
fn execute(conn: &Connection, sql: &str, params: Vec<Value>) {
    conn.execute(sql, params, false).unwrap();
}
fn rows(conn: &Connection, sql: &str) -> Vec<Vec<Value>> {
    conn.execute(sql, vec![], false)
        .unwrap()
        .fetchmany(1000)
        .unwrap()
}

#[test]
fn sqlite_transactions_values_returning_and_integrity() {
    let conn = Connection::open(&config()).unwrap();
    conn.begin().unwrap();
    conn.begin().unwrap();
    execute(
        &conn,
        "CREATE TABLE facts (id INTEGER PRIMARY KEY NOT NULL, body JSON, enabled BOOLEAN NOT NULL, amount FLOAT, large BIGINT)",
        vec![],
    );
    let cursor = conn
        .execute(
            "INSERT INTO facts VALUES (?, ?, ?, ?, ?) RETURNING id",
            vec![
                json!(1),
                json!("{\"s\":\"中文\"}"),
                json!(true),
                json!(1.25),
                json!(9_007_199_254_740_993_i64),
            ],
            false,
        )
        .unwrap();
    assert_eq!(cursor.rowcount, 1);
    assert_eq!(cursor.lastrowid, Some(1));
    assert_eq!(cursor.fetchmany(1).unwrap(), [vec![json!(1)]]);
    conn.commit().unwrap();
    let returned = rows(&conn, "SELECT body, enabled, amount, large FROM facts");
    assert_eq!(
        returned[0],
        [
            json!("{\"s\":\"中文\"}"),
            json!(1),
            json!(1.25),
            json!(9_007_199_254_740_993_i64)
        ]
    );
    execute(
        &conn,
        "INSERT INTO facts VALUES (?, ?, ?, ?, ?)",
        vec![
            json!(2),
            Value::Null,
            json!(false),
            Value::Null,
            Value::Null,
        ],
    );
    conn.rollback().unwrap();
    assert_eq!(rows(&conn, "SELECT count(*) FROM facts"), [vec![json!(1)]]);
    let failure = conn
        .execute(
            "INSERT INTO facts VALUES (1,NULL,1,NULL,NULL)",
            vec![],
            false,
        )
        .err()
        .unwrap();
    assert_eq!(failure.code, "integrity");
    assert!(!failure.message.contains("facts"));
    conn.rollback().unwrap();
    conn.close().unwrap();
    assert_eq!(conn.begin().unwrap_err().code, "closed");
}

#[test]
fn sqlite_true_stream_is_exclusive_cancelable_and_bounded() {
    let conn = Connection::open(&config()).unwrap();
    let cursor = conn.execute("WITH RECURSIVE n(v) AS (VALUES(1) UNION ALL SELECT v+1 FROM n WHERE v<100005) SELECT v FROM n", vec![], true).unwrap();
    assert_eq!(cursor.fetchmany(1000).unwrap().len(), 1000);
    assert_eq!(
        conn.execute("SELECT 1", vec![], false).err().unwrap().code,
        "busy"
    );
    let mut count = 1000;
    loop {
        let batch = cursor.fetchmany(997).unwrap();
        if batch.is_empty() {
            break;
        }
        count += batch.len();
    }
    assert_eq!(count, 100005);
    assert_eq!(rows(&conn, "SELECT 7"), [vec![json!(7)]]);
    let rejected = conn.execute("WITH RECURSIVE n(v) AS (VALUES(1) UNION ALL SELECT v+1 FROM n WHERE v<100001) SELECT v FROM n", vec![], false).err().unwrap();
    assert_eq!(rejected.code, "data");
    assert_eq!(conn.commit().unwrap_err().code, "operational");
    conn.rollback().unwrap();
    let cursor = conn
        .execute("SELECT 1 UNION ALL SELECT 2", vec![], true)
        .unwrap();
    conn.rollback().unwrap();
    assert_eq!(cursor.fetchmany(1).unwrap_err().code, "closed");
    cursor.close().unwrap();
    assert_eq!(rows(&conn, "SELECT 3"), [vec![json!(3)]]);
}

#[test]
fn sqlite_schema_observation_is_read_only_and_exact() {
    let conn = Connection::open(&config()).unwrap();
    execute(
        &conn,
        "CREATE TABLE parent (id VARCHAR NOT NULL PRIMARY KEY)",
        vec![],
    );
    execute(
        &conn,
        "CREATE TABLE child (id INTEGER NOT NULL PRIMARY KEY, parent_id VARCHAR NOT NULL, value JSON, UNIQUE(parent_id,value), FOREIGN KEY(parent_id) REFERENCES parent(id))",
        vec![],
    );
    execute(
        &conn,
        "CREATE INDEX child_parent ON child(parent_id)",
        vec![],
    );
    conn.commit().unwrap();
    assert_eq!(conn.table_names().unwrap(), ["child", "parent"]);
    let schema = conn.observe_schema().unwrap();
    assert_eq!(schema[0].columns[0].name, "id");
    assert!(!schema[0].columns[0].nullable);
    assert_eq!(schema[0].primary, ["id"]);
    assert_eq!(schema[0].unique, [vec!["parent_id", "value"]]);
    assert_eq!(schema[0].foreign[0].references, ["id"]);
    assert_eq!(schema[0].foreign[0].table, "parent");
    assert_eq!(schema[0].indexes.len(), 2);
    assert_eq!(rows(&conn, "SELECT count(*) FROM child"), [vec![json!(0)]]);
}

#[test]
fn native_pool_preserves_memory_rolls_back_and_invalidates_returned_cursors() {
    let pool = Database::new(config(), 1, 0.05).unwrap();
    let mut lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    execute(
        conn,
        "CREATE TABLE facts(id INTEGER NOT NULL PRIMARY KEY)",
        vec![],
    );
    conn.commit().unwrap();
    execute(conn, "INSERT INTO facts VALUES (1)", vec![]);
    let retained = conn.execute("SELECT id FROM facts", vec![], false).unwrap();
    let started = Instant::now();
    assert!(pool.connect().is_err());
    assert!(started.elapsed() >= Duration::from_millis(40));
    lease.close().unwrap();
    assert_eq!(retained.fetchmany(1).unwrap_err().code, "closed");
    let next = pool.connect().unwrap();
    assert_eq!(
        rows(next.connection().unwrap(), "SELECT count(*) FROM facts"),
        [vec![json!(0)]]
    );
    drop(next);
    pool.dispose().unwrap();
    assert!(
        pool.connect()
            .unwrap()
            .connection()
            .unwrap()
            .table_names()
            .unwrap()
            .is_empty()
    );
}

#[test]
fn pool_disposal_preserves_outstanding_lease_and_retires_its_generation() {
    let pool = Database::new(config(), 1, 0.1).unwrap();
    let lease = pool.connect().unwrap();
    execute(
        lease.connection().unwrap(),
        "CREATE TABLE old(id INTEGER)",
        vec![],
    );
    lease.connection().unwrap().commit().unwrap();
    pool.dispose().unwrap();
    assert_eq!(lease.connection().unwrap().table_names().unwrap(), ["old"]);
    let next = pool.connect().unwrap();
    assert!(next.connection().unwrap().table_names().unwrap().is_empty());
    drop(lease);
    drop(next);
    assert!(
        pool.connect()
            .unwrap()
            .connection()
            .unwrap()
            .table_names()
            .unwrap()
            .is_empty()
    );
}

#[test]
fn autocommit_is_explicit_and_streaming_dml_is_rejected() {
    let conn = Connection::open(&config()).unwrap();
    conn.set_autocommit(true).unwrap();
    conn.begin().unwrap();
    execute(&conn, "CREATE TABLE facts(id INTEGER)", vec![]);
    conn.set_autocommit(false).unwrap();
    assert_eq!(
        conn.execute("INSERT INTO facts VALUES (1) RETURNING id", vec![], true)
            .err()
            .unwrap()
            .code,
        "programming"
    );
    assert_eq!(conn.commit().unwrap_err().code, "operational");
    conn.rollback().unwrap();
    assert_eq!(rows(&conn, "SELECT count(*) FROM facts"), [vec![json!(0)]]);
    conn.rollback().unwrap();
}

fn postgres() -> Connection {
    Connection::open(
        &std::env::var("ASTERION_NATIVE_DATABASE_TEST_CONFIG")
            .expect("isolated PostgreSQL JSON config"),
    )
    .unwrap()
}
#[test]
#[ignore = "requires an isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn postgres_real_values_transactions_and_streaming() {
    let conn = postgres();
    execute(
        &conn,
        "CREATE TEMP TABLE native_facts (id INTEGER PRIMARY KEY, large BIGINT, body JSON, enabled BOOLEAN, amount DOUBLE PRECISION)",
        vec![],
    );
    let cursor = conn
        .execute(
            "INSERT INTO native_facts VALUES ($1,$2,$3::JSON,$4,$5) RETURNING id",
            vec![
                json!(1),
                json!(9_007_199_254_740_993_i64),
                json!("{\"n\":1}"),
                json!(true),
                json!(1.25),
            ],
            false,
        )
        .unwrap();
    assert_eq!(cursor.rowcount, 1);
    assert_eq!(cursor.fetchmany(1).unwrap(), [vec![json!(1)]]);
    conn.commit().unwrap();
    assert_eq!(
        rows(&conn, "SELECT large,body,enabled,amount FROM native_facts")[0],
        [
            json!(9_007_199_254_740_993_i64),
            json!("{\"n\":1}"),
            json!(true),
            json!(1.25)
        ]
    );
    execute(
        &conn,
        "INSERT INTO native_facts(id) VALUES ($1)",
        vec![json!(2)],
    );
    conn.rollback().unwrap();
    assert_eq!(
        rows(&conn, "SELECT count(*) FROM native_facts"),
        [vec![json!(1)]]
    );
    let cursor = conn
        .execute(
            "SELECT generate_series(1,$1) AS value",
            vec![json!(100005)],
            true,
        )
        .unwrap();
    let mut count = 0;
    loop {
        let batch = cursor.fetchmany(1000).unwrap();
        if batch.is_empty() {
            break;
        }
        count += batch.len();
    }
    assert_eq!(count, 100005);
    assert_eq!(rows(&conn, "SELECT 1"), [vec![json!(1)]]);
    let error = conn
        .execute("SELECT $1::INTEGER", vec![json!(i64::MAX)], false)
        .err()
        .unwrap();
    assert_eq!(error.code, "data");
    conn.rollback().unwrap();
    let error = conn
        .execute("SELECT $1::REAL", vec![json!(f64::MAX)], false)
        .err()
        .unwrap();
    assert_eq!(error.code, "data");
}

#[test]
#[ignore = "requires an isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn postgres_real_schema_and_json_null() {
    let conn = postgres();
    let suffix = uuid::Uuid::new_v4().simple().to_string();
    let parent = format!("native_parent_{suffix}");
    let child = format!("native_child_{suffix}");
    execute(
        &conn,
        &format!("CREATE TABLE {parent} (id VARCHAR NOT NULL PRIMARY KEY)"),
        vec![],
    );
    execute(
        &conn,
        &format!(
            "CREATE TABLE {child} (id INTEGER NOT NULL PRIMARY KEY, parent_id VARCHAR NOT NULL REFERENCES {parent}(id), code VARCHAR NOT NULL UNIQUE, body JSON, enabled BOOLEAN NOT NULL, amount FLOAT)"
        ),
        vec![],
    );
    let schema = conn.observe_schema().unwrap();
    let child = schema.iter().find(|schema| schema.name == child).unwrap();
    assert_eq!(child.primary, ["id"]);
    assert_eq!(child.unique, [vec!["code"]]);
    assert_eq!(child.foreign[0].table, parent);
    assert_eq!(child.columns.last().unwrap().kind, "FLOAT");
    assert_eq!(
        conn.execute(
            "SELECT $1::JSON, $2::JSON",
            vec![json!("null"), Value::Null],
            false
        )
        .unwrap()
        .fetchmany(1)
        .unwrap(),
        [vec![json!("null"), Value::Null]]
    );
    conn.rollback().unwrap();
    assert!(
        !conn
            .table_names()
            .unwrap()
            .iter()
            .any(|name| name.contains(&suffix))
    );
}

#[test]
fn caught_database_error_cannot_publish_partial_sqlite_transaction() {
    let conn = Connection::open(&config()).unwrap();
    execute(
        &conn,
        "CREATE TABLE facts(id INTEGER PRIMARY KEY NOT NULL)",
        vec![],
    );
    conn.commit().unwrap();
    execute(&conn, "INSERT INTO facts VALUES (1)", vec![]);
    assert_eq!(
        conn.execute("INSERT INTO facts VALUES (1)", vec![], false)
            .err()
            .unwrap()
            .code,
        "integrity"
    );
    assert_eq!(conn.commit().unwrap_err().code, "operational");
    conn.rollback().unwrap();
    assert_eq!(rows(&conn, "SELECT count(*) FROM facts"), [vec![json!(0)]]);
}

#[test]
#[ignore = "requires an isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn caught_postgres_error_never_reports_rolled_back_commit_as_success() {
    let conn = postgres();
    execute(
        &conn,
        "CREATE TEMP TABLE partial(id INTEGER PRIMARY KEY)",
        vec![],
    );
    conn.commit().unwrap();
    execute(&conn, "INSERT INTO partial VALUES (1)", vec![]);
    assert_eq!(
        conn.execute("INSERT INTO partial VALUES (1)", vec![], false)
            .err()
            .unwrap()
            .code,
        "integrity"
    );
    assert_eq!(conn.commit().unwrap_err().code, "operational");
    conn.rollback().unwrap();
    assert_eq!(
        rows(&conn, "SELECT count(*) FROM partial"),
        [vec![json!(0)]]
    );
}

#[test]
fn sqlite_batch_prepares_once_and_failure_prevents_partial_commit() {
    let conn = Connection::open(&config()).unwrap();
    execute(
        &conn,
        "CREATE TABLE facts(id INTEGER NOT NULL PRIMARY KEY)",
        vec![],
    );
    conn.commit().unwrap();
    let batch = conn
        .executemany(
            "INSERT INTO facts VALUES (?)",
            (0..5000).map(|i| vec![json!(i)]).collect(),
        )
        .unwrap();
    assert_eq!(batch.rowcount, 5000);
    conn.commit().unwrap();
    assert_eq!(
        conn.executemany(
            "INSERT INTO facts VALUES (?)",
            vec![vec![json!(6000)], vec![json!(6000)]]
        )
        .err()
        .unwrap()
        .code,
        "integrity"
    );
    assert_eq!(conn.commit().unwrap_err().code, "operational");
    conn.rollback().unwrap();
    assert_eq!(
        rows(&conn, "SELECT count(*) FROM facts"),
        [vec![json!(5000)]]
    );
}

#[test]
fn pool_waiter_wakes_when_a_streaming_lease_is_returned() {
    let pool = std::sync::Arc::new(Database::new(config(), 1, 2.0).unwrap());
    let lease = pool.connect().unwrap();
    let cursor = lease
        .connection()
        .unwrap()
        .execute("SELECT 1 UNION ALL SELECT 2", vec![], true)
        .unwrap();
    let other = pool.clone();
    let thread = std::thread::spawn(move || {
        let lease = other.connect().unwrap();
        assert_eq!(
            rows(lease.connection().unwrap(), "SELECT 7"),
            [vec![json!(7)]]
        );
    });
    drop(lease);
    thread.join().unwrap();
    assert_eq!(cursor.fetchmany(1).unwrap_err().code, "closed");
}

#[test]
#[ignore = "requires an isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn postgres_native_batch_and_broken_pool_lease() {
    let raw = std::env::var("ASTERION_NATIVE_DATABASE_TEST_CONFIG").unwrap();
    let pool = Database::new(raw, 1, 1.0).unwrap();
    let mut lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    execute(
        conn,
        "CREATE TEMP TABLE batch(id INTEGER PRIMARY KEY)",
        vec![],
    );
    let batch = conn
        .executemany(
            "INSERT INTO batch VALUES ($1)",
            (0..2000).map(|i| vec![json!(i)]).collect(),
        )
        .unwrap();
    assert_eq!(batch.rowcount, 2000);
    conn.commit().unwrap();
    assert_eq!(
        rows(conn, "SELECT count(*) FROM batch"),
        [vec![json!(2000)]]
    );
    assert!(
        conn.execute(
            "SELECT pg_terminate_backend(pg_backend_pid())",
            vec![],
            false
        )
        .is_err()
    );
    assert!(lease.close().is_err());
    let replacement = pool.connect().unwrap();
    assert_eq!(
        rows(replacement.connection().unwrap(), "SELECT 7"),
        [vec![json!(7)]]
    );
}
