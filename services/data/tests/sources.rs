//! Connections, configurations and credential snapshots of data sources, on a
//! SQLite store with source declarations supplied by the test.
use asterion_data::configuration::{self, ConfigurationUpdate, SourceError};
use asterion_data::connections::{self, ConnectionUpdate, NewConnection};
use asterion_data::credentials::Credentials;
use asterion_data_store::provider::{ConfigurationField, ConfigurationSpec, ProviderManifest};
use asterion_store::{Store, Transaction};
use serde_json::{Map, Value, json};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

const SECRET: &str = "synthetic-provider-token-never-log";
const MASTER: &str = "test-runtime-token-at-least-24-characters";

fn field(id: &str, kind: &str) -> ConfigurationField {
    ConfigurationField {
        id: id.into(),
        label: id.to_uppercase(),
        kind: kind.into(),
        secret: false,
        required: false,
        default: Value::Null,
        description: String::new(),
        placeholder: String::new(),
        min_length: None,
        max_length: None,
        minimum: None,
        maximum: None,
    }
}

fn manifest(id: &str, fields: Vec<ConfigurationField>) -> ProviderManifest {
    ProviderManifest {
        id: id.into(),
        name: format!("{id} source"),
        version: "1".into(),
        api_version: 2,
        capabilities: vec![],
        configuration: ConfigurationSpec {
            schema_version: 1,
            fields,
        },
        description: String::new(),
        demo: false,
    }
}

/// A secret-token source and an ordinary-integer source.
fn sources() -> Vec<ProviderManifest> {
    let mut token = field("token", "string");
    token.secret = true;
    token.required = true;
    token.min_length = Some(1);
    token.max_length = Some(256);
    let mut seed = field("seed", "integer");
    seed.default = json!(7);
    seed.minimum = Some(0);
    seed.maximum = Some(10000);
    vec![
        manifest("vendor", vec![token]),
        manifest("synthetic", vec![seed]),
    ]
}

struct Fixture {
    _directory: tempfile::TempDir,
    root: PathBuf,
    store: Store,
    credentials: Credentials,
    sources: Vec<ProviderManifest>,
}

fn fixture() -> Fixture {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("data");
    let store = Store::open(&format!(
        "sqlite:///{}",
        directory.path().join("sources.db").display()
    ))
    .unwrap();
    store
        .transaction(true, |tx: &Transaction| {
            for ddl in [
                "CREATE TABLE data_connections (id VARCHAR NOT NULL PRIMARY KEY, provider VARCHAR NOT NULL, name VARCHAR NOT NULL)",
                "CREATE TABLE data_connection_settings (id VARCHAR NOT NULL PRIMARY KEY, name VARCHAR NOT NULL, state VARCHAR NOT NULL, revision INTEGER NOT NULL)",
                "CREATE TABLE data_provider_configurations (provider VARCHAR NOT NULL PRIMARY KEY, revision INTEGER NOT NULL, schema_version INTEGER NOT NULL, snapshot_ref VARCHAR NOT NULL)",
                "CREATE TABLE data_connection_verifications (provider VARCHAR NOT NULL PRIMARY KEY, revision INTEGER NOT NULL, started_at FLOAT NOT NULL, checked_at FLOAT NOT NULL, status VARCHAR NOT NULL)",
            ] {
                tx.execute(ddl, vec![])?;
            }
            Ok::<_, asterion_store::StoreError>(())
        })
        .unwrap();
    Fixture {
        credentials: Credentials::new(MASTER, &root),
        root,
        _directory: directory,
        store,
        sources: sources(),
    }
}

fn update(revision: i64, values: Value, secrets: Value) -> ConfigurationUpdate {
    serde_json::from_value(
        json!({"expected_revision": revision, "values": values, "secrets": secrets}),
    )
    .unwrap()
}

