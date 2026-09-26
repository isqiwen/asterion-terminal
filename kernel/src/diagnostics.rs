//! Best-effort execution observations and internal runtime events.
//!
//! This journal accepts identifiers and fixed failure categories, never messages,
//! exceptions, requests, responses or subprocess output. Business/plugin logs do
//! not extend these internal runtime-event enums. Their observations remain owned
//! by the relevant feature and use its own authorized contracts.
use crate::communication::parse_context;
use asterion_foundation::wire_generated::Context;
use rusqlite::{Connection, OpenFlags, TransactionBehavior, params};
use serde::Serialize;
use std::{
    fs::{self, OpenOptions},
    path::{Path, PathBuf},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use uuid::Uuid;

pub const MAX_ROWS: usize = 200;
pub const RECENT_ROWS: usize = 30;
pub const MAX_DATABASE_BYTES: u64 = 1_048_576;
const BUSY_TIMEOUT: Duration = Duration::from_secs(2);
const EXECUTIONS: &str = "CREATE TABLE executions (id TEXT PRIMARY KEY, digest TEXT NOT NULL, started REAL NOT NULL, duration_ms INTEGER NOT NULL, phase TEXT NOT NULL, code TEXT NOT NULL, calls INTEGER NOT NULL)";
const EVENTS: &str = "CREATE TABLE runtime_events (id TEXT PRIMARY KEY, started REAL NOT NULL, component TEXT NOT NULL, code TEXT NOT NULL, level TEXT NOT NULL, correlation_id TEXT, request_id TEXT)";
const UNAVAILABLE: &str = "Execution diagnostics are unavailable";
const UNSUPPORTED: &str = "Unsupported diagnostics database contract";
type Result<T> = std::result::Result<T, &'static str>;

#[derive(Serialize)]
pub struct Execution {
    pub id: String,
    pub digest: String,
    pub started: f64,
    pub duration_ms: i64,
    pub phase: String,
    pub code: String,
    pub calls: i64,
}

#[derive(Serialize)]
pub struct RuntimeEvent {
    pub id: String,
    pub started: f64,
    pub component: String,
    pub code: String,
    pub level: String,
    pub correlation_id: Option<String>,
    pub request_id: Option<String>,
}

// Closed categories for *internal* host events, not a business logging API.
#[derive(Clone, Copy)]
pub enum Component {
    Worker,
    Supervisor,
}
impl Component {
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "worker" => Some(Self::Worker),
            "supervisor" => Some(Self::Supervisor),
            _ => None,
        }
    }
    fn value(self) -> &'static str {
        match self {
            Self::Worker => "worker",
            Self::Supervisor => "supervisor",
        }
    }
}
#[derive(Clone, Copy)]
pub enum EventCode {
    ExecutionFailed,
    ControlPlaneUnavailable,
    SubprocessFailed,
}
impl EventCode {
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "execution_failed" => Some(Self::ExecutionFailed),
            "control_plane_unavailable" => Some(Self::ControlPlaneUnavailable),
            "subprocess_failed" => Some(Self::SubprocessFailed),
            _ => None,
        }
    }
    fn value(self) -> &'static str {
        match self {
            Self::ExecutionFailed => "execution_failed",
            Self::ControlPlaneUnavailable => "control_plane_unavailable",
            Self::SubprocessFailed => "subprocess_failed",
        }
    }
}

fn outcome(value: &str) -> bool {
    matches!(
        value,
        "success"
            | "startup"
            | "revoked"
            | "timeout"
            | "request_limit"
            | "output_limit"
            | "process_exit"
            | "protocol"
            | "host_error"
    )
}
fn hex(value: &str, length: usize) -> bool {
    value.len() == length
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn method(value: &str) -> bool {
    (1..=64).contains(&value.len())
        && value.as_bytes()[0].is_ascii_lowercase()
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'_' | b'.'))
}
fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0.0, |value| value.as_secs_f64())
}
fn valid_time(value: f64) -> bool {
    value.is_finite() && value >= 0.0
}

