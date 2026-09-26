use asterion_kernel::{
    invoke,
    plugins::{self, Capability, Grants, Host, HostState, Layer, Manifest, Resource},
};
use serde_json::{Value, json};

fn plugin(id: &str, layer: Layer) -> Manifest {
    Manifest {
        id: id.into(),
        layer,
        api_version: 1,
        type_requires: vec![],
        requires: vec![],
        provides: vec![],
        consumes: vec![],
        resources: vec![],
        observes: vec![],
    }
}

fn capability() -> Capability {
    Capability {
        id: "fixture.read".into(),
        provider: "fixture.base".into(),
        contract: "fixture.ReadPort".into(),
    }
}

fn pair() -> Vec<Manifest> {
    let mut base = plugin("fixture.base", Layer::L2);
    base.provides.push(capability());
    let mut child = plugin("fixture.child", Layer::L3);
    child.type_requires.push(base.id.clone());
    child.requires.push(base.id.clone());
    child.consumes.push(capability());
    vec![child, base]
}

#[test]
fn zero_feature_plugins_boot_and_close_without_an_auth_provider() {
    let mut host = Host::new(vec![]).unwrap();
    host.begin(&Grants::new()).unwrap();
    assert_eq!(host.next_activation().unwrap(), None);
    host.finish().unwrap();
    assert_eq!(host.state(), HostState::Active);
    assert!(host.close().is_empty());
    assert_eq!(host.state(), HostState::Closed);
    assert!(host.close().is_empty());
    assert!(host.begin(&Grants::new()).is_err());
}

#[test]
fn source_and_activation_graphs_allow_domain_port_inversion_without_import_cycle() {
    let mut domain = plugin("fixture.domain", Layer::L2);
    let mut adapter = plugin("fixture.adapter", Layer::L3);
    adapter.type_requires.push(domain.id.clone());
    let port = Capability {
        id: "fixture.broker".into(),
        provider: adapter.id.clone(),
        contract: "fixture.domain.BrokerPort".into(),
    };
    adapter.provides.push(port.clone());
    domain.requires.push(adapter.id.clone());
    domain.consumes.push(port);
    let plan = plugins::validate(&[domain, adapter]).unwrap();
    assert_eq!(plan.order, ["fixture.adapter", "fixture.domain"]);
    assert_eq!(plan.type_order, ["fixture.domain", "fixture.adapter"]);
}

#[test]
fn duplicate_missing_cyclic_upward_and_ui_domain_dependencies_are_rejected() {
    let base = plugin("fixture.base", Layer::L2);
    assert!(
        plugins::validate(&[base.clone(), base.clone()])
            .unwrap_err()
            .contains("Duplicate plugin")
    );
    let mut altered = base.clone();
    altered.requires.push("fixture.absent".into());
    assert!(
        plugins::validate(&[altered])
            .unwrap_err()
            .contains("Missing required")
    );
    for source in [false, true] {
        let mut altered = base.clone();
        if source {
            altered.type_requires.push(altered.id.clone());
        } else {
            altered.requires.push(altered.id.clone());
        }
        assert!(
            plugins::validate(&[altered])
                .unwrap_err()
                .contains("dependency cycle")
        );
    }
    let mut altered = base.clone();
    altered.type_requires.push("fixture.app".into());
    assert!(
        plugins::validate(&[altered, plugin("fixture.app", Layer::L3)])
            .unwrap_err()
            .contains("upward")
    );
    let mut ui = plugin("fixture.ui", Layer::L4);
    ui.type_requires.push(base.id.clone());
    assert!(
        plugins::validate(&[base, ui])
            .unwrap_err()
            .contains("application interface")
    );
}

