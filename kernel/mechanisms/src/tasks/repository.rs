//! Fixed task persistence. Lease transitions and events share one physical transaction.
use super::{Action, Predicate, RecordRequest, ReuseRequest, Snapshot, TransitionRequest};
use crate::{
    database::{Connection, OperationError, Session, TransactionRef},
    events::{self, Registry, Topic},
    storage::Transaction,
};
use serde::Deserialize;
use serde_json::{Map, Value, json};
use std::{collections::BTreeSet, sync::Arc, time::Instant};

type Result<T> = std::result::Result<T, OperationError>;
const COLUMNS: &str =
    "id,command_id,kind,payload,state,attempt,token,worker_id,lease_until,created_at,error,result";
const NAMES: [&str; 12] = [
    "id",
    "command_id",
    "kind",
    "payload",
    "state",
    "attempt",
    "token",
    "worker_id",
    "lease_until",
    "created_at",
    "error",
    "result",
];
const PUBLIC_COLUMNS: &str =
    "id,command_id,kind,NULL,state,attempt,NULL,worker_id,lease_until,created_at,error,result";

#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Request {
    Submit {
        record: RecordRequest,
    },
    SubmitBatch {
        records: Vec<RecordRequest>,
    },
    List,
    Get {
        id: String,
    },
    Claim {
        worker_id: String,
        now: f64,
        lease_seconds: f64,
        token: String,
        /// Omitted only by hosts that execute every kind.
        #[serde(default)]
        kinds: Option<super::ClaimKinds>,
    },
    Lease {
        id: String,
        token: String,
        now: f64,
    },
    Apply {
        id: String,
        action: Action,
        now: f64,
    },
    CancelBatch {
        ids: BTreeSet<String>,
    },
    Isolate {
        reason: String,
    },
}

