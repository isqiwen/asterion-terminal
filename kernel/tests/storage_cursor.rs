use asterion_kernel::{
    database::Connection,
    storage::{Scope, Transaction},
};
use serde_json::json;
use std::sync::Arc;

fn sqlite() -> Connection {
    Connection::open(&json!({"backend":"sqlite", "path":":memory:"}).to_string()).unwrap()
}
fn owner(connection: &Connection) -> (Arc<Scope>, Arc<Transaction>) {
    connection.begin().unwrap();
    let scope = Scope::new(connection.database_id(), vec![1], vec![]);
    let transaction = scope.begin(true).unwrap();
    transaction.bind(connection.transaction().unwrap()).unwrap();
    (scope, transaction)
}
fn revoke(connection: &Connection, streaming: bool) {
    let (scope, transaction) = owner(connection);
    let cursor = connection
        .execute(
            "SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3",
            vec![],
            streaming,
        )
        .unwrap();
    cursor.guard(transaction.clone()).unwrap();
    assert_eq!(cursor.fetchmany(1).unwrap(), [vec![json!(1)]]);
    scope.close();
    assert!(cursor.fetchmany(1).is_err());
    assert!(transaction.commit_ready().is_err());
    cursor.close().unwrap();
    connection.rollback().unwrap();
}

#[test]
fn sqlite_buffered_and_streaming_fetch_keep_the_original_owner() {
    for stream in [false, true] {
        revoke(&sqlite(), stream);
    }
}

#[test]
fn ended_physical_transactions_invalidate_buffered_and_streaming_results() {
    for stream in [false, true] {
        for commit in [false, true] {
            let connection = sqlite();
            let (_, transaction) = owner(&connection);
            let cursor = connection.execute("SELECT 1", vec![], stream).unwrap();
            cursor.guard(transaction.clone()).unwrap();
            if commit {
                connection.commit().unwrap();
            } else {
                connection.rollback().unwrap();
            }
            connection.begin().unwrap();
            assert!(transaction.check_bound().is_err());
            assert!(cursor.fetchmany(1).is_err());
            cursor.close().unwrap();
        }
    }
    let connection = sqlite();
    let cursor = connection.execute("SELECT 1", vec![], false).unwrap();
    connection.commit().unwrap();
    assert!(cursor.fetchmany(1).is_err());
}

#[test]
fn joined_parent_end_and_wrong_thread_poison_the_root() {
    for wrong_thread in [false, true] {
        let connection = sqlite();
        let (scope, root) = owner(&connection);
        let parent = scope.join(&root, true).unwrap();
        let child = scope.join(&parent, false).unwrap();
        let cursor = connection.execute("SELECT 1", vec![], true).unwrap();
        cursor.guard(child).unwrap();
        if wrong_thread {
            assert!(
                std::thread::spawn(move || cursor.fetchmany(1))
                    .join()
                    .unwrap()
                    .is_err()
            );
        } else {
            parent.close();
            assert!(cursor.fetchmany(1).is_err());
        }
        assert!(root.commit_ready().is_err());
        connection.rollback().unwrap();
    }
}

#[test]
fn cursor_rejects_rebinding_and_another_physical_connection() {
    for other in [false, true] {
        let connection = sqlite();
        let (_, transaction) = owner(&connection);
        let cursor = connection.execute("SELECT 1", vec![], false).unwrap();
        if other {
            let foreign = sqlite();
            let (_, wrong) = owner(&foreign);
            assert!(cursor.guard(wrong.clone()).is_err());
            assert!(wrong.commit_ready().is_err());
        } else {
            cursor.guard(transaction.clone()).unwrap();
            assert!(cursor.guard(transaction.clone()).is_err());
            assert!(transaction.commit_ready().is_err());
        }
        assert!(cursor.fetchmany(1).is_err());
    }
}

#[test]
fn fetch_failure_cannot_be_caught_and_committed() {
    let connection = sqlite();
    let (_, transaction) = owner(&connection);
    let cursor = connection.execute("SELECT 1", vec![], true).unwrap();
    cursor.guard(transaction.clone()).unwrap();
    assert!(cursor.fetchmany(1001).is_err());
    assert!(transaction.commit_ready().is_err());
    cursor.close().unwrap();
    connection.rollback().unwrap();
}

#[test]
#[ignore = "requires an isolated ASTERION_NATIVE_DATABASE_TEST_CONFIG"]
fn postgres_stream_and_buffered_results_reject_revoked_owners() {
    for stream in [false, true] {
        let connection = Connection::open(
            &std::env::var("ASTERION_NATIVE_DATABASE_TEST_CONFIG").expect("isolated PostgreSQL"),
        )
        .unwrap();
        revoke(&connection, stream);
    }
}