impl Fixture {
    fn spec(&self, owner: &str) -> ConfigurationSpec {
        self.store
            .transaction(false, |tx| connections::spec(tx, &self.sources, owner))
            .unwrap()
    }
    fn apply(
        &self,
        owner: &str,
        update: ConfigurationUpdate,
    ) -> Result<configuration::ConfigurationState, SourceError> {
        let spec = self.spec(owner);
        self.store.transaction(true, |tx| {
            configuration::apply(tx, &self.credentials, owner, &spec, &update)
        })
    }
    fn state(&self, owner: &str) -> configuration::ConfigurationState {
        let spec = self.spec(owner);
        self.store
            .transaction(false, |tx| {
                configuration::state(tx, &self.credentials, owner, &spec)
            })
            .unwrap()
    }
    fn snapshots(&self) -> Vec<PathBuf> {
        let folder = self.root.join(".credentials/configurations");
        let mut paths: Vec<PathBuf> = std::fs::read_dir(folder)
            .map(|entries| entries.map(|entry| entry.unwrap().path()).collect())
            .unwrap_or_default();
        paths.sort();
        paths
    }
    fn fixed(&self, owner: &str, provider: &str) -> Value {
        self.store
            .transaction(false, |tx| {
                connections::fix_for_task(tx, &self.credentials, &self.sources, owner, provider)
            })
            .unwrap()["configuration"]
            .clone()
    }
}