#[derive(Clone)]
pub struct Repository {
    database: u64,
    registry: Arc<Registry>,
    topic: Topic,
}
impl Repository {
    pub fn new(database: u64, topic: Topic) -> Result<Self> {
        if topic.id != "runtime.task.changed" || topic.owner != "asterion.runtime" {
            return Err("Invalid task event declaration".into());
        }
        Ok(Self {
            database,
            registry: Arc::new(Registry::new(vec![topic.clone()])?),
            topic,
        })
    }
    pub fn run(&self, connection: &Connection, request: Request, trace: Value) -> Result<Value> {
        self.execute(connection.transaction()?, request, trace, None)
    }
    pub fn granted(&self, kinds: BTreeSet<String>) -> Grant {
        Grant {
            repository: self.clone(),
            kinds: Arc::new(kinds),
        }
    }
    fn execute(
        &self,
        ticket: TransactionRef,
        request: Request,
        trace: Value,
        kinds: Option<Arc<BTreeSet<String>>>,
    ) -> Result<Value> {
        if ticket.database_id() != self.database {
            return Err("Task transaction belongs to another database".into());
        }
        let repository = self.clone();
        let started = Instant::now();
        ticket.operation(move |session| {
            repository.dispatch(session, request, trace, kinds.as_deref(), started)
        })
    }
    fn changed(&self, session: &mut Session<'_>, row: &Value, trace: &Value) -> Result<Value> {
        let mut trace = trace.clone();
        let previous=session.execute("SELECT message FROM communication_events WHERE topic=$1 AND stream=$2 ORDER BY sequence DESC LIMIT 1",vec![json!(self.topic.id),row["id"].clone()])?;
        if let Some(previous) = previous.first() {
            let previous: Value =
                serde_json::from_str(previous[0].as_str().ok_or("Invalid task event")?)
                    .map_err(|_| "Invalid task event")?;
            trace["correlation_id"] = previous["correlation_id"].clone();
            trace["request_id"] = previous["id"].clone();
        }
        let stream = row["id"].as_str().ok_or("Invalid task identity")?;
        let payload = json!({"job_id":stream,"kind":row["kind"],"state":row["state"],"attempt":row["attempt"]});
        let draft = self.registry.prepare(&self.topic, stream, payload, trace)?;
        events::publish_in(session, &self.topic.id, stream, draft)
    }
    fn insert(
        &self,
        session: &mut Session<'_>,
        request: RecordRequest,
        trace: &Value,
        idempotent: bool,
        kinds: Option<&BTreeSet<String>>,
    ) -> Result<Value> {
        check_kind(kinds, &request.kind)?;
        let record = super::record(request)?;
        let conflict = if idempotent {
            " ON CONFLICT(command_id) DO NOTHING"
        } else {
            ""
        };
        let inserted=session.execute(&format!("INSERT INTO jobs(id,command_id,kind,payload,state,attempt,created_at) VALUES($1,$2,$3,$4,$5,$6,$7){conflict} RETURNING id"),
            vec![record["id"].clone(),record["command_id"].clone(),record["kind"].clone(),json!(record["payload"].to_string()),record["state"].clone(),record["attempt"].clone(),record["created_at"].clone()])?;
        if inserted.is_empty() {
            let old = query_one(
                session,
                "command_id=$1",
                vec![record["command_id"].clone()],
                false,
            )?
            .ok_or("Task disappeared after conflict")?;
            super::check_reuse(&ReuseRequest {
                original_kind: text(&old, "kind")?.into(),
                original_payload: object(&old["payload"])?,
                kind: text(&record, "kind")?.into(),
                payload: object(&record["payload"])?,
            })?;
            return Ok(old);
        }
        self.changed(session, &record, trace)?;
        Ok(record)
    }
    fn apply(
        &self,
        session: &mut Session<'_>,
        mut row: Value,
        action: Action,
        now: f64,
        trace: &Value,
    ) -> Result<(bool, Value, Option<Value>)> {
        let plan = super::transition(TransitionRequest {
            row: snapshot(&row)?,
            action,
            now,
        })?;
        let Some(plan) = plan else {
            return Ok((false, row, None));
        };
        let mut params = Vec::new();
        let mut assignments = Vec::new();
        for (column, value) in &plan.values {
            if !matches!(
                column.as_str(),
                "state" | "attempt" | "token" | "worker_id" | "lease_until" | "error" | "result"
            ) {
                return Err("Invalid task update".into());
            }
            params.push(if column == "result" && !value.is_null() {
                json!(value.to_string())
            } else {
                value.clone()
            });
            assignments.push(format!("{column}=${}", params.len()));
        }
        let condition = predicate(&plan.condition, &mut params);
        let changed = session.execute(
            &format!(
                "UPDATE jobs SET {} WHERE {condition} RETURNING id",
                assignments.join(",")
            ),
            params,
        )?;
        if changed.len() != 1 {
            return Err("Task changed before its transition committed".into());
        }
        row.as_object_mut()
            .ok_or("Invalid task row")?
            .extend(plan.values);
        let event = if plan.event {
            Some(self.changed(session, &row, trace)?)
        } else {
            None
        };
        Ok((true, row, event))
    }
    fn dispatch(
        &self,
        session: &mut Session<'_>,
        request: Request,
        trace: Value,
        kinds: Option<&BTreeSet<String>>,
        started: Instant,
    ) -> Result<Value> {
        match request {
            Request::Submit { record } => self.insert(session, record, &trace, true, kinds),
            Request::SubmitBatch { records } => {
                for record in &records {
                    check_kind(kinds, &record.kind)?;
                }
                Ok(Value::Array(
                    records
                        .into_iter()
                        .map(|record| self.insert(session, record, &trace, false, kinds))
                        .collect::<Result<_>>()?,
                ))
            }
            Request::List => {
                host_only(kinds)?;
                Ok(Value::Array(
                    session
                        .execute(
                            &format!(
                                "SELECT {PUBLIC_COLUMNS} FROM jobs ORDER BY created_at DESC LIMIT 100"
                            ),
                            vec![],
                        )?
                        .into_iter()
                        .map(decode)
                        .map(|row| row.map(public))
                        .collect::<Result<_>>()?,
                ))
            }
            Request::Get { id } => {
                host_only(kinds)?;
                Ok(session
                    .execute(
                        &format!("SELECT {PUBLIC_COLUMNS} FROM jobs WHERE id=$1"),
                        vec![json!(id)],
                    )?
                    .into_iter()
                    .next()
                    .map(decode)
                    .transpose()?
                    .map(public)
                    .unwrap_or(Value::Null))
            }
            Request::Claim {
                worker_id,
                now,
                lease_seconds,
                token,
                kinds: scope,
            } => {
                host_only(kinds)?;
                let mut params = vec![];
                let condition = predicate(&super::claim_filter(now, scope.as_ref())?, &mut params);
                let lock = if session.is_postgres() {
                    " FOR UPDATE SKIP LOCKED"
                } else {
                    ""
                };
                let rows=session.execute(&format!("SELECT {COLUMNS} FROM jobs WHERE {condition} ORDER BY created_at LIMIT 1{lock}"),params)?;
                let Some(row) = rows.into_iter().next() else {
                    return Ok(Value::Null);
                };
                let (_, mut row, event) = self.apply(
                    session,
                    decode(row)?,
                    Action::Claim {
                        worker_id,
                        token,
                        lease_seconds,
                    },
                    now + started.elapsed().as_secs_f64(),
                    &trace,
                )?;
                if let Some(event) = event {
                    row["communication_event"] = event;
                }
                Ok(row)
            }
            Request::Lease { id, token, now } => {
                let row = query_one(session, "id=$1", vec![json!(id)], true)?;
                super::require_lease(
                    row.as_ref().map(snapshot).transpose()?.as_ref(),
                    &token,
                    now + started.elapsed().as_secs_f64(),
                )?;
                let row = row.ok_or("Task not found")?;
                check_kind(kinds, text(&row, "kind")?)?;
                Ok(row)
            }
            Request::Apply { id, action, now } => {
                if kinds.is_some()
                    && !matches!(&action, Action::Complete { .. } | Action::Progress { .. })
                {
                    return Err("Task operation is not granted".into());
                }
                let row = query_one(session, "id=$1", vec![json!(id)], true)?
                    .ok_or("Lease expired, cancelled, or superseded")?;
                check_kind(kinds, text(&row, "kind")?)?;
                Ok(json!(
                    self.apply(
                        session,
                        row,
                        action,
                        now + started.elapsed().as_secs_f64(),
                        &trace
                    )?
                    .0 as u64
                ))
            }
            Request::CancelBatch { ids } => {
                let mut rows = Vec::new();
                for id in ids {
                    let row = query_one(session, "id=$1", vec![json!(id)], true)?;
                    if let Some(row) = row {
                        check_kind(kinds, text(&row, "kind")?)?;
                        rows.push(row);
                    } else if kinds.is_some() {
                        return Err("Task not found".into());
                    }
                }
                let mut count = 0;
                for row in rows {
                    count += self.apply(session, row, Action::Cancel, 0., &trace)?.0 as u64;
                }
                Ok(json!(count))
            }
            Request::Isolate { reason } => {
                host_only(kinds)?;
                let lock = if session.is_postgres() {
                    " FOR UPDATE"
                } else {
                    ""
                };
                let rows=session.execute(&format!("SELECT {COLUMNS} FROM jobs WHERE state IN ('QUEUED','RUNNING') ORDER BY id{lock}"),vec![])?;
                let mut count = 0;
                for row in rows {
                    count += self
                        .apply(
                            session,
                            decode(row)?,
                            Action::Isolate {
                                reason: reason.clone(),
                            },
                            0.,
                            &trace,
                        )?
                        .0 as u64;
                }
                Ok(json!(count))
            }
        }
    }
}

