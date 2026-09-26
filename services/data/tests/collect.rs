//! Collection of claimed sync tasks from the built-in Tushare source, with
//! the vendor transport replaced by recorded responses; planning, mapping,
//! configuration, evidence and lease handling are the real implementations.
use asterion_data::collect::{CollectError, Host, collect};
use asterion_data::configuration::{self, ConfigurationUpdate};
use asterion_data::credentials::Credentials;
use asterion_data::observations;
use asterion_data::providers;
use asterion_data::sync::{self, Admission};
use asterion_data_store::provider::{Partition, Provider, ProviderManifest, SyncRequest};
use asterion_data_store::table::Row;
use asterion_kernel::artifacts::ArtifactStore;
use asterion_kernel::events::Topic;
use asterion_kernel::tasks::repository::{Repository, Request};
use asterion_provider_tushare::Tushare;
use asterion_store::{Store, StoreError, Transaction};
use serde_json::{Map, Value, json};
use std::sync::Mutex;

const TOKEN: &str = "synthetic-provider-token-never-log";
const MASTER: &str = "test-runtime-token-at-least-24-characters";

/// Tushare with its vendor transport replaced by recorded responses.
struct Recorded {
    inner: Tushare,
    failing: Option<(String, String)>,
    fetched: Mutex<Vec<(String, String)>>,
}
impl Recorded {
    fn new(failing: Option<(&str, &str)>) -> Self {
        Self {
            inner: Tushare::default(),
            failing: failing.map(|(day, error)| (day.into(), error.into())),
            fetched: Mutex::new(Vec::new()),
        }
    }
    fn days(&self) -> Vec<String> {
        self.fetched
            .lock()
            .unwrap()
            .iter()
            .map(|(day, _)| day.clone())
            .collect()
    }
}
impl Provider for Recorded {
    fn manifest(&self) -> ProviderManifest {
        self.inner.manifest()
    }
    fn plan(&self, request: &SyncRequest) -> Result<Vec<Partition>, String> {
        self.inner.plan(request)
    }
    fn probe(&self, _: &Map<String, Value>) -> Result<String, String> {
        Err("not probed".into())
    }
    fn fetch(
        &self,
        partition: &Partition,
        configuration: &Map<String, Value>,
    ) -> Result<Vec<Value>, String> {
        let day = partition.params["start_date"].clone();
        let token = configuration["token"]
            .as_str()
            .unwrap_or_default()
            .to_string();
        self.fetched.lock().unwrap().push((day.clone(), token));
        if let Some((failing, error)) = &self.failing
            && failing == &day
        {
            return Err(error.clone());
        }
        // One calendar row per requested day; an undeclared vendor field
        // never enters the evidence store.
        let day = |text: &str| chrono::NaiveDate::parse_from_str(text, "%Y%m%d").unwrap();
        let (mut cursor, end) = (
            day(&partition.params["start_date"]),
            day(&partition.params["end_date"]),
        );
        let mut rows = Vec::new();
        while cursor <= end {
            let open = u32::from(chrono::Datelike::weekday(&cursor).number_from_monday() < 6);
            rows.push(
                json!({"exchange": "SHFE", "cal_date": cursor.format("%Y%m%d").to_string(),
                             "is_open": open, "pretrade_date": null, "undeclared": "vendor extra"}),
            );
            cursor = cursor.succ_opt().unwrap();
        }
        Ok(rows)
    }
    fn normalize(&self, request: &SyncRequest, rows: &[Value]) -> Result<Vec<Row>, String> {
        self.inner.normalize(request, rows)
    }
}

struct Fixture {
    _directory: tempfile::TempDir,
    store: Store,
    artifacts: ArtifactStore,
    tasks: Repository,
    credentials: Credentials,
    trace: Value,
}

