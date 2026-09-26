use asterion_connections::*;
use serde_json::{Value, json};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicUsize, Ordering},
};

struct Secrets(asterion_kernel::secrets::Secrets);
impl SecretPort for Secrets {
    fn encrypt(&self, value: &[u8]) -> Result<Vec<u8>> {
        Ok(self.0.encrypt(value))
    }
    fn decrypt(&self, value: &[u8]) -> Result<Vec<u8>> {
        self.0
            .decrypt(value)
            .map_err(|_| Error::invalid("invalid secret"))
    }
}
#[derive(Default)]
struct Cancel(AtomicBool);
impl Cancellation for Cancel {
    fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}
#[derive(Default)]
struct Source {
    sessions: Mutex<Vec<Arc<Adapter>>>,
    live: Arc<AtomicUsize>,
    maximum: AtomicUsize,
}
struct Adapter {
    emit: Mutex<Option<Emitter>>,
    close_fails: AtomicBool,
    closed: AtomicBool,
    malformed: Mutex<Option<&'static str>>,
    live: Arc<AtomicUsize>,
    retained: Mutex<Option<Arc<ReadTicket>>>,
    pause: AtomicBool,
}
impl Connector for Source {
    fn validate(&self, config: &Values, secrets: &Values) -> Result<()> {
        if config.get("user").is_none_or(String::is_empty)
            || secrets.get("password").is_none_or(String::is_empty)
        {
            return Err(Error::invalid("credentials missing"));
        }
        Ok(())
    }
    fn open(&self, _: &ConnectionProfile, _: &Values) -> Result<Arc<dyn Session>> {
        let live = self.live.fetch_add(1, Ordering::AcqRel) + 1;
        self.maximum.fetch_max(live, Ordering::AcqRel);
        let value = Arc::new(Adapter {
            emit: Mutex::new(None),
            close_fails: AtomicBool::new(false),
            closed: AtomicBool::new(false),
            malformed: Mutex::new(None),
            live: self.live.clone(),
            retained: Mutex::new(None),
            pause: AtomicBool::new(false),
        });
        self.sessions.lock().unwrap().push(value.clone());
        Ok(value)
    }
}
impl Session for Adapter {
    fn start_market(&self, _: &[Subscription], emit: Emitter) -> Result<()> {
        assert!(emit.accept("connected", None).is_some());
        *self.emit.lock().unwrap() = Some(emit);
        Ok(())
    }
    fn subscriptions(&self, _: &[Subscription]) -> Result<()> {
        Ok(())
    }
    fn stop_market(&self) -> Result<()> {
        Ok(())
    }
    fn read(&self, _: ReadKind, request: &ReadRequest, cancel: Arc<ReadTicket>) -> Result<Value> {
        assert!(!cancel.is_cancelled());
        *self.retained.lock().unwrap() = Some(cancel.clone());
        while self.pause.load(Ordering::Acquire) && !cancel.is_cancelled() {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        let mut value = serde_json::to_value(request)?;
        value["observed_at"] = json!(request.started_at);
        value["complete"] = json!(true);
        match *self.malformed.lock().unwrap() {
            Some("request") => value["request_id"] = json!("unissued"),
            Some("generation") => value["generation"] = json!(999),
            Some("time") => value["started_at"] = json!(0),
            Some("complete") => value["complete"] = json!(false),
            _ => {}
        }
        Ok(value)
    }
    fn close(&self) -> Result<()> {
        if self.close_fails.load(Ordering::Acquire) {
            return Err(Error::invalid("private credentials"));
        }
        if !self.closed.swap(true, Ordering::AcqRel) {
            self.live.fetch_sub(1, Ordering::AcqRel);
        }
        Ok(())
    }
}
fn descriptor() -> ConnectorDescriptor {
    serde_json::from_value(json!({"id":"test", "owner":"test.source", "version":1, "title":"Test", "instructions":"", "capabilities":["market_quotes","instrument_catalog","account_snapshot","positions"], "fields":[{"key":"user","label":"User","secret":false,"required":true,"identity":true,"default":""},{"key":"endpoint","label":"Endpoint","secret":false,"required":true,"identity":false,"default":""},{"key":"password","label":"Password","secret":true,"required":true,"identity":false,"default":""}]})).unwrap()
}
fn manager(path: &std::path::Path, source: Arc<Source>) -> Arc<Manager> {
    let manager = Manager::new(
        path,
        Arc::new(Secrets(asterion_kernel::secrets::Secrets::new(
            b"testing",
            b"connections",
            b"profile",
        ))),
        Values::from([("test".into(), "test.source".into())]),
    );
    manager
        .initialize(vec![Contribution {
            descriptor: descriptor(),
            source,
        }])
        .unwrap();
    manager
}
fn body() -> Value {
    json!({"connection_id":null,"expected_revision":null,"connector_id":"test","name":"示例","config":{"user":"account","endpoint":"server"},"secrets":{"password":{"action":"replace","value":"private-fixture"}}})
}
fn save(manager: &Manager) -> String {
    manager
        .save(serde_json::from_value(body()).unwrap())
        .unwrap()["connection_id"]
        .as_str()
        .unwrap()
        .into()
}

#[test]
fn persistence_secret_binding_and_failed_publication_are_atomic() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("connections/profiles.json");
    let source = Arc::new(Source::default());
    let manager = manager(&path, source.clone());
    let id = save(&manager);
    let original = std::fs::read(&path).unwrap();
    assert!(!String::from_utf8_lossy(&original).contains("private-fixture"));
    let restarted = self::manager(&path, source);
    assert_eq!(restarted.profiles().as_slice(), std::slice::from_ref(&id));
    assert!(restarted.active_id().is_none());
    let mut changed = body();
    changed["connection_id"] = json!(id);
    changed["expected_revision"] = json!(1);
    changed["name"] = json!("changed");
    let parent = path.parent().unwrap();
    let moved = dir.path().join("retained");
    std::fs::rename(parent, &moved).unwrap();
    std::fs::write(parent, "block directory").unwrap();
    assert!(
        manager
            .save(serde_json::from_value(changed).unwrap())
            .unwrap_err()
            .io
    );
    assert_eq!(manager.profile(&id).unwrap().name, "示例");
    std::fs::remove_file(parent).unwrap();
    std::fs::rename(&moved, parent).unwrap();
    assert_eq!(std::fs::read(&path).unwrap(), original);
    let mut corrupted: Value = serde_json::from_slice(&original).unwrap();
    corrupted["connections"][0]["name"] = json!("different identity");
    std::fs::write(&path, serde_json::to_vec(&corrupted).unwrap()).unwrap();
    let rejected = self::manager(&path, Arc::new(Source::default()));
    assert!(rejected.profiles().is_empty());
    assert!(
        rejected
            .save(serde_json::from_value(body()).unwrap())
            .is_err()
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&std::fs::read(path).unwrap()).unwrap(),
        corrupted
    );
}

