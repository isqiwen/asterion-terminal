use asterion_kernel::communication::{self as communication, Error};
use serde_json::{Value, json};

#[test]
fn child_context_preserves_causality_and_never_extends_parent() {
    let parent = communication::context_at(None, 2.0, 1000).unwrap();
    let child = communication::context_at(Some(&parent), 30.0, 1100).unwrap();
    assert_eq!(child.correlation_id, parent.correlation_id);
    assert_eq!(child.causation_id, json!(parent.request_id));
    assert_ne!(child.request_id, parent.request_id);
    assert_eq!(child.deadline_ms, 3000);
    assert_eq!(
        communication::context_at(Some(&parent), 1.0, 3000).unwrap_err(),
        Error::Deadline
    );
    let shorter = communication::context_at(Some(&parent), 0.1, 1100).unwrap();
    assert_eq!(shorter.deadline_ms, 1200);
}

#[test]
fn invalid_time_budgets_and_contexts_are_rejected() {
    for seconds in [f64::NAN, f64::INFINITY, -1.0, 0.0001, 1e30] {
        assert!(communication::context_at(None, seconds, 1000).is_err());
    }
    assert_eq!(
        communication::context_at(None, 0.0, 1000).unwrap_err(),
        Error::Deadline
    );
    let mut parent = communication::context_at(None, 1.0, 1000).unwrap();
    parent.version = 2;
    assert!(communication::context_at(Some(&parent), 1.0, 1100).is_err());
    assert!(communication::context_at(None, 1.0, 9_007_199_254_740_990).is_err());
}

#[test]
fn late_mismatched_and_remote_error_results_are_never_accepted() {
    let trace = communication::context_at(None, 1.0, 1000).unwrap();
    let response = communication::prepare_reply(&trace, json!({"ok":true}), Value::Null).unwrap();
    assert_eq!(
        communication::reply_result_at(&trace, response.clone(), 1999).unwrap(),
        json!({"ok":true})
    );
    assert_eq!(
        communication::reply_result_at(&trace, response.clone(), 2000).unwrap_err(),
        Error::Deadline
    );
    let other = communication::context_at(None, 1.0, 1000).unwrap();
    assert_eq!(
        communication::reply_result_at(&other, response, 1500).unwrap_err(),
        Error::Mismatch
    );
    let response = communication::prepare_reply(
        &trace,
        Value::Null,
        json!({"code":"FAULT","message":"private exception data"}),
    )
    .unwrap();
    let error = communication::reply_result_at(&trace, response, 1500).unwrap_err();
    assert_eq!(error, Error::Remote);
    assert!(!error.to_string().contains("private"));
}

#[test]
fn context_matching_preserves_schema_integer_semantics() {
    let trace = communication::context_at(None, 1.0, 1000).unwrap();
    let mut response = communication::prepare_reply(&trace, json!(true), Value::Null).unwrap();
    response["context"]["version"] = json!(1.0);
    response["context"]["deadline_ms"] = json!(2000.0);
    assert_eq!(
        communication::reply_result_at(&trace, response, 1999).unwrap(),
        json!(true)
    );
}

#[test]
fn ingress_rejects_oversize_duplicate_and_expired_context_without_fallback() {
    assert!(communication::ingress(Some(&vec![b' '; 2049]), 60.0).is_err());
    assert!(communication::ingress(Some(br#"{"version":1,"version":1}"#), 60.0).is_err());
    let trace = communication::context_at(None, 1.0, 1000).unwrap();
    assert_eq!(
        communication::ingress(Some(&serde_json::to_vec(&trace).unwrap()), 60.0).unwrap_err(),
        Error::Deadline
    );
    communication::ingress(None, 60.0).unwrap();
}