fn fixture() -> Fixture {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("data");
    std::fs::create_dir(&root).unwrap();
    let store = Store::open(&format!(
        "sqlite:///{}",
        directory.path().join("sync.db").display()
    ))
    .unwrap();
    store
        .transaction(true, |tx: &Transaction| {
            for ddl in [
                "CREATE TABLE jobs(id TEXT PRIMARY KEY,command_id TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,payload JSON NOT NULL,state TEXT NOT NULL,attempt INTEGER NOT NULL,token TEXT,worker_id TEXT,lease_until FLOAT,created_at FLOAT NOT NULL,error TEXT,result JSON)",
                "CREATE TABLE communication_heads(topic TEXT PRIMARY KEY,sequence BIGINT NOT NULL)",
                "CREATE TABLE communication_events(id TEXT PRIMARY KEY,topic TEXT NOT NULL,sequence BIGINT NOT NULL,stream TEXT NOT NULL,message JSON NOT NULL,UNIQUE(topic,sequence))",
                "CREATE TABLE ingestion_observations(job_id VARCHAR,attempt INTEGER,partition_index INTEGER,manifest JSON NOT NULL,PRIMARY KEY(job_id,attempt,partition_index))",
                "CREATE TABLE data_connections(id VARCHAR PRIMARY KEY,provider VARCHAR NOT NULL,name VARCHAR NOT NULL)",
                "CREATE TABLE data_connection_settings(id VARCHAR PRIMARY KEY,name VARCHAR NOT NULL,state VARCHAR NOT NULL,revision INTEGER NOT NULL)",
                "CREATE TABLE data_provider_configurations(provider VARCHAR PRIMARY KEY,revision INTEGER NOT NULL,schema_version INTEGER NOT NULL,snapshot_ref VARCHAR NOT NULL)",
            ] {
                tx.execute(ddl, vec![])?;
            }
            Ok::<_, StoreError>(())
        })
        .unwrap();
    let tasks = Repository::new(
        store.database_id(),
        Topic {
            id: "runtime.task.changed".into(),
            owner: "asterion.runtime".into(),
            payload: "task".into(),
            read_path: "/jobs".into(),
        },
    )
    .unwrap();
    let fixture = Fixture {
        artifacts: ArtifactStore::new(&root, 1 << 30).unwrap(),
        credentials: Credentials::new(MASTER, &root),
        trace: serde_json::to_value(asterion_kernel::communication::context(None, 600.0).unwrap())
            .unwrap(),
        _directory: directory,
        store,
        tasks,
    };
    let update: ConfigurationUpdate =
        serde_json::from_value(json!({"expected_revision": 0, "secrets": {"token": TOKEN}}))
            .unwrap();
    let spec = providers::get("tushare").unwrap().manifest().configuration;
    fixture
        .store
        .transaction(true, |tx| {
            configuration::apply(tx, &fixture.credentials, "tushare", &spec, &update)
        })
        .unwrap();
    fixture
}

impl Fixture {
    fn host(&self) -> Host<'_> {
        Host {
            store: &self.store,
            artifacts: &self.artifacts,
            tasks: &self.tasks,
            credentials: &self.credentials,
            lease_seconds: 60.0,
            trace: &self.trace,
        }
    }
    fn submit(&self, command: &str, admission: Admission) -> Value {
        let request: SyncRequest = serde_json::from_value(json!({
            "command_id": command, "provider": "tushare", "dataset": "calendar",
            "exchange": "SHFE", "start": "2024-01-01", "end": "2024-02-05",
        }))
        .unwrap();
        self.store
            .transaction(true, |tx| {
                sync::submit(
                    tx,
                    &self.artifacts,
                    &self.tasks,
                    &self.credentials,
                    &request,
                    &admission,
                    now(),
                    &self.trace,
                )
            })
            .unwrap()
    }
    fn claim(&self) -> (Value, String) {
        let token = uuid::Uuid::new_v4().to_string();
        let claim = serde_json::from_value(json!({
            "op": "claim", "worker_id": "test", "now": now(), "lease_seconds": 60.0, "token": token,
        }))
        .unwrap();
        let job = self
            .store
            .transaction(true, |tx| {
                self.tasks
                    .run(tx.connection(), claim, self.trace.clone())
                    .map_err(|error| StoreError::from(error.to_string()))
            })
            .unwrap();
        (job, token)
    }
    fn fail(&self, job: &Value, token: &str) {
        self.host()
            .fail(job["id"].as_str().unwrap(), token, "interrupted".into());
    }
    fn observations(&self, job: &Value) -> Value {
        self.store
            .transaction(false, |tx| {
                observations::list(tx, job["id"].as_str().unwrap(), 0, 100)
                    .map_err(|_| StoreError::from("list".to_string()))
            })
            .unwrap()
    }
    fn job(&self, id: &str) -> Value {
        self.store
            .transaction(false, |tx| {
                self.tasks
                    .run(
                        tx.connection(),
                        Request::Get { id: id.into() },
                        self.trace.clone(),
                    )
                    .map_err(|error| StoreError::from(error.to_string()))
            })
            .unwrap()
    }
}

fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs_f64()
}

#[test]
fn collection_retains_declared_fields_and_reports_progress() {
    let f = fixture();
    f.submit("collect", Admission::default());
    let (job, token) = f.claim();
    let provider = Recorded::new(None);
    let content = collect(&f.host(), &provider, &job, &token).unwrap();
    let evidence: Vec<Value> = serde_json::from_slice(&content).unwrap();
    assert_eq!(evidence.len(), 2);
    assert!(
        evidence
            .iter()
            .all(|value| value["rows"][0].get("undeclared").is_none())
    );
    assert!(
        provider
            .fetched
            .lock()
            .unwrap()
            .iter()
            .all(|(_, token)| token == TOKEN)
    );
    let saved = f.observations(&job);
    assert_eq!(saved["total"], 2);
    assert!(
        saved["items"]
            .as_array()
            .unwrap()
            .iter()
            .all(|item| item["manifest"]["status"] == "RECEIVED")
    );
    let state = f.job(job["id"].as_str().unwrap());
    assert_eq!(state["result"], json!({"completed": 2, "total": 2}));
    assert!(state["lease_until"].as_f64().unwrap() > now());
}

#[test]
fn failures_keep_prior_evidence_and_resume_fetches_only_the_rest() {
    let f = fixture();
    let first = f.submit("first", Admission::default());
    let (job, token) = f.claim();
    let provider = Recorded::new(Some(("20240201", &format!("PERMISSION_DENIED：{TOKEN}"))));
    match collect(&f.host(), &provider, &job, &token) {
        Err(CollectError::Failed(reason)) => {
            assert_eq!(reason, "PERMISSION_DENIED：采集失败，请检查数据源后重试");
        }
        other => panic!("{other:?}"),
    }
    let saved = f.observations(&job);
    let statuses: Vec<&Value> = saved["items"]
        .as_array()
        .unwrap()
        .iter()
        .map(|item| &item["manifest"]["status"])
        .collect();
    assert_eq!(statuses, ["RECEIVED", "PERMISSION_DENIED"]);
    assert!(!saved.to_string().contains(TOKEN));
    f.fail(&job, &token);
    // A retry that resumes the failed task reuses the verified partition.
    let admission = Admission {
        resume_from: first["id"].as_str().map(str::to_string),
        retry_of: first["id"].as_str().map(str::to_string),
        ..Admission::default()
    };
    f.submit("resume", admission);
    let (job, token) = f.claim();
    let provider = Recorded::new(None);
    let content = collect(&f.host(), &provider, &job, &token).unwrap();
    assert_eq!(provider.days(), ["20240201"]);
    let evidence: Vec<Value> = serde_json::from_slice(&content).unwrap();
    assert_eq!(evidence[0]["reused_from"]["job_id"], first["id"]);
    assert!(evidence[1].get("reused_from").is_none());
    // Unclassified source errors fail as a generic fetch failure.
    f.submit("other", Admission::default());
    let (job, token) = f.claim();
    let provider = Recorded::new(Some(("20240101", "connection reset")));
    match collect(&f.host(), &provider, &job, &token) {
        Err(CollectError::Failed(reason)) => {
            assert!(reason.starts_with("FETCH_FAILED："), "{reason}")
        }
        other => panic!("{other:?}"),
    }
}

#[test]
fn unfixed_or_changed_inputs_are_refused_before_fetching() {
    let f = fixture();
    f.submit("fixed", Admission::default());
    let (job, token) = f.claim();
    let provider = Recorded::new(None);
    let mut unfixed = job.clone();
    unfixed["payload"]
        .as_object_mut()
        .unwrap()
        .remove("configuration");
    match collect(&f.host(), &provider, &unfixed, &token) {
        Err(CollectError::Failed(reason)) => assert!(reason.contains("固定配置"), "{reason}"),
        other => panic!("{other:?}"),
    }
    let mut changed = job.clone();
    changed["payload"]["plugin_version"] = json!("0.0.0");
    assert!(matches!(
        collect(&f.host(), &provider, &changed, &token),
        Err(CollectError::Failed(_))
    ));
    assert!(provider.days().is_empty());
    // A lost lease stops collection without failing the task.
    assert!(matches!(
        collect(&f.host(), &provider, &job, "stale"),
        Err(CollectError::Lease(_))
    ));
}