#[test]
fn revisions_identity_fields_secrets_and_current_schema_are_enforced() {
    let dir = tempfile::tempdir().unwrap();
    let manager = manager(
        &dir.path().join("profiles.json"),
        Arc::new(Source::default()),
    );
    let id = save(&manager);
    for change in [
        json!({"expected_revision":2}),
        json!({"config":{"user":"other","endpoint":"server"}}),
        json!({"config":{"user":"account","endpoint":"different"},"secrets":{"password":{"action":"keep"}}}),
        json!({"secrets":{"password":{"action":"keep","value":"unexpected"}}}),
    ] {
        let mut value = body();
        value["connection_id"] = json!(id);
        value["expected_revision"] = json!(1);
        value
            .as_object_mut()
            .unwrap()
            .extend(change.as_object().unwrap().clone());
        assert!(
            manager
                .save(serde_json::from_value(value).unwrap())
                .is_err()
        );
    }
    let mut descriptor = descriptor();
    descriptor.fields.push(descriptor.fields[0].clone());
    assert!(descriptor.validate().is_err());
    assert!(
        Subscription {
            exchange: "CZCE".into(),
            symbol: "SA609".into()
        }
        .validate()
        .is_ok()
    );
    assert!(
        Subscription {
            exchange: "SHFE".into(),
            symbol: "rb0000".into()
        }
        .validate()
        .is_err()
    );
    assert_eq!(manager.profile(&id).unwrap().config_revision, 1);
}

#[test]
fn one_session_close_failure_generation_and_revoked_emitters() {
    let dir = tempfile::tempdir().unwrap();
    let source = Arc::new(Source::default());
    let manager = manager(&dir.path().join("profiles.json"), source.clone());
    let a = save(&manager);
    let b = save(&manager);
    manager.connect(&a).unwrap();
    let old = source.sessions.lock().unwrap()[0].clone();
    let emit = old.emit.lock().unwrap().clone().unwrap();
    old.close_fails.store(true, Ordering::Release);
    let error = manager.connect(&b).unwrap_err();
    assert!(!error.message.contains("private"));
    assert_eq!(manager.active_id().as_deref(), Some(a.as_str()));
    assert_eq!(source.sessions.lock().unwrap().len(), 1);
    assert!(emit.accept("connected", None).is_none());
    old.close_fails.store(false, Ordering::Release);
    manager.connect(&b).unwrap();
    assert!(old.closed.load(Ordering::Acquire));
    assert!(emit.accept("connected", None).is_none());
    assert_eq!(
        manager.channel(&a, Channel::Account).unwrap().state,
        ConnectionStatus::Disconnected
    );
    manager.close().unwrap();
    assert_eq!(source.maximum.load(Ordering::Acquire), 1);
    assert_eq!(source.live.load(Ordering::Acquire), 0);
}