pub struct Observation {
    root: PathBuf,
    digest: Option<String>,
    started: f64,
    clock: Instant,
    phase: String,
    calls: i64,
    finished: bool,
}
impl Observation {
    pub fn new(root: &Path, digest: &str) -> Self {
        Self {
            root: root.into(),
            digest: hex(digest, 64).then(|| digest.to_owned()),
            started: now(),
            clock: Instant::now(),
            phase: "startup".into(),
            calls: 0,
            finished: false,
        }
    }
    pub fn call(&mut self, name: &str) {
        if self.finished {
            return;
        }
        self.phase = if method(name) { name } else { "request" }.to_owned();
        self.calls = self.calls.saturating_add(1);
    }
    pub fn finish(&mut self, code: &str) {
        if self.finished {
            return;
        }
        self.finished = true;
        let Some(digest) = &self.digest else {
            return;
        };
        // Unsupported categories are not rewritten as another outcome. Logging
        // rejection is intentionally non-throwing at the execution boundary.
        if !outcome(code) {
            return;
        }
        let duration = (self.clock.elapsed().as_secs_f64() * 1000.0).round_ties_even();
        let record = Execution {
            id: Uuid::new_v4().simple().to_string(),
            digest: digest.clone(),
            started: self.started,
            duration_ms: duration.min(i64::MAX as f64) as i64,
            phase: self.phase.clone(),
            code: code.to_owned(),
            calls: self.calls,
        };
        let _ = persist_execution(&self.root, &record);
    }
}

fn path(root: &Path) -> PathBuf {
    root.join("diagnostics.sqlite")
}
fn open(root: &Path, write: bool) -> Result<Option<Connection>> {
    let file = path(root);
    if write {
        let mut builder = fs::DirBuilder::new();
        builder.recursive(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            builder.mode(0o700);
        }
        builder.create(root).map_err(|_| UNAVAILABLE)?;
        let mut options = OpenOptions::new();
        options.read(true).write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        match options.open(&file) {
            Ok(_) => (),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => (),
            Err(_) => return Err(UNAVAILABLE),
        }
    }
    let metadata = match fs::symlink_metadata(&file) {
        Ok(value) => value,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(UNAVAILABLE),
    };
    if !metadata.is_file() || metadata.len() > MAX_DATABASE_BYTES {
        return Err(UNSUPPORTED);
    }
    let flags = if write {
        OpenFlags::SQLITE_OPEN_READ_WRITE
    } else {
        OpenFlags::SQLITE_OPEN_READ_ONLY
    };
    let conn = Connection::open_with_flags(file, flags).map_err(|_| UNAVAILABLE)?;
    conn.busy_timeout(BUSY_TIMEOUT).map_err(|_| UNAVAILABLE)?;
    Ok(Some(conn))
}