#[test]
fn kernel_namespaces_and_layers_cannot_be_registered_or_replaced() {
    for name in [
        "kernel.authority",
        "asterion.kernel.tasks",
        "foundation.id",
        "core.storage",
    ] {
        assert!(
            plugins::validate(&[plugin(name, Layer::L3)])
                .unwrap_err()
                .contains("Reserved")
        );
    }
    let mut base = plugin("fixture.base", Layer::L2);
    base.provides.push(Capability {
        id: "kernel.authority".into(),
        provider: base.id.clone(),
        contract: "Authority".into(),
    });
    assert!(plugins::validate(&[base]).unwrap_err().contains("Reserved"));
    let mut value = serde_json::to_value(plugin("fixture.base", Layer::L2)).unwrap();
    value["layer"] = json!("L1");
    assert!(invoke("plugins.validate", json!({"plugins":[value]})).is_err());
}

#[test]
fn capability_owner_type_dependency_and_exact_provision_are_checked() {
    for invalid in ["owner", "duplicate", "dependency", "missing", "type"] {
        let mut manifests = pair();
        match invalid {
            "owner" => manifests[1].provides[0].provider = "fixture.child".into(),
            "duplicate" => manifests[1].provides.push(capability()),
            "dependency" => manifests[0].requires.clear(),
            "missing" => manifests[1].provides.clear(),
            "type" => manifests[0].consumes[0].contract = "fixture.OtherPort".into(),
            _ => unreachable!(),
        }
        assert!(plugins::validate(&manifests).is_err(), "{invalid}");
    }
}

#[test]
fn full_resource_grants_are_validated_before_beginning() {
    let resource = Resource {
        id: "fixture.config".into(),
        contract: "fixture.Config".into(),
    };
    let mut base = plugin("fixture.base", Layer::L3);
    base.resources.push(resource.clone());
    for invalid in ["missing", "extra", "type", "duplicate", "unknown"] {
        let mut manifest = base.clone();
        let mut grants = Grants::from([(base.id.clone(), vec![resource.clone()])]);
        match invalid {
            "missing" => grants.clear(),
            "extra" => grants.get_mut(&base.id).unwrap().push(Resource {
                id: "fixture.private".into(),
                contract: "fixture.Config".into(),
            }),
            "type" => grants.get_mut(&base.id).unwrap()[0].contract = "fixture.Other".into(),
            "duplicate" => manifest.resources.push(resource.clone()),
            "unknown" => {
                grants.insert("fixture.unknown".into(), vec![]);
            }
            _ => unreachable!(),
        }
        let mut host = Host::new(vec![manifest]).unwrap();
        assert!(host.begin(&grants).is_err(), "{invalid}");
        assert_eq!(host.state(), HostState::Closed);
        assert!(host.next_activation().is_err());
    }
}

#[test]
fn lifecycle_enforces_order_exports_reverse_close_and_stale_handle_invalidation() {
    let mut manifests = pair();
    let resource = Resource {
        id: "fixture.config".into(),
        contract: "fixture.Config".into(),
    };
    manifests[0].resources.push(resource.clone());
    let mut host = Host::new(manifests).unwrap();
    host.begin(&Grants::from([(
        "fixture.child".into(),
        vec![resource.clone()],
    )]))
    .unwrap();
    assert_eq!(
        host.next_activation().unwrap().as_deref(),
        Some("fixture.base")
    );
    assert!(host.finish().is_err());
    assert!(host.activated("fixture.child", &[]).is_err());
    host.activated("fixture.base", &[capability()]).unwrap();
    assert_eq!(
        host.next_activation().unwrap().as_deref(),
        Some("fixture.child")
    );
    let cap_handle = host
        .capability_handle("fixture.child", &capability())
        .unwrap();
    let resource_handle = host.resource_handle("fixture.child", &resource).unwrap();
    let private = Resource {
        id: "fixture.private".into(),
        contract: resource.contract.clone(),
    };
    assert!(host.resource_handle("fixture.child", &private).is_err());
    host.activated("fixture.child", &[]).unwrap();
    host.finish().unwrap();
    assert!(cap_handle.check().is_ok());
    assert_eq!(host.close(), ["fixture.child", "fixture.base"]);
    assert!(cap_handle.check().is_err());
    assert!(resource_handle.check().is_err());
    assert!(host.close().is_empty());
}