#[test]
fn requests_bind_exact_identity_and_tokens_end_after_one_call() {
    let dir = tempfile::tempdir().unwrap();
    let source = Arc::new(Source::default());
    let manager = manager(&dir.path().join("profiles.json"), source.clone());
    let id = save(&manager);
    manager.connect(&id).unwrap();
    let session = source.sessions.lock().unwrap()[0].clone();
    for wrong in ["request", "generation", "time", "complete"] {
        *session.malformed.lock().unwrap() = Some(wrong);
        assert!(
            manager
                .read(&id, ReadKind::Account, Arc::new(Cancel::default()))
                .is_err()
        );
        assert_eq!(
            manager.channel(&id, Channel::Account).unwrap().state,
            ConnectionStatus::Error
        );
        assert!(
            session
                .retained
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .is_cancelled()
        );
    }
    *session.malformed.lock().unwrap() = None;
    let first = manager
        .read(&id, ReadKind::Account, Arc::new(Cancel::default()))
        .unwrap();
    let second = manager
        .read(&id, ReadKind::Account, Arc::new(Cancel::default()))
        .unwrap();
    assert_ne!(first["request_id"], second["request_id"]);
    let cancelled = Arc::new(Cancel::default());
    cancelled.0.store(true, Ordering::Release);
    assert!(manager.read(&id, ReadKind::Account, cancelled).is_err());
    manager.close().unwrap();
}

#[test]
fn concurrent_switches_never_overlap_and_restart_never_connects() {
    let dir = tempfile::tempdir().unwrap();
    let source = Arc::new(Source::default());
    let manager = manager(&dir.path().join("profiles.json"), source.clone());
    let ids = [save(&manager), save(&manager), save(&manager)];
    let threads = ids
        .into_iter()
        .map(|id| {
            let manager = manager.clone();
            std::thread::spawn(move || {
                for _ in 0..10 {
                    manager.connect(&id).unwrap();
                }
            })
        })
        .collect::<Vec<_>>();
    for thread in threads {
        thread.join().unwrap();
    }
    assert_eq!(source.maximum.load(Ordering::Acquire), 1);
    manager.close().unwrap();
    assert_eq!(source.live.load(Ordering::Acquire), 0);
}

#[test]
fn reconnect_epoch_and_callback_reads_do_not_reenter_lifecycle() {
    let dir = tempfile::tempdir().unwrap();
    let source = Arc::new(Source::default());
    let manager = manager(&dir.path().join("profiles.json"), source.clone());
    let id = save(&manager);
    manager.connect(&id).unwrap();
    let emit = source.sessions.lock().unwrap()[0]
        .emit
        .lock()
        .unwrap()
        .clone()
        .unwrap();
    let initial = manager.channel(&id, Channel::Market).unwrap().generation;
    assert_eq!(emit.accept("reconnecting", None), Some(initial + 1));
    assert_eq!(emit.accept("reconnecting", None), Some(initial + 1));
    assert_eq!(emit.accept("connected", None), Some(initial + 1));
    assert_eq!(
        manager.channel(&id, Channel::Account).unwrap().generation,
        initial
    );
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                emit.dispatch(|| {
                    assert_eq!(manager.snapshot().unwrap()["active_id"], id);
                    assert!(manager.disconnect(&id).is_err());
                })
            })
            .join()
            .unwrap();
    });
    manager.close().unwrap();
}

#[test]
fn cancellation_during_query_never_revives_a_disconnected_channel() {
    let dir = tempfile::tempdir().unwrap();
    let source = Arc::new(Source::default());
    let manager = manager(&dir.path().join("profiles.json"), source.clone());
    let id = save(&manager);
    manager.connect(&id).unwrap();
    let session = source.sessions.lock().unwrap()[0].clone();
    session.pause.store(true, Ordering::Release);
    std::thread::scope(|scope| {
        let query =
            scope.spawn(|| manager.read(&id, ReadKind::Account, Arc::new(Cancel::default())));
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        while session.retained.lock().unwrap().is_none() {
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        manager.disconnect(&id).unwrap();
        assert_eq!(
            query.join().unwrap().unwrap_err().category,
            "session_invalid"
        );
    });
    assert_eq!(
        manager.channel(&id, Channel::Account).unwrap().state,
        ConnectionStatus::Disconnected
    );
    manager.close().unwrap();
}

#[test]
fn bounded_current_contract_rejects_oversized_or_ambiguous_data() {
    assert!(
        bounded_value(
            &json!({"content":"x".repeat(MAX_READ_BYTES)}),
            MAX_READ_BYTES
        )
        .is_err()
    );
    let mut value = json!(0);
    for _ in 0..66 {
        value = json!([value]);
    }
    assert!(bounded_value(&value, MAX_READ_BYTES).is_err());
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("profiles.json");
    for invalid in [
        r#"{"version":2,"connections":[]}"#,
        r#"{"version":2,"version":2,"connections":[],"selected_id":null}"#,
    ] {
        std::fs::write(&path, invalid).unwrap();
        let manager = manager(&path, Arc::new(Source::default()));
        assert!(
            manager
                .save(serde_json::from_value(body()).unwrap())
                .is_err()
        );
        assert_eq!(std::fs::read_to_string(&path).unwrap(), invalid);
    }
}