type Column = (String, String, bool, bool, Option<String>, i64);
fn columns(conn: &Connection, table: &str) -> Result<Vec<Column>> {
    let mut query = conn
        .prepare(&format!("PRAGMA table_xinfo({table})"))
        .map_err(|_| UNSUPPORTED)?;
    let rows = query
        .query_map([], |row| {
            Ok((
                row.get(1)?,
                row.get(2)?,
                row.get(3)?,
                row.get(5)?,
                row.get(4)?,
                row.get(6)?,
            ))
        })
        .map_err(|_| UNSUPPORTED)?;
    rows.collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|_| UNSUPPORTED)
}
fn expected(fields: &[(&str, &str)]) -> Vec<Column> {
    fields
        .iter()
        .map(|(name, kind)| {
            (
                name.to_string(),
                kind.to_string(),
                !matches!(*name, "id" | "correlation_id" | "request_id"),
                *name == "id",
                None,
                0,
            )
        })
        .collect()
}
fn validate(conn: &Connection) -> Result<(bool, bool)> {
    let objects = conn
        .prepare("SELECT type,name,sql FROM sqlite_master")
        .map_err(|_| UNSUPPORTED)?
        .query_map([], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, Option<String>>(2)?,
            ))
        })
        .map_err(|_| UNSUPPORTED)?
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|_| UNSUPPORTED)?;
    for (kind, name, sql) in &objects {
        let valid_table =
            kind == "table" && matches!(name.as_str(), "executions" | "runtime_events");
        let valid_index = kind == "index"
            && sql.is_none()
            && matches!(
                name.as_str(),
                "sqlite_autoindex_executions_1" | "sqlite_autoindex_runtime_events_1"
            );
        if !valid_table && !valid_index {
            return Err(UNSUPPORTED);
        }
    }
    let execution = objects
        .iter()
        .any(|(kind, name, _)| kind == "table" && name == "executions");
    let event = objects
        .iter()
        .any(|(kind, name, _)| kind == "table" && name == "runtime_events");
    if execution {
        if columns(conn, "executions")?
            != expected(&[
                ("id", "TEXT"),
                ("digest", "TEXT"),
                ("started", "REAL"),
                ("duration_ms", "INTEGER"),
                ("phase", "TEXT"),
                ("code", "TEXT"),
                ("calls", "INTEGER"),
            ])
        {
            return Err(UNSUPPORTED);
        }
        let rows = read_executions(conn, None, MAX_ROWS + 1)?;
        if rows.len() > MAX_ROWS
            || rows.iter().any(|row| {
                !hex(&row.id, 32)
                    || !hex(&row.digest, 64)
                    || !valid_time(row.started)
                    || row.duration_ms < 0
                    || !method(&row.phase)
                    || !outcome(&row.code)
                    || row.calls < 0
            })
        {
            return Err(UNSUPPORTED);
        }
    }
    if event {
        if columns(conn, "runtime_events")?
            != expected(&[
                ("id", "TEXT"),
                ("started", "REAL"),
                ("component", "TEXT"),
                ("code", "TEXT"),
                ("level", "TEXT"),
                ("correlation_id", "TEXT"),
                ("request_id", "TEXT"),
            ])
        {
            return Err(UNSUPPORTED);
        }
        let rows = read_events(conn, MAX_ROWS + 1)?;
        if rows.len() > MAX_ROWS
            || rows.iter().any(|row| {
                !hex(&row.id, 32)
                    || !valid_time(row.started)
                    || Component::parse(&row.component).is_none()
                    || EventCode::parse(&row.code).is_none()
                    || row.level != "error"
                    // Communication identifiers are 32 Unicode characters;
                    // only this journal's generated record IDs are hex UUIDs.
                    || row
                        .correlation_id
                        .as_ref()
                        .is_some_and(|id| id.chars().count() != 32)
                    || row
                        .request_id
                        .as_ref()
                        .is_some_and(|id| id.chars().count() != 32)
                    || row.correlation_id.is_some() != row.request_id.is_some()
            })
        {
            return Err(UNSUPPORTED);
        }
    }
    Ok((execution, event))
}
fn capacity(conn: &Connection) -> Result<()> {
    let page_size: i64 = conn
        .query_row("PRAGMA page_size", [], |row| row.get(0))
        .map_err(|_| UNSUPPORTED)?;
    if page_size <= 0 {
        return Err(UNSUPPORTED);
    }
    let pages = MAX_DATABASE_BYTES as i64 / page_size;
    let accepted: i64 = conn
        .query_row(&format!("PRAGMA max_page_count={pages}"), [], |row| {
            row.get(0)
        })
        .map_err(|_| UNAVAILABLE)?;
    if accepted > pages {
        return Err(UNSUPPORTED);
    }
    Ok(())
}
fn persist_execution(root: &Path, record: &Execution) -> Result<()> {
    let mut conn = open(root, true)?.ok_or(UNAVAILABLE)?;
    let transaction = conn
        .transaction_with_behavior(TransactionBehavior::Immediate)
        .map_err(|_| UNAVAILABLE)?;
    let (exists, _) = validate(&transaction)?;
    capacity(&transaction)?;
    if !exists {
        transaction
            .execute_batch(EXECUTIONS)
            .map_err(|_| UNAVAILABLE)?;
    }
    transaction
        .execute(
            "INSERT INTO executions VALUES (?1,?2,?3,?4,?5,?6,?7)",
            params![
                record.id,
                record.digest,
                record.started,
                record.duration_ms,
                record.phase,
                record.code,
                record.calls
            ],
        )
        .map_err(|_| UNAVAILABLE)?;
    transaction.execute("DELETE FROM executions WHERE id NOT IN (SELECT id FROM executions ORDER BY started DESC,id DESC LIMIT ?1)",[MAX_ROWS as i64]).map_err(|_| UNAVAILABLE)?;
    transaction.commit().map_err(|_| UNAVAILABLE)
}
fn read_executions(
    conn: &Connection,
    digest: Option<&str>,
    limit: usize,
) -> Result<Vec<Execution>> {
    let mut query = conn.prepare("SELECT id,digest,started,duration_ms,phase,code,calls FROM executions WHERE (?1 IS NULL OR digest=?1) ORDER BY started DESC,id DESC LIMIT ?2").map_err(|_| UNSUPPORTED)?;
    query
        .query_map(params![digest, limit as i64], |row| {
            Ok(Execution {
                id: row.get(0)?,
                digest: row.get(1)?,
                started: row.get(2)?,
                duration_ms: row.get(3)?,
                phase: row.get(4)?,
                code: row.get(5)?,
                calls: row.get(6)?,
            })
        })
        .map_err(|_| UNSUPPORTED)?
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|_| UNSUPPORTED)
}
pub fn recent(root: &Path, digest: &str) -> Result<Vec<Execution>> {
    if !hex(digest, 64) {
        return Err("Invalid diagnostics artifact digest");
    }
    let Some(mut conn) = open(root, false)? else {
        return Ok(vec![]);
    };
    let transaction = conn.transaction().map_err(|_| UNAVAILABLE)?;
    let (exists, _) = validate(&transaction)?;
    if exists {
        read_executions(&transaction, Some(digest), RECENT_ROWS)
    } else {
        Ok(vec![])
    }
}
pub fn record_event(root: &Path, component: Component, code: EventCode, context: Option<&Context>) {
    // Expired but structurally valid context remains useful when recording a
    // timeout. This does not authorize work or extend a communication deadline.
    if context.is_some_and(|trace| parse_context(serde_json::json!(trace)).is_err()) {
        return;
    }
    let _ = persist_event(root, component, code, context);
}
fn persist_event(
    root: &Path,
    component: Component,
    code: EventCode,
    context: Option<&Context>,
) -> Result<()> {
    let mut conn = open(root, true)?.ok_or(UNAVAILABLE)?;
    let transaction = conn
        .transaction_with_behavior(TransactionBehavior::Immediate)
        .map_err(|_| UNAVAILABLE)?;
    let (_, exists) = validate(&transaction)?;
    capacity(&transaction)?;
    if !exists {
        transaction.execute_batch(EVENTS).map_err(|_| UNAVAILABLE)?;
    }
    transaction
        .execute(
            "INSERT INTO runtime_events VALUES (?1,?2,?3,?4,?5,?6,?7)",
            params![
                Uuid::new_v4().simple().to_string(),
                now(),
                component.value(),
                code.value(),
                "error",
                context.map(|trace| &trace.correlation_id),
                context.map(|trace| &trace.request_id)
            ],
        )
        .map_err(|_| UNAVAILABLE)?;
    transaction.execute("DELETE FROM runtime_events WHERE id NOT IN (SELECT id FROM runtime_events ORDER BY started DESC,id DESC LIMIT ?1)",[MAX_ROWS as i64]).map_err(|_| UNAVAILABLE)?;
    transaction.commit().map_err(|_| UNAVAILABLE)
}
fn read_events(conn: &Connection, limit: usize) -> Result<Vec<RuntimeEvent>> {
    let mut query = conn.prepare("SELECT id,started,component,code,level,correlation_id,request_id FROM runtime_events ORDER BY started DESC,id DESC LIMIT ?1").map_err(|_| UNSUPPORTED)?;
    query
        .query_map([limit as i64], |row| {
            Ok(RuntimeEvent {
                id: row.get(0)?,
                started: row.get(1)?,
                component: row.get(2)?,
                code: row.get(3)?,
                level: row.get(4)?,
                correlation_id: row.get(5)?,
                request_id: row.get(6)?,
            })
        })
        .map_err(|_| UNSUPPORTED)?
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|_| UNSUPPORTED)
}
pub fn events(root: &Path) -> Result<Vec<RuntimeEvent>> {
    let Some(mut conn) = open(root, false)? else {
        return Ok(vec![]);
    };
    let transaction = conn.transaction().map_err(|_| UNAVAILABLE)?;
    let (_, exists) = validate(&transaction)?;
    if exists {
        read_events(&transaction, RECENT_ROWS)
    } else {
        Ok(vec![])
    }
}