fn refused<T: std::fmt::Debug>(result: Result<T, SourceError>) -> String {
    match result {
        Err(SourceError::Refused(message)) => message,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

fn conflict<T: std::fmt::Debug>(result: Result<T, SourceError>) {
    assert!(
        matches!(result, Err(SourceError::Conflict(_))),
        "{result:?}"
    );
}

fn mode(path: &Path) -> u32 {
    std::fs::metadata(path).unwrap().permissions().mode() & 0o777
}

#[test]
fn schema_rejects_undeclared_fields_wrong_types_and_secret_misplacement() {
    let f = fixture();
    for values in [
        json!({"seed": true}),
        json!({"seed": "7"}),
        json!({"seed": -1}),
        json!({"seed": 10001}),
        json!({"seed": 7.0}),
        json!({"extra": 7}),
    ] {
        refused(f.apply("synthetic", update(0, values, json!({}))));
    }
    for (owner, values, secrets) in [
        ("vendor", json!({"token": SECRET}), json!({})),
        ("synthetic", json!({}), json!({"seed": "7"})),
        ("vendor", json!({}), json!({"unknown": SECRET})),
        ("vendor", json!({}), json!({"token": "bad token"})),
        (
            "vendor",
            json!({}),
            json!({"token": "separated\u{1c}token"}),
        ),
        ("vendor", json!({}), json!({"token": "x".repeat(257)})),
    ] {
        let message = refused(f.apply(owner, update(0, values, secrets)));
        assert!(!message.contains(SECRET), "{message}");
    }
    let synthetic = f.state("synthetic");
    assert_eq!(
        (synthetic.revision, json!(synthetic.values)),
        (0, json!({"seed": 7}))
    );
    assert!(!f.state("vendor").configured);
    let spec = ConfigurationSpec {
        schema_version: 1,
        fields: vec![field("enabled", "boolean")],
    };
    let values = |value: Value| value.as_object().unwrap().clone();
    assert_eq!(
        configuration::validate(&spec, &values(json!({"enabled": false})), true).unwrap(),
        values(json!({"enabled": false}))
    );
    assert!(configuration::validate(&spec, &values(json!({"enabled": 0})), true).is_err());
}

#[test]
fn saved_configuration_is_revisioned_encrypted_and_never_echoes_secrets() {
    let f = fixture();
    let state = f
        .apply("vendor", update(0, json!({}), json!({"token": SECRET})))
        .unwrap();
    assert!(state.revision == 1 && state.configured);
    assert!(state.values.is_empty() && state.secret_fields == ["token"]);
    assert!(!serde_json::to_string(&state).unwrap().contains(SECRET));
    let listing = f
        .store
        .transaction(false, |tx| {
            connections::listing(tx, &f.credentials, &f.sources)
        })
        .unwrap();
    assert!(!serde_json::to_string(&listing).unwrap().contains(SECRET));
    conflict(f.apply(
        "vendor",
        update(0, json!({}), json!({"token": "stale-token"})),
    ));
    let kept = f.apply("vendor", update(1, json!({}), json!({}))).unwrap();
    assert!(kept.revision == 2 && kept.configured);
    let cleared = f
        .apply("vendor", update(2, json!({}), json!({"token": null})))
        .unwrap();
    assert!(cleared.revision == 3 && !cleared.configured && cleared.secret_fields.is_empty());
    let rows = f
        .store
        .transaction(false, |tx| {
            tx.rows("SELECT * FROM data_provider_configurations", vec![])
        })
        .unwrap();
    assert!(!json!(rows).to_string().contains(SECRET));
    assert_eq!(mode(&f.root.join(".credentials")), 0o700);
    assert_eq!(mode(&f.root.join(".credentials/configurations")), 0o700);
    for path in f.snapshots() {
        assert!(!String::from_utf8_lossy(&std::fs::read(&path).unwrap()).contains(SECRET));
        assert_eq!(mode(&path), 0o600);
    }
}

#[test]
fn drafts_never_change_saved_state_and_report_concurrent_changes() {
    let f = fixture();
    f.apply("vendor", update(0, json!({}), json!({"token": SECRET})))
        .unwrap();
    let before = f.snapshots();
    let spec = f.spec("vendor");
    let draft = f
        .store
        .transaction(false, |tx| {
            configuration::draft(
                tx,
                &f.credentials,
                "vendor",
                &spec,
                &update(1, json!({}), json!({"token": "draft-only-token"})),
            )
        })
        .unwrap();
    assert_eq!(json!(draft), json!({"token": "draft-only-token"}));
    assert_eq!(f.snapshots(), before);
    let checked = f
        .store
        .transaction(false, |tx| {
            configuration::checked(tx, "vendor", 1, "probe complete".into())
        })
        .unwrap();
    assert!(
        !serde_json::to_string(&checked)
            .unwrap()
            .contains("draft-only-token")
    );
    // The saved configuration changed while a draft was being probed.
    f.apply("synthetic", update(0, json!({"seed": 8}), json!({})))
        .unwrap();
    conflict(f.store.transaction(false, |tx| {
        configuration::checked(tx, "synthetic", 0, "late".into())
    }));
    assert_eq!(json!(f.state("synthetic").values), json!({"seed": 8}));
    // A draft requires runnable values.
    let spec = f.spec("vendor");
    refused(f.store.transaction(false, |tx| {
        configuration::draft(
            tx,
            &f.credentials,
            "vendor",
            &spec,
            &update(1, json!({}), json!({"token": null})),
        )
    }));
}

#[test]
fn unreadable_snapshots_require_explicit_replacement() {
    let f = fixture();
    f.apply("vendor", update(0, json!({}), json!({"token": SECRET})))
        .unwrap();
    let damaged = f.snapshots().remove(0);
    std::fs::write(&damaged, b"damaged configuration snapshot").unwrap();
    let state = f.state("vendor");
    assert!(state.revision == 1 && !state.configured && state.error.is_some());
    assert!(state.secret_fields.is_empty());
    refused(f.apply("vendor", update(1, json!({}), json!({}))));
    let restored = f
        .apply(
            "vendor",
            update(1, json!({}), json!({"token": "replacement"})),
        )
        .unwrap();
    assert!(restored.revision == 2 && restored.configured && restored.error.is_none());
    assert_eq!(
        std::fs::read(&damaged).unwrap(),
        b"damaged configuration snapshot"
    );

    f.apply("synthetic", update(0, json!({"seed": 19}), json!({})))
        .unwrap();
    let fixed = f.fixed("synthetic", "synthetic");
    let path = f.root.join(format!(
        ".credentials/configurations/{}.enc",
        fixed["ref"].as_str().unwrap()
    ));
    std::fs::remove_file(&path).unwrap();
    let state = f.state("synthetic");
    assert!(state.revision == 1 && !state.configured && state.error.is_some());
    assert_eq!(json!(state.values), json!({"seed": 7}));
    refused(f.apply("synthetic", update(1, json!({}), json!({}))));
    let restored = f
        .apply("synthetic", update(1, json!({"seed": 19}), json!({})))
        .unwrap();
    assert!(restored.revision == 2 && restored.configured);
    assert!(!path.exists());
}

#[test]
fn fixed_configurations_resolve_only_their_own_snapshot() {
    let f = fixture();
    f.apply("vendor", update(0, json!({}), json!({"token": SECRET})))
        .unwrap();
    let fixed = f.fixed("vendor", "vendor");
    let spec = f.spec("vendor");
    assert_eq!(
        json!(configuration::resolve(&f.credentials, &fixed, "vendor", &spec).unwrap()),
        json!({"token": SECRET})
    );
    // Freezing the same revision again reuses the snapshot.
    assert_eq!(f.fixed("vendor", "vendor"), fixed);
    let other = Credentials::new("a-different-runtime-key-entirely", &f.root);
    assert!(refused(configuration::resolve(&other, &fixed, "vendor", &spec)).contains("固定配置"));
    let changed = |key: &str, value: Value| {
        let mut changed = fixed.clone();
        changed[key] = value;
        changed
    };
    for invalid in [
        Value::Null,
        json!({}),
        json!({"ref": "not-a-valid-ref", "schema_version": 1, "revision": 0}),
        changed("revision", json!(2)),
        changed("revision", json!(-1)),
        changed("revision", json!(1.0)),
        changed("schema_version", json!(2)),
        changed("extra", json!(1)),
    ] {
        refused(configuration::resolve(
            &f.credentials,
            &invalid,
            "vendor",
            &spec,
        ));
    }
    refused(configuration::resolve(
        &f.credentials,
        &fixed,
        "synthetic",
        &spec,
    ));
    let sealed = std::fs::read(f.snapshots().remove(0)).unwrap();
    assert!(f.credentials.opens(&sealed) && !other.opens(&sealed));
    assert!(
        f.credentials
            .freeze("../escape", 1, &Map::new(), 1)
            .is_err()
    );
}

#[test]
fn reading_credentials_does_not_create_or_modify_directories() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("absent");
    let credentials = Credentials::new(MASTER, &root);
    assert!(credentials.read(&"a".repeat(64), "vendor", 1, 0).is_err());
    assert!(!root.exists());
    let values = json!({"token": "fixture"}).as_object().unwrap().clone();
    let reference = credentials.freeze("vendor", 1, &values, 1).unwrap();
    std::fs::set_permissions(
        root.join(".credentials"),
        std::fs::Permissions::from_mode(0o750),
    )
    .unwrap();
    assert_eq!(
        credentials.read(&reference, "vendor", 1, 1).unwrap(),
        values
    );
    assert_eq!(mode(&root.join(".credentials")), 0o750);
}

#[test]
fn verification_is_versioned_and_the_latest_start_wins() {
    let f = fixture();
    f.apply("vendor", update(0, json!({}), json!({"token": SECRET})))
        .unwrap();
    let verification = |f: &Fixture| {
        f.store
            .transaction(false, |tx| configuration::verification(tx, "vendor"))
            .unwrap()
    };
    let record = |f: &Fixture, revision, started, verified| {
        f.store
            .transaction(true, |tx| {
                configuration::record_verification(
                    tx,
                    "vendor",
                    revision,
                    started,
                    started + 1.0,
                    verified,
                )
            })
            .unwrap()
    };
    assert_eq!(verification(&f).status, "never");
    record(&f, 1, 100.0, true);
    let verified = verification(&f);
    assert_eq!(
        (verified.status, verified.checked_at, verified.revision),
        ("verified", Some(101.0), Some(1))
    );
    // An earlier-started probe finishing later does not replace the result.
    record(&f, 1, 50.0, false);
    assert_eq!(verification(&f).status, "verified");
    record(&f, 1, 200.0, false);
    assert_eq!(verification(&f).status, "failed");
    // A probe of a revision that changed meanwhile is stale.
    f.apply(
        "vendor",
        update(1, json!({}), json!({"token": "new-token"})),
    )
    .unwrap();
    assert_eq!(verification(&f).status, "stale");
}

#[test]
fn connections_have_independent_lifecycles_and_listing() {
    let f = fixture();
    let create = |provider: &str, name: &str| {
        let body: NewConnection =
            serde_json::from_value(json!({"provider": provider, "name": name})).unwrap();
        f.store
            .transaction(true, |tx| connections::create(tx, &f.sources, &body))
    };
    let change = |id: &str, revision: i64, name: &str, state: &str| {
        let body: ConnectionUpdate = serde_json::from_value(
            json!({"expected_revision": revision, "name": name, "state": state}),
        )
        .unwrap();
        f.store
            .transaction(true, |tx| connections::update(tx, &f.sources, id, &body))
    };
    refused(create("unknown", "A"));
    refused(create("vendor", " \u{1c} "));
    let instance = create("synthetic", " 独立示例 ").unwrap();
    assert!(instance.id.starts_with("c_") && instance.name == "独立示例");
    let changed = change("vendor", 0, "研究连接", "disabled").unwrap();
    assert_eq!((changed.revision, changed.state.as_str()), (1, "disabled"));
    conflict(change("vendor", 0, "过期修改", "enabled"));
    change(&instance.id, 0, "归档连接", "archived").unwrap();
    refused(change("missing", 0, "A", "enabled"));
    f.store
        .transaction(true, |tx| {
            tx.execute(
                "INSERT INTO data_connections (id, provider, name) VALUES (?, ?, ?)",
                vec![
                    json!(format!("c_{}", "a".repeat(32))),
                    json!("removed"),
                    json!("旧开发连接"),
                ],
            )
        })
        .unwrap();
    let listing = f
        .store
        .transaction(false, |tx| {
            connections::listing(tx, &f.credentials, &f.sources)
        })
        .unwrap();
    let listed = json!(listing);
    let ids: Vec<&str> = listed
        .as_array()
        .unwrap()
        .iter()
        .map(|item| item["id"].as_str().unwrap())
        .collect();
    assert_eq!(ids, ["vendor", "synthetic", instance.id.as_str()]);
    assert_eq!(listed[0]["name"], "研究连接");
    assert_eq!(listed[0]["lifecycle"]["state"], "disabled");
    assert_eq!(listed[0]["plugin_id"], Value::Null);
    assert_eq!(listed[2]["plugin_id"], "synthetic");
    assert_eq!(listed[2]["connection_id"], instance.id.as_str());
    assert_eq!(listed[2]["lifecycle"]["state"], "archived");
    assert_eq!(listed[1]["configured"], true);
    assert_eq!(listed[1]["verification"]["status"], "never");
    let instance_state = f.state(&instance.id);
    assert_eq!(json!(instance_state.values), json!({"seed": 7}));
    // New tasks need an enabled connection of the requested source.
    let fix = |owner: &str, provider: &str| {
        f.store.transaction(false, |tx| {
            connections::fix_for_task(tx, &f.credentials, &f.sources, owner, provider)
        })
    };
    assert!(refused(fix(&instance.id, "synthetic")).contains("停用或归档"));
    change("synthetic", 0, "合成", "enabled").unwrap();
    assert!(refused(fix("synthetic", "vendor")).contains("不匹配"));
    assert_eq!(
        fix("synthetic", "synthetic").unwrap()["connection_name"],
        "合成"
    );
}

/// Existing snapshots stay readable and are reused: the canonical text and
/// its reference are those every saved configuration already has on disk.
#[test]
fn snapshot_references_are_stable_for_existing_configurations() {
    let directory = tempfile::tempdir().unwrap();
    let credentials = Credentials::new("snapshot-compat-master-key-long-enough", directory.path());
    let token = json!({"token": "令牌-ü-\u{2028}-\u{7f}-\"\\"});
    let mixed = json!({"seed": 7, "flag": true, "name": "中文\u{1}\t\n"});
    for (owner, values, revision, expected) in [
        (
            "tushare",
            token,
            7,
            "7cd8e780497a8bc4aec07d2a5b14a189f2748b3fd0c52ca07d99d1faa5f21c0b",
        ),
        (
            "synthetic",
            mixed,
            2,
            "481a25795fa0bac5d79063bc51b6d5e351554dfb81947ae81170eabace6ff485",
        ),
    ] {
        let values = values.as_object().unwrap().clone();
        assert_eq!(
            credentials.freeze(owner, 1, &values, revision).unwrap(),
            expected
        );
    }
}
