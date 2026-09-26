use asterion_kernel::{
    invoke,
    plugins::Resource,
    tasks::{self, ExecutionScope, RecordRequest, ReuseRequest, TransitionRequest},
};
use serde_json::{Value, json};

fn queued() -> Value {
    json!({"id":"job", "state":"QUEUED", "attempt":0, "token":null, "lease_until":null, "result":null})
}
fn running() -> Value {
    json!({"id":"job", "state":"RUNNING", "attempt":1, "token":"first", "lease_until":120.0, "result":null})
}
fn plan(row: Value, now: f64, action: Value) -> Result<Value, String> {
    let request = TransitionRequest {
        row: serde_json::from_value(row).unwrap(),
        action: serde_json::from_value(action).unwrap(),
        now,
    };
    tasks::transition(request).map(|plan| serde_json::to_value(plan).unwrap())
}
fn claim() -> Value {
    json!({"type":"claim","worker_id":"worker","token":"second","lease_seconds":60.0})
}

#[test]
fn expired_boundary_is_reclaimable_but_never_authorized() {
    assert!(plan(running(), 119.999, claim()).unwrap().is_null());
    let update = plan(running(), 120.0, claim()).unwrap();
    assert_eq!(update["values"]["attempt"], 2);
    assert_eq!(update["values"]["lease_until"], 180.0);
    assert_eq!(
        update["condition"]["clauses"][3],
        json!({"op":"eq","column":"token","value":"first"})
    );
    assert_eq!(
        update["condition"]["clauses"][4],
        json!({"op":"eq","column":"lease_until","value":120.0})
    );
    assert!(
        tasks::require_lease(
            Some(&serde_json::from_value(running()).unwrap()),
            "first",
            120.0
        )
        .is_err()
    );
    assert!(
        plan(
            running(),
            100.0,
            json!({"type":"complete","token":"second","result":{}})
        )
        .is_err()
    );
}

#[test]
fn terminal_states_never_resume_or_accept_worker_writes() {
    for state in ["SUCCEEDED", "FAILED", "CANCELLED"] {
        let mut row = running();
        row["state"] = json!(state);
        assert!(plan(row.clone(), 100.0, claim()).unwrap().is_null());
        assert!(
            plan(row.clone(), 100.0, json!({"type":"cancel"}))
                .unwrap()
                .is_null()
        );
        for action in [
            json!({"type":"complete","token":"first","result":{}}),
            json!({"type":"progress","token":"first","result":{}}),
            json!({"type":"heartbeat","token":"first","lease_seconds":60.0}),
            json!({"type":"fail","token":"first","error":"failure"}),
        ] {
            assert!(plan(row.clone(), 100.0, action).is_err());
        }
    }
}

#[test]
fn progress_events_are_deduplicated_and_heartbeat_cannot_shorten_lease() {
    let update = plan(
        running(),
        50.0,
        json!({"type":"heartbeat","token":"first","lease_seconds":60.0}),
    )
    .unwrap();
    assert_eq!(update["values"]["lease_until"], 120.0);
    assert_eq!(update["event"], false);
    let mut row = running();
    row["result"] = json!({"count":3});
    assert!(
        plan(
            row.clone(),
            100.0,
            json!({"type":"progress","token":"first","result":{"count":3}})
        )
        .unwrap()
        .is_null()
    );
    assert_eq!(
        plan(
            row,
            100.0,
            json!({"type":"progress","token":"first","result":{"count":4}})
        )
        .unwrap()["event"],
        true
    );
    let failure = plan(
        running(),
        100.0,
        json!({"type":"fail","token":"first","error":"失".repeat(2001)}),
    )
    .unwrap();
    assert_eq!(
        failure["values"]["error"].as_str().unwrap().chars().count(),
        2000
    );
}

#[test]
fn all_new_attempts_are_fenced_and_invalid_inputs_fail_before_mutation() {
    assert_eq!(
        plan(queued(), 100.0, claim()).unwrap()["values"]["attempt"],
        1
    );
    let mut row = running();
    row["attempt"] = json!(i64::MAX);
    assert!(plan(row, 120.0, claim()).is_err());
    assert!(plan(queued(), -1.0, claim()).is_err());
    assert!(
        plan(
            queued(),
            100.0,
            json!({"type":"claim","worker_id":"","token":"second","lease_seconds":60})
        )
        .is_err()
    );
    assert!(
        plan(
            running(),
            120.0,
            json!({"type":"claim","worker_id":"worker","token":"first","lease_seconds":60})
        )
        .is_err()
    );
    for duration in [0.0, -1.0] {
        assert!(invoke("tasks.duration", json!({"lease_seconds":duration})).is_err());
    }
    assert!(invoke("tasks.contract", json!({"unknown":true})).is_err());
}

#[test]
fn task_idempotence_is_exact() {
    let mut request = ReuseRequest {
        original_kind: "feature.read".into(),
        original_payload: json!({"x":true}).as_object().unwrap().clone(),
        kind: "feature.read".into(),
        payload: json!({"x":1}).as_object().unwrap().clone(),
    };
    assert!(tasks::check_reuse(&request).is_err());
    request.original_payload = request.payload.clone();
    assert!(tasks::check_reuse(&request).is_ok());
    assert!(
        tasks::record(RecordRequest {
            id: "job".into(),
            command_id: "command".into(),
            kind: "feature.read".into(),
            payload: Default::default(),
            now: 100.0,
        })
        .is_ok()
    );
}

#[test]
fn execution_handles_reject_wrong_grants_and_invalidate_on_close_or_drop() {
    let resource = Resource {
        id: "feature.resource".into(),
        contract: "FeaturePort".into(),
    };
    assert!(
        ExecutionScope::new(
            vec![resource.clone(), resource.clone()],
            vec![resource.clone()]
        )
        .is_err()
    );
    assert!(ExecutionScope::new(vec![resource.clone()], vec![]).is_err());
    let mut scope = ExecutionScope::new(vec![resource.clone()], vec![resource.clone()]).unwrap();
    let handle = scope.resource(&resource).unwrap();
    let other = Resource {
        id: resource.id.clone(),
        contract: "OtherPort".into(),
    };
    assert!(scope.resource(&other).is_err());
    handle.check().unwrap();
    scope.close();
    assert!(handle.check().is_err());
    let scope = ExecutionScope::new(vec![resource.clone()], vec![resource.clone()]).unwrap();
    let handle = scope.resource(&resource).unwrap();
    drop(scope);
    assert!(handle.check().is_err());
}

#[test]
fn restore_isolation_clears_execution_credentials_without_rewriting_terminal_history() {
    let update = plan(
        running(),
        100.0,
        json!({"type":"isolate","reason":"restore isolation"}),
    )
    .unwrap();
    assert_eq!(
        update["values"],
        json!({"state":"CANCELLED","token":null,"worker_id":null,
        "lease_until":null,"error":"restore isolation"})
    );
    let mut row = running();
    row["state"] = json!("SUCCEEDED");
    row["result"] = json!({"immutable":true});
    assert!(
        plan(
            row,
            100.0,
            json!({"type":"isolate","reason":"restore isolation"})
        )
        .unwrap()
        .is_null()
    );
}
