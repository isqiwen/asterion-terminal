use asterion_kernel::{
    database::{Connection, Database},
    events::Topic,
    storage::Scope,
    tasks::repository::{Repository, Request},
};
use serde_json::{Value, json};

fn trace() -> Value {
    json!({"version":1,"request_id":"a".repeat(32),"correlation_id":"b".repeat(32),"causation_id":null,"deadline_ms":1})
}
fn setup() -> (Database, Repository) {
    let pool = Database::new(
        json!({"backend":"sqlite","path":":memory:"}).to_string(),
        1,
        1.,
    )
    .unwrap();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    for sql in [
        "CREATE TABLE jobs(id TEXT PRIMARY KEY,command_id TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,payload JSON NOT NULL,state TEXT NOT NULL,attempt INTEGER NOT NULL,token TEXT,worker_id TEXT,lease_until FLOAT,created_at FLOAT NOT NULL,error TEXT,result JSON)",
        "CREATE TABLE communication_heads(topic TEXT PRIMARY KEY,sequence BIGINT NOT NULL)",
        "CREATE TABLE communication_events(id TEXT PRIMARY KEY,topic TEXT NOT NULL,sequence BIGINT NOT NULL,stream TEXT NOT NULL,message JSON NOT NULL,UNIQUE(topic,sequence))",
    ] {
        conn.execute(sql, vec![], false).unwrap();
    }
    conn.commit().unwrap();
    let repository = Repository::new(
        pool.id(),
        Topic {
            id: "runtime.task.changed".into(),
            owner: "asterion.runtime".into(),
            payload: "task".into(),
            read_path: "/jobs".into(),
        },
    )
    .unwrap();
    (pool, repository)
}
fn run(repo: &Repository, conn: &Connection, value: Value) -> Value {
    repo.run(conn, serde_json::from_value(value).unwrap(), trace())
        .unwrap()
}
fn record(id: &str, kind: &str) -> Value {
    json!({"id":id,"command_id":id,"kind":kind,"payload":{},"now":100.})
}
#[test]
fn native_task_and_journal_rollback_together_even_if_caller_catches_the_error() {
    let (pool, repo) = setup();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    conn.begin_write().unwrap();
    run(
        &repo,
        conn,
        json!({"op":"submit","record":record("one","fixture.read")}),
    );
    conn.commit().unwrap();
    conn.begin_write().unwrap();
    let claimed = run(
        &repo,
        conn,
        json!({"op":"claim","worker_id":"worker","now":100.,"lease_seconds":60.,"token":"token"}),
    );
    conn.commit().unwrap();
    conn.execute(
        "UPDATE communication_heads SET sequence=9223372036854775807",
        vec![],
        false,
    )
    .unwrap();
    conn.commit().unwrap();
    conn.begin_write().unwrap();
    let request:Request=serde_json::from_value(json!({"op":"apply","id":claimed["id"],"now":120.,"action":{"type":"complete","token":"token","result":{"fact":true}}})).unwrap();
    assert!(repo.run(conn, request, trace()).is_err());
    assert!(conn.commit().is_err());
    conn.rollback().unwrap();
    conn.begin().unwrap();
    let row = run(&repo, conn, json!({"op":"get","id":"one"}));
    assert_eq!(row["state"], "RUNNING");
    assert_eq!(row["result"], Value::Null);
    conn.rollback().unwrap();
}
#[test]
fn task_grants_reject_a_mixed_batch_before_any_state_or_event_can_commit() {
    let (pool, repo) = setup();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    let scope = Scope::new(pool.id(), vec![], vec![]);
    let grant = repo.granted(["fixture.read".into()].into_iter().collect());
    conn.begin_write().unwrap();
    let transaction = scope.begin(true).unwrap();
    transaction.bind(conn.transaction().unwrap()).unwrap();
    let request=serde_json::from_value(json!({"op":"submit_batch","records":[record("one","fixture.read"),record("two","fixture.write")]})).unwrap();
    assert!(grant.run(&transaction, request, trace()).is_err());
    assert!(transaction.commit_ready().is_err());
    assert!(conn.commit().is_err());
    conn.rollback().unwrap();
    conn.begin().unwrap();
    assert_eq!(run(&repo, conn, json!({"op":"list"})), json!([]));
    conn.rollback().unwrap();
}
#[test]
fn committed_lease_reclaim_and_idempotency_use_current_database_state() {
    let (pool, repo) = setup();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    conn.begin_write().unwrap();
    run(
        &repo,
        conn,
        json!({"op":"submit","record":record("one","fixture.read")}),
    );
    let first = run(
        &repo,
        conn,
        json!({"op":"claim","worker_id":"a","now":100.,"lease_seconds":60.,"token":"first"}),
    );
    conn.commit().unwrap();
    conn.begin_write().unwrap();
    let second = run(
        &repo,
        conn,
        json!({"op":"claim","worker_id":"b","now":160.01,"lease_seconds":60.,"token":"second"}),
    );
    assert_eq!(second["attempt"], 2);
    assert_eq!(first["id"], second["id"]);
    conn.commit().unwrap();
    conn.begin_write().unwrap();
    assert!(
        repo.run(
            conn,
            serde_json::from_value(json!({"op":"lease","id":"one","token":"first","now":161.}))
                .unwrap(),
            trace()
        )
        .is_err()
    );
    conn.rollback().unwrap();
    conn.begin_write().unwrap();
    let reused = run(
        &repo,
        conn,
        json!({"op":"submit","record":record("one","fixture.read")}),
    );
    assert_eq!(reused["attempt"], 2);
    conn.commit().unwrap();
}

#[test]
fn claim_scopes_give_each_task_kind_exactly_one_executor() {
    let (pool, repo) = setup();
    let lease = pool.connect().unwrap();
    let conn = lease.connection().unwrap();
    conn.begin_write().unwrap();
    for (id, kind) in [
        ("native", "data.import_csv"),
        ("python", "research.backtest"),
    ] {
        run(
            &repo,
            conn,
            json!({"op":"submit","record":record(id, kind)}),
        );
    }
    conn.commit().unwrap();
    let claim = |scope: Value, token: &str| {
        conn.begin_write().unwrap();
        let claimed = run(
            &repo,
            conn,
            json!({"op":"claim","worker_id":"w","now":100.,"lease_seconds":60.,"token":token,"kinds":scope}),
        );
        conn.commit().unwrap();
        claimed["id"].clone()
    };
    let except = json!({"scope":"except","kinds":["data.import_csv"]});
    let only = json!({"scope":"only","kinds":["data.import_csv"]});
    assert_eq!(claim(except.clone(), "a"), json!("python"));
    assert_eq!(claim(except, "b"), Value::Null);
    assert_eq!(claim(only.clone(), "c"), json!("native"));
    assert_eq!(claim(only, "d"), Value::Null);
    conn.begin_write().unwrap();
    let empty: Request = serde_json::from_value(
        json!({"op":"claim","worker_id":"w","now":100.,"lease_seconds":60.,"token":"e","kinds":{"scope":"only","kinds":[]}}),
    )
    .unwrap();
    assert!(repo.run(conn, empty, trace()).is_err());
    conn.rollback().unwrap();
}