#[test]
fn failed_export_validation_invalidates_handles_and_includes_failed_plugins_cleanup() {
    let mut host = Host::new(pair()).unwrap();
    host.begin(&Grants::new()).unwrap();
    host.next_activation().unwrap();
    assert!(host.activated("fixture.base", &[]).is_err());
    assert!(host.finish().is_err());
    assert_eq!(host.close(), ["fixture.base"]);
}

fn grant(path: &str, methods: &[&str]) -> Value {
    json!({"path":path,"methods":methods,"descendants":false})
}

fn auth_request(token: &str) -> Value {
    json!({"secret":"first-secret","policies":{"reader":[grant("/records/:id", &["GET"])]},
        "worker_grants":[grant("/jobs/claim", &["POST"])], "credential":token,
        "method":"GET","path":"/records/one","session":"account-one","now":100})
}

#[test]
fn scope_is_bound_to_secret_session_policy_and_expiry() {
    let token = invoke("auth.issue", json!({"secret":"first-secret","policies":{"reader":[grant("/records/:id", &["GET"])]},"scope":"reader","session":"account-one","now":100})).unwrap();
    assert_eq!(token["expires"], 400);
    let request = auth_request(token["token"].as_str().unwrap());
    assert_eq!(invoke("auth.authorize", request.clone()).unwrap(), "reader");
    for (key, value) in [
        ("method", json!("POST")),
        ("path", json!("/records/one/remove")),
        ("path", json!("/records/../account")),
        ("path", json!("/records/%2e%2e")),
        ("session", json!("account-two")),
        ("secret", json!("other-secret")),
        ("now", json!(400)),
    ] {
        let mut altered = request.clone();
        altered[key] = value;
        assert!(invoke("auth.authorize", altered).is_err(), "{key}");
    }
}

#[test]
fn worker_cannot_escalate_and_unknown_operations_fail_closed() {
    let worker = invoke("auth.worker_token", json!({"secret":"first-secret"})).unwrap();
    let mut request = auth_request(worker.as_str().unwrap());
    assert!(
        invoke("auth.authorize", request.clone())
            .unwrap_err()
            .contains("任务执行器")
    );
    request["method"] = json!("POST");
    request["path"] = json!("/jobs/claim");
    assert_eq!(invoke("auth.authorize", request).unwrap(), "worker");
    assert!(invoke("auth.install_provider", json!({})).is_err());
    assert!(
        invoke(
            "auth.issue",
            json!({"secret":"s","policies":{},"scope":"absent","session":"a","now":0})
        )
        .is_err()
    );
}

#[test]
fn invalid_paths_do_not_match_even_descendant_grants() {
    for path in [
        "/records/",
        "/records//one",
        "/records/./one",
        "/records/../one",
        "/records/%2F",
        "/records/a?x=1",
        "/records/日本",
        "records/one",
        "/recordsx/one",
    ] {
        assert_eq!(invoke("auth.grant_allows", json!({"grant":{"path":"/records","methods":["GET"],"descendants":true},"method":"GET","path":path})).unwrap(), false, "{path}");
    }
    assert_eq!(invoke("auth.grant_allows", json!({"grant":{"path":"/records","methods":["GET"],"descendants":true},"method":"GET","path":"/records/one/two"})).unwrap(), true);
}

#[test]
fn tampered_malformed_and_unsigned_tokens_are_rejected() {
    for token in [
        "",
        "scope.x.x",
        "scope.e30.0000000000000000000000000000000000000000000000000000000000000000",
        "root",
        "scope.a.b.extra",
    ] {
        assert!(
            invoke("auth.authorize", auth_request(token)).is_err(),
            "{token}"
        );
    }
}

#[test]
fn binding_entrypoint_rejects_unknown_fields_and_returns_deterministic_orders() {
    let request = json!({"plugins":pair()});
    let result = invoke("plugins.validate", request.clone()).unwrap();
    assert_eq!(result["order"], json!(["fixture.base", "fixture.child"]));
    let mut invalid = request;
    invalid["fallback"] = json!(true);
    assert!(invoke("plugins.validate", invalid).is_err());
}
