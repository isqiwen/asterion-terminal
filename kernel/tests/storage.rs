use asterion_kernel::storage::{Scope, Statement, ownership, preflight};
use serde_json::json;

fn query(operation: &str, reads: &[u64], writes: &[u64]) -> Statement {
    Statement {
        operation: operation.into(),
        reads: reads.into(),
        writes: writes.into(),
        raw: false,
        undeclared: false,
    }
}

#[test]
fn statement_tree_cannot_expand_grants_or_hide_mutating_ctes() {
    let scope = Scope::new(1, vec![10], vec![20]);
    let transaction = scope.begin(true).unwrap();
    assert!(
        transaction
            .authorize(&query("select", &[10, 20], &[]))
            .is_ok()
    );
    assert!(
        transaction
            .authorize(&query("insert", &[10], &[10]))
            .is_ok()
    );
    assert!(
        transaction
            .authorize(&query("select", &[10, 20], &[20]))
            .is_err()
    );
    assert!(transaction.authorize(&query("select", &[30], &[])).is_err());
    assert!(transaction.authorize(&query("delete", &[], &[])).is_err());
    let mut raw = query("select", &[10], &[]);
    raw.raw = true;
    assert!(transaction.authorize(&raw).is_err());
    let reader = scope.begin(false).unwrap();
    assert!(reader.authorize(&query("insert", &[10], &[10])).is_err());
    assert!(reader.require_write().is_err());
}

#[test]
fn nested_scopes_keep_database_and_readonly_boundaries() {
    let a = Scope::new(1, vec![10], vec![]);
    let b = Scope::new(1, vec![20], vec![]);
    let foreign = Scope::new(2, vec![20], vec![]);
    let root = a.begin(true).unwrap();
    let joined = b.join(&root, true).unwrap();
    assert!(joined.authorize(&query("insert", &[20], &[20])).is_ok());
    assert!(joined.authorize(&query("insert", &[10], &[10])).is_err());
    assert!(foreign.join(&root, true).is_err());
    let borrowed = b.join(&root, false).unwrap();
    assert!(b.join(&borrowed, true).is_err());
    assert!(joined.commit_ready().is_err());
    root.close();
    assert!(joined.check().is_err());
    assert!(borrowed.check().is_err());
}

#[test]
fn closing_a_joined_writer_revokes_its_previously_staged_writes() {
    let a = Scope::new(1, vec![10], vec![]);
    let b = Scope::new(1, vec![20], vec![]);
    let root = a.begin(true).unwrap();
    let joined = b.join(&root, true).unwrap();
    joined.close();
    b.close();
    assert!(root.commit_ready().is_err());
}

#[test]
fn plugin_resource_lifetime_revokes_stored_grants_and_pending_commit() {
    use asterion_kernel::plugins::{Grants, Host, Manifest};
    let manifest: Manifest = serde_json::from_value(json!({
        "id":"fixture.owner", "layer":"L3", "api_version":1,
        "requires":[], "type_requires":[], "provides":[], "consumes":[],
        "resources":[], "observes":[]
    }))
    .unwrap();
    let mut host = Host::new(vec![manifest]).unwrap();
    host.begin(&Grants::new()).unwrap();
    host.next_activation().unwrap();
    let grant = Scope::new(1, vec![10], vec![]);
    let owned = grant
        .guarded(host.context_handle("fixture.owner").unwrap().into())
        .unwrap();
    let root = grant.begin(true).unwrap();
    let joined = owned.join(&root, true).unwrap();
    joined.authorize(&query("insert", &[10], &[10])).unwrap();
    joined.close();
    host.close();
    assert!(owned.initialize(&[10]).is_err());
    assert!(owned.begin(false).is_err());
    assert!(root.commit_ready().is_err());
    // Product ownership is distinct: closing a plugin does not revoke a
    // different resource grant or erase its data.
    assert!(grant.begin(false).is_ok());
}

#[test]
fn task_resource_lifetime_revokes_pending_storage_use() {
    use asterion_kernel::{plugins::Resource, tasks::ExecutionScope};
    let resource = Resource {
        id: "fixture.store".into(),
        contract: "storage".into(),
    };
    let mut task = ExecutionScope::new(vec![resource.clone()], vec![resource.clone()]).unwrap();
    let storage = Scope::new(1, vec![10], vec![])
        .guarded(task.resource(&resource).unwrap().into())
        .unwrap();
    let pending = storage.begin(true).unwrap();
    task.close();
    assert!(storage.check().is_err());
    assert!(pending.commit_ready().is_err());
}

#[test]
fn commit_admission_seals_the_transaction_and_explicit_close_is_idempotent() {
    let scope = Scope::new(1, vec![10], vec![]);
    let transaction = scope.begin(true).unwrap();
    transaction.commit_ready().unwrap();
    assert!(transaction.check().is_err());
    assert!(transaction.commit_ready().is_err());
    transaction.close();
    transaction.close();
}

#[test]
fn failed_participant_poisons_root_and_other_participants() {
    let a = Scope::new(1, vec![10], vec![]);
    let b = Scope::new(1, vec![20], vec![]);
    let root = a.begin(true).unwrap();
    let participant = b.join(&root, true).unwrap();
    participant.abort();
    participant.close();
    assert!(root.check().is_err());
    assert!(root.commit_ready().is_err());
    assert!(b.join(&root, true).is_err());
}

#[test]
fn closed_participant_rejection_does_not_poison_root() {
    let a = Scope::new(1, vec![10], vec![]);
    let b = Scope::new(1, vec![20], vec![]);
    let root = a.begin(true).unwrap();
    let participant = b.join(&root, true).unwrap();
    participant.close();
    assert!(participant.check().is_err());
    participant.abort();
    assert!(root.check().is_ok());
    root.commit_ready().unwrap();
}

#[test]
fn live_child_of_an_ended_parent_still_poisons_root() {
    let a = Scope::new(1, vec![10], vec![]);
    let b = Scope::new(1, vec![20], vec![]);
    let root = a.begin(true).unwrap();
    let parent = b.join(&root, true).unwrap();
    let child = b.join(&parent, false).unwrap();
    parent.close();
    child.abort();
    assert!(root.commit_ready().is_err());
}

#[test]
fn transaction_handles_are_thread_confined() {
    let scope = Scope::new(1, vec![10], vec![]);
    let transaction = scope.begin(true).unwrap();
    let other = transaction.clone();
    assert!(
        std::thread::spawn(move || other.check().is_err())
            .join()
            .unwrap()
    );
    assert!(transaction.check().is_ok());
}

#[test]
fn schema_preflight_and_ownership_use_physical_identity() {
    let schema = json!({"name":"rows","columns":[{"name":"id","kind":"INTEGER","nullable":false}],"primary":["id"],"unique":[],"foreign":[]});
    let expected = vec![serde_json::from_value(schema.clone()).unwrap()];
    assert!(preflight(&expected, &[]).is_ok());
    assert!(preflight(&expected, &expected).is_ok());
    let mut changed = schema;
    changed["columns"][0]["nullable"] = json!(true);
    assert!(preflight(&expected, &[serde_json::from_value(changed).unwrap()]).is_err());
    assert!(ownership(&["rows".into()], &[vec!["rows".into()]]).is_err());
    assert!(ownership(&[], &[vec!["rows".into()], vec!["rows".into()]]).is_err());
}