pub struct Grant {
    repository: Repository,
    kinds: Arc<BTreeSet<String>>,
}
impl Grant {
    pub fn submit(
        &self,
        connection: &Connection,
        record: RecordRequest,
        trace: Value,
    ) -> Result<Value> {
        self.repository.execute(
            connection.transaction()?,
            Request::Submit { record },
            trace,
            Some(self.kinds.clone()),
        )
    }
    pub fn run(&self, transaction: &Transaction, request: Request, trace: Value) -> Result<Value> {
        let result = (|| {
            self.repository.execute(
                transaction.require_bound(true)?,
                request,
                trace,
                Some(self.kinds.clone()),
            )
        })();
        if result.is_err() {
            transaction.abort();
        }
        result
    }
}
fn host_only(kinds: Option<&BTreeSet<String>>) -> Result<()> {
    if kinds.is_some() {
        Err("Task operation is not granted".into())
    } else {
        Ok(())
    }
}
fn check_kind(kinds: Option<&BTreeSet<String>>, kind: &str) -> Result<()> {
    if kinds.is_some_and(|kinds| !kinds.contains(kind)) {
        Err(format!("Task kind is not granted: {kind}").into())
    } else {
        Ok(())
    }
}
fn text<'a>(value: &'a Value, name: &str) -> Result<&'a str> {
    value[name]
        .as_str()
        .ok_or_else(|| "Invalid task row".into())
}
fn object(value: &Value) -> Result<Map<String, Value>> {
    value
        .as_object()
        .cloned()
        .ok_or_else(|| "Invalid task object".into())
}
fn public(mut row: Value) -> Value {
    let row_map = row.as_object_mut().expect("decoded object");
    row_map.remove("payload");
    row_map.remove("token");
    row
}
fn decode(row: Vec<Value>) -> Result<Value> {
    if row.len() != NAMES.len() {
        return Err("Invalid task row".into());
    }
    let mut row: Map<String, Value> = NAMES
        .into_iter()
        .zip(row)
        .map(|(key, value)| (key.into(), value))
        .collect();
    for column in ["payload", "result"] {
        if !row[column].is_null() {
            row.insert(
                column.into(),
                serde_json::from_str(row[column].as_str().ok_or("Invalid task JSON")?)
                    .map_err(|_| "Invalid task JSON")?,
            );
        }
    }
    Ok(Value::Object(row))
}
fn query_one(
    session: &mut Session<'_>,
    condition: &str,
    params: Vec<Value>,
    lock: bool,
) -> Result<Option<Value>> {
    let lock = if lock && session.is_postgres() {
        " FOR UPDATE"
    } else {
        ""
    };
    session
        .execute(
            &format!("SELECT {COLUMNS} FROM jobs WHERE {condition}{lock}"),
            params,
        )?
        .into_iter()
        .next()
        .map(decode)
        .transpose()
}
fn snapshot(row: &Value) -> Result<Snapshot> {
    serde_json::from_value(json!({"id":row["id"],"state":row["state"],"attempt":row["attempt"],"token":row["token"],"lease_until":row["lease_until"],"result":row["result"]})).map_err(|_|"Invalid task snapshot".into())
}
fn predicate(value: &Predicate, params: &mut Vec<Value>) -> String {
    let column = |column: &super::Column| match column {
        super::Column::Id => "id",
        super::Column::State => "state",
        super::Column::Attempt => "attempt",
        super::Column::Token => "token",
        super::Column::LeaseUntil => "lease_until",
        super::Column::Kind => "kind",
    };
    match value {
        Predicate::Eq { column: c, value } if value.is_null() => format!("{} IS NULL", column(c)),
        Predicate::Eq { column: c, value } => {
            params.push(value.clone());
            format!("{}=${}", column(c), params.len())
        }
        Predicate::Le { column: c, value } => {
            params.push(json!(value));
            format!("{}<=${}", column(c), params.len())
        }
        Predicate::In { column: c, values } => {
            let slots: Vec<String> = values
                .iter()
                .map(|value| {
                    params.push(json!(value));
                    format!("${}", params.len())
                })
                .collect();
            format!("{} IN ({})", column(c), slots.join(","))
        }
        Predicate::Not { clause } => format!("NOT ({})", predicate(clause, params)),
        Predicate::And { clauses } | Predicate::Or { clauses } => format!(
            "({})",
            clauses
                .iter()
                .map(|clause| predicate(clause, params))
                .collect::<Vec<_>>()
                .join(if matches!(value, Predicate::And { .. }) {
                    " AND "
                } else {
                    " OR "
                })
        ),
    }
}
