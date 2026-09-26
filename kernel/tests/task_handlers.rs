use asterion_kernel::{
    authority::Grant,
    tasks::handlers::{Declaration, Registry},
};
use std::sync::Arc;

fn grant(path: &str) -> Grant {
    Grant {
        path: path.into(),
        methods: vec!["POST".into()],
        descendants: false,
    }
}

fn declaration(kind: &str, requests: Vec<Grant>) -> Declaration {
    Declaration::new(kind.into(), "/publish-result".into(), requests).unwrap()
}

#[test]
fn namespaced_declarations_and_publication_endpoints_are_strict() {
    for kind in ["research.report", "fixture_v2.item1.run"] {
        assert!(Declaration::new(kind.into(), "/publish-result2".into(), vec![]).is_ok());
    }
    for kind in [
        "", "fixture", ".run", "fixture.", "a..b", "A.b", "a.1b", "a.b-c", "a.中文", "a.b\n",
    ] {
        assert!(
            Declaration::new(kind.into(), "/publish".into(), vec![]).is_err(),
            "{kind:?}"
        );
    }
    for suffix in [
        "",
        "/",
        "https://other.test",
        "//other.test",
        "/../fail",
        "/x?q=1",
        "/a/b",
        "/A",
        "/a_b",
        "/x\n",
    ] {
        assert!(
            Declaration::new("fixture.run".into(), suffix.into(), vec![]).is_err(),
            "{suffix:?}"
        );
    }
}

#[test]
fn request_declarations_are_exact_valid_job_relative_post_routes() {
    for path in [
        "/jobs/:id/progress",
        "/jobs/:id/observations/:id",
        "/jobs/:id/checkpoints/:id/content",
    ] {
        assert!(
            Declaration::new("fixture.run".into(), "/publish".into(), vec![grant(path)]).is_ok()
        );
    }
    for path in [
        "/",
        "/jobs/claim",
        "/other/:id/run",
        "/jobs/:id/",
        "/jobs/:id/:id",
        "/jobs/:id//progress",
        "/jobs/:id/../run",
        "/jobs/:id/run/..",
        "/jobs/:id/run?mode=1",
        "/jobs/:id/run/%2e%2e",
        "/jobs/:id/run/:other",
        "/jobs/:id/run/",
    ] {
        assert!(
            Declaration::new("fixture.run".into(), "/publish".into(), vec![grant(path)]).is_err(),
            "{path}"
        );
    }
    for methods in [
        vec![],
        vec!["GET".into()],
        vec!["POST".into(), "GET".into()],
        vec!["post".into()],
    ] {
        let mut request = grant("/jobs/:id/progress");
        request.methods = methods;
        assert!(Declaration::new("fixture.run".into(), "/publish".into(), vec![request]).is_err());
    }
    let mut request = grant("/jobs/:id/progress");
    request.descendants = true;
    assert!(Declaration::new("fixture.run".into(), "/publish".into(), vec![request]).is_err());
}

#[test]
fn host_control_operations_cannot_be_granted_by_handlers() {
    for operation in ["claim", "heartbeat", "fail", "cancel"] {
        assert!(Declaration::new("fixture.run".into(), format!("/{operation}"), vec![]).is_err());
        assert!(
            Declaration::new(
                "fixture.run".into(),
                "/publish".into(),
                vec![grant(&format!("/jobs/:id/{operation}"))]
            )
            .is_err()
        );
    }
}

#[test]
fn duplicate_registration_is_atomic_and_lookup_uses_original_callback_indices() {
    let mut registry = Registry::default();
    assert_eq!(
        registry
            .register(declaration("fixture.first", vec![]))
            .unwrap(),
        0
    );
    assert_eq!(
        registry
            .register(declaration("fixture.second", vec![]))
            .unwrap(),
        1
    );
    assert!(
        registry
            .register(declaration("fixture.first", vec![]))
            .is_err()
    );
    assert_eq!(
        registry
            .register(declaration("fixture.third", vec![]))
            .unwrap(),
        2
    );
    assert_eq!(registry.get("fixture.first").unwrap(), 0);
    assert_eq!(registry.get("fixture.third").unwrap(), 2);
    assert_eq!(
        registry.get("missing").unwrap_err(),
        "Unsupported job kind: missing"
    );
}

#[test]
fn execution_requests_use_only_current_declaration_and_revoke_retained_scope() {
    let declared = declaration("fixture.first", vec![grant("/jobs/:id/checkpoints/:id")]);
    let scope = Arc::new(declared.requests());
    scope.authorize("/checkpoints/1").unwrap();
    scope.authorize("/checkpoints/batch-2").unwrap();
    for suffix in [
        "/other",
        "/heartbeat",
        "/fail",
        "/cancel",
        "/publish-result",
        "https://other.test",
        "//other.test",
        "/checkpoints/..",
        "/checkpoints/1/extra",
        "/checkpoints/%2f",
        "/checkpoints/1?next=1",
        "/checkpoints/1#fragment",
        "/checkpoints//",
    ] {
        assert!(scope.authorize(suffix).is_err(), "{suffix}");
    }
    let other = declaration("fixture.second", vec![grant("/jobs/:id/other")]);
    other.requests().authorize("/other").unwrap();
    assert!(scope.authorize("/other").is_err());
    let retained = Arc::clone(&scope);
    std::thread::spawn(move || retained.close()).join().unwrap();
    assert_eq!(
        scope.authorize("/checkpoints/1").unwrap_err(),
        "Execution transport is closed"
    );
    scope.close();
    declared.requests().authorize("/checkpoints/1").unwrap();
}

#[test]
fn server_worker_grants_contain_only_control_and_validated_contributions() {
    let mut registry = Registry::default();
    registry
        .register(declaration(
            "fixture.first",
            vec![grant("/jobs/:id/progress")],
        ))
        .unwrap();
    registry
        .register(declaration(
            "fixture.second",
            vec![grant("/jobs/:id/progress")],
        ))
        .unwrap();
    let grants = registry.worker_grants();
    assert_eq!(grants.len(), 5);
    for path in [
        "/jobs/claim",
        "/jobs/current/heartbeat",
        "/jobs/current/fail",
        "/jobs/current/publish-result",
        "/jobs/current/progress",
    ] {
        assert!(
            grants.iter().any(|grant| grant.allows("POST", path)),
            "{path}"
        );
    }
    for (method, path) in [
        ("GET", "/jobs/current/progress"),
        ("POST", "/jobs/current/cancel"),
        ("POST", "/jobs/current/progress/subpath"),
        ("POST", "/other/current/progress"),
    ] {
        assert!(
            !grants.iter().any(|grant| grant.allows(method, path)),
            "{path}"
        );
    }
}
