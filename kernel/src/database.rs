//! Fixed physical database connections, transactions and bounded cursors.
//!
//! SQL construction belongs to the language adapter. The driver owns the physical
//! connection on one actor thread. A SQLite streaming cursor is exclusive until
//! consumed or closed; an overlapping statement fails instead of deadlocking.
use bytes::BytesMut;
use postgres::types::{IsNull, ToSql, Type};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, VecDeque};
use std::sync::{
    Arc, Mutex, Weak,
    atomic::{AtomicBool, AtomicU64, Ordering},
    mpsc,
};
use std::time::Duration;

static DATABASE_IDS: AtomicU64 = AtomicU64::new(1);

const ROW_LIMIT: usize = 100_000;
const BYTE_LIMIT: usize = 64 * 1024 * 1024;
const FETCH_LIMIT: usize = 1000;
const SQL_LIMIT: usize = 4 * 1024 * 1024;

#[derive(Debug, Clone)]
pub struct Error {
    pub code: &'static str,
    pub message: &'static str,
}
impl Error {
    fn rollback_required() -> Self {
        Self {
            code: "operational",
            message: "Database transaction requires rollback",
        }
    }
    fn new(code: &'static str) -> Self {
        Self {
            code,
            message: match code {
                "integrity" => "Database constraint violation",
                "operational" => "Database operation failed",
                "programming" => "Unsupported or invalid database operation",
                "data" => "Database value or result exceeds its contract",
                "busy" => "Database connection has an active streaming cursor",
                _ => "Database connection or cursor is closed",
            },
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message)
    }
}
impl std::error::Error for Error {}
impl From<rusqlite::Error> for Error {
    fn from(value: rusqlite::Error) -> Self {
        use rusqlite::{Error as E, ErrorCode as C};
        let code = match value {
            E::SqliteFailure(e, _) => match e.code {
                C::ConstraintViolation => "integrity",
                C::TypeMismatch | C::TooBig => "data",
                C::Unknown | C::ApiMisuse => "programming",
                _ => "operational",
            },
            E::InvalidQuery
            | E::MultipleStatement
            | E::InvalidParameterCount(..)
            | E::InvalidParameterName(..) => "programming",
            _ => "data",
        };
        Self::new(code)
    }
}
impl From<postgres::Error> for Error {
    fn from(value: postgres::Error) -> Self {
        let mut source = std::error::Error::source(&value);
        while let Some(cause) = source {
            if let Some(error) = cause.downcast_ref::<Error>() {
                return error.clone();
            }
            source = cause.source();
        }
        let code = match value.code().map(|code| &code.code()[..2]) {
            Some("23") => "integrity",
            Some("22") => "data",
            Some("42" | "0A") => "programming",
            _ => "operational",
        };
        Self::new(code)
    }
}
pub type Result<T> = std::result::Result<T, Error>;
fn timeout() -> u64 {
    5000
}
fn statement_timeout() -> u64 {
    30_000
}
#[derive(Deserialize)]
#[serde(tag = "backend", rename_all = "lowercase", deny_unknown_fields)]
enum Config {
    Sqlite {
        path: String,
        #[serde(default = "timeout")]
        timeout_ms: u64,
    },
    Postgresql {
        host: String,
        port: u16,
        database: String,
        user: String,
        #[serde(default)]
        password: String,
        #[serde(default)]
        options: String,
        #[serde(default = "timeout")]
        connect_timeout_ms: u64,
        #[serde(default = "statement_timeout")]
        statement_timeout_ms: u64,
    },
}

fn percent_decode(value: &str) -> std::result::Result<String, String> {
    let bytes = value.as_bytes();
    let mut output = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' {
            let hex = value
                .get(index + 1..index + 3)
                .ok_or("Invalid percent-encoding in database URL")?;
            output.push(
                u8::from_str_radix(hex, 16)
                    .map_err(|_| "Invalid percent-encoding in database URL")?,
            );
            index += 3;
        } else {
            output.push(bytes[index]);
            index += 1;
        }
    }
    String::from_utf8(output).map_err(|_| "Database URL credentials must be UTF-8".into())
}

/// The native connection configuration of a `sqlite://` or `postgresql://`
/// URL, as the JSON accepted by `Database::new`. Query parameters are not
/// supported; timeouts and options take their defaults.
pub fn config_from_url(url: &str) -> std::result::Result<serde_json::Value, String> {
    let (scheme, rest) = url
        .split_once("://")
        .ok_or("Database URL must use postgresql:// or sqlite://")?;
    if rest.contains('?') || rest.contains('#') {
        return Err("Database URL query parameters are not supported".into());
    }
    match scheme {
        "sqlite" | "sqlite+asterion" => {
            let path = match rest {
                "" => ":memory:",
                _ => rest
                    .strip_prefix('/')
                    .filter(|path| !path.is_empty())
                    .ok_or("SQLite URL must be sqlite:// or sqlite:///path")?,
            };
            Ok(serde_json::json!({"backend": "sqlite", "path": path, "timeout_ms": timeout()}))
        }
        "postgresql" | "postgresql+asterion" => {
            let (authority, database) = rest.split_once('/').unwrap_or((rest, ""));
            let (credentials, location) = authority.rsplit_once('@').unwrap_or(("", authority));
            let (user, password) = credentials.split_once(':').unwrap_or((credentials, ""));
            let (host, port) = match location.rsplit_once(':') {
                Some((host, port)) => (
                    host,
                    port.parse::<u16>()
                        .map_err(|_| "Invalid database port in URL")?,
                ),
                None => (location, 5432),
            };
            if host.contains(['/', '[', ']']) {
                return Err("Unsupported database host in URL".into());
            }
            Ok(serde_json::json!({
                "backend": "postgresql",
                "host": if host.is_empty() { "127.0.0.1" } else { host },
                "port": port,
                "database": if database.is_empty() { "postgres".into() } else { percent_decode(database)? },
                "user": percent_decode(user)?,
                "password": percent_decode(password)?,
                "options": "",
                "connect_timeout_ms": timeout(),
                "statement_timeout_ms": statement_timeout(),
            }))
        }
        _ => Err("Unsupported database driver; use postgresql:// or sqlite://".into()),
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct Column {
    pub name: String,
    pub kind: String,
    pub nullable: bool,
}
#[derive(Debug, Clone, Serialize)]
pub struct Foreign {
    pub columns: Vec<String>,
    pub table: String,
    pub references: Vec<String>,
}
#[derive(Debug, Clone, Serialize)]
pub struct Index {
    pub name: String,
    pub columns: Vec<String>,
    pub unique: bool,
}
#[derive(Debug, Clone, Serialize)]
pub struct Schema {
    pub name: String,
    pub columns: Vec<Column>,
    pub primary: Vec<String>,
    pub unique: Vec<Vec<String>>,
    pub foreign: Vec<Foreign>,
    pub indexes: Vec<Index>,
}
#[derive(Clone, Serialize)]
pub struct Description {
    pub name: String,
    pub kind: String,
}
struct Execution {
    id: u64,
    description: Vec<Description>,
    rows: VecDeque<Vec<Value>>,
    rowcount: i64,
    lastrowid: Option<i64>,
    streaming: bool,
}
struct Batch {
    rows: Vec<Vec<Value>>,
    done: bool,
}
enum Response {
    Unit,
    Ticket(u64),
    Operation(OperationResult),
    Flag(bool),
    Execute(Execution),
    Batch(Batch),
    Names(Vec<String>),
    Schema(Vec<Schema>),
}
enum Command {
    Begin,
    BeginWrite,
    Ticket {
        generation: u64,
    },
    Operation {
        generation: u64,
        transaction: u64,
        operation: Operation,
    },
    Execute {
        sql: String,
        params: Vec<Value>,
        stream: bool,
    },
    Batch {
        sql: String,
        params: Vec<Vec<Value>>,
    },
    Fetch {
        id: u64,
        size: usize,
    },
    Release(u64),
    Commit,
    Rollback,
    Autocommit(Option<bool>),
    Names,
    Observe,
    Close,
}
struct Request {
    command: Command,
    reply: mpsc::SyncSender<Result<Response>>,
}
struct Handle {
    sender: mpsc::SyncSender<Request>,
    closed: AtomicBool,
    generation: Arc<AtomicU64>,
    database: u64,
    postgres: bool,
}
impl Handle {
    fn request(&self, command: Command) -> Result<Response> {
        if self.closed.load(Ordering::Acquire) {
            return Err(Error::new("closed"));
        }
        let (reply, answer) = mpsc::sync_channel(1);
        self.sender
            .send(Request { command, reply })
            .map_err(|_| Error::new("closed"))?;
        answer.recv().map_err(|_| Error::new("closed"))?
    }
}
impl Drop for Handle {
    fn drop(&mut self) {
        let (reply, _) = mpsc::sync_channel(1);
        let _ = self.sender.try_send(Request {
            command: Command::Close,
            reply,
        });
    }
}
#[derive(Clone)]
pub struct Connection {
    handle: Arc<Handle>,
}
impl Connection {
    pub fn open(raw: &str) -> Result<Self> {
        Self::open_for(raw, DATABASE_IDS.fetch_add(1, Ordering::Relaxed))
    }
    fn open_for(raw: &str, database: u64) -> Result<Self> {
        if raw.len() > 16_384 {
            return Err(Error::new("programming"));
        }
        let config: Config = serde_json::from_str(raw).map_err(|_| Error::new("programming"))?;
        let postgres = matches!(config, Config::Postgresql { .. });
        let (sender, receiver) = mpsc::sync_channel(32);
        let (ready, opened) = mpsc::sync_channel(1);
        let generation = Arc::new(AtomicU64::new(0));
        let actor_generation = generation.clone();
        std::thread::Builder::new()
            .name("asterion-database".into())
            .spawn(move || {
                let backend = Backend::open(config);
                match backend {
                    Ok(backend) => {
                        let _ = ready.send(Ok(()));
                        actor(backend, receiver, actor_generation);
                    }
                    Err(error) => {
                        let _ = ready.send(Err(error));
                    }
                }
            })
            .map_err(|_| Error::new("operational"))?;
        opened.recv().map_err(|_| Error::new("operational"))??;
        Ok(Self {
            handle: Arc::new(Handle {
                sender,
                closed: AtomicBool::new(false),
                generation,
                database,
                postgres,
            }),
        })
    }
    fn unit(&self, command: Command) -> Result<()> {
        match self.handle.request(command)? {
            Response::Unit => Ok(()),
            _ => Err(Error::new("programming")),
        }
    }
    pub fn database_id(&self) -> u64 {
        self.handle.database
    }
    /// Whether statements use PostgreSQL (`$n`) rather than SQLite (`?`)
    /// placeholders; for callers that share this connection's transaction.
    pub fn is_postgres(&self) -> bool {
        self.handle.postgres
    }
    pub fn transaction(&self) -> Result<TransactionRef> {
        let generation = self.handle.generation.load(Ordering::Acquire);
        let Response::Ticket(transaction) = self.handle.request(Command::Ticket { generation })?
        else {
            return Err(Error::new("programming"));
        };
        Ok(TransactionRef {
            handle: Arc::downgrade(&self.handle),
            database: self.handle.database,
            generation,
            transaction,
        })
    }
    pub fn begin_write(&self) -> Result<()> {
        self.unit(Command::BeginWrite)
    }
    pub fn begin(&self) -> Result<()> {
        self.unit(Command::Begin)
    }
    pub fn commit(&self) -> Result<()> {
        self.unit(Command::Commit)
    }
    pub fn rollback(&self) -> Result<()> {
        self.unit(Command::Rollback)
    }
    pub fn close(&self) -> Result<()> {
        if self.handle.closed.load(Ordering::Acquire) {
            return Ok(());
        }
        let result = self.unit(Command::Close);
        self.handle.closed.store(true, Ordering::Release);
        result
    }
    pub fn autocommit(&self) -> Result<bool> {
        match self.handle.request(Command::Autocommit(None))? {
            Response::Flag(value) => Ok(value),
            _ => Err(Error::new("programming")),
        }
    }
    pub fn set_autocommit(&self, value: bool) -> Result<()> {
        self.unit(Command::Autocommit(Some(value)))
    }
    pub fn execute(&self, sql: &str, params: Vec<Value>, stream: bool) -> Result<Cursor> {
        statement(sql)?;
        parameters(&params)?;
        let Response::Execute(result) = self.handle.request(Command::Execute {
            sql: sql.into(),
            params,
            stream,
        })?
        else {
            return Err(Error::new("programming"));
        };
        Ok(Cursor {
            handle: self.handle.clone(),
            id: result.id,
            generation: self.handle.generation.load(Ordering::Acquire),
            description: result.description,
            rowcount: result.rowcount,
            lastrowid: result.lastrowid,
            state: Mutex::new(CursorState {
                rows: result.rows,
                streaming: result.streaming,
                closed: false,
                owner: None,
            }),
        })
    }
    pub fn executemany(&self, sql: &str, params: Vec<Vec<Value>>) -> Result<Cursor> {
        statement(sql)?;
        if params.len() > ROW_LIMIT || params.iter().any(|row| row.len() > 32_767) {
            return Err(Error::new("programming"));
        }
        parameter_budget(&params)?;
        let Response::Execute(result) = self.handle.request(Command::Batch {
            sql: sql.into(),
            params,
        })?
        else {
            return Err(Error::new("programming"));
        };
        Ok(Cursor {
            handle: self.handle.clone(),
            id: result.id,
            generation: self.handle.generation.load(Ordering::Acquire),
            description: result.description,
            rowcount: result.rowcount,
            lastrowid: result.lastrowid,
            state: Mutex::new(CursorState {
                rows: result.rows,
                streaming: false,
                closed: false,
                owner: None,
            }),
        })
    }
    pub fn table_names(&self) -> Result<Vec<String>> {
        match self.handle.request(Command::Names)? {
            Response::Names(names) => Ok(names),
            _ => Err(Error::new("programming")),
        }
    }
    pub fn observe_schema(&self) -> Result<Vec<Schema>> {
        match self.handle.request(Command::Observe)? {
            Response::Schema(schema) => Ok(schema),
            _ => Err(Error::new("programming")),
        }
    }
}
struct CursorState {
    rows: VecDeque<Vec<Value>>,
    streaming: bool,
    closed: bool,
    owner: Option<Arc<crate::storage::Transaction>>,
}
pub struct Cursor {
    handle: Arc<Handle>,
    id: u64,
    generation: u64,
    pub description: Vec<Description>,
    pub rowcount: i64,
    pub lastrowid: Option<i64>,
    state: Mutex<CursorState>,
}
impl Cursor {
    /// Bind once to the exact physical transaction and its scoped owner. A
    /// language adapter cannot substitute another database's or a later lease.
    pub fn guard(&self, owner: Arc<crate::storage::Transaction>) -> Result<()> {
        let mut state = self.state.lock().map_err(|_| Error::new("closed"))?;
        let result = (|| {
            if state.closed || state.owner.is_some() {
                return Err(Error::new("programming"));
            }
            let ticket = owner.require_bound(false).map_err(|message| Error {
                code: "closed",
                message,
            })?;
            let connection = ticket
                .handle
                .upgrade()
                .ok_or_else(|| Error::new("closed"))?;
            if !Arc::ptr_eq(&connection, &self.handle) || ticket.generation != self.generation {
                return Err(Error::new("closed"));
            }
            state.owner = Some(owner.clone());
            Ok(())
        })();
        if result.is_err() {
            owner.abort();
            if let Some(previous) = &state.owner {
                previous.abort();
            }
            let _ = self.release(&mut state);
        }
        result
    }
    fn check(&self, state: &CursorState) -> Result<()> {
        if state.closed
            || self.handle.closed.load(Ordering::Acquire)
            || self.generation != self.handle.generation.load(Ordering::Acquire)
        {
            return Err(Error::new("closed"));
        }
        if let Some(owner) = &state.owner {
            owner.check_bound().map_err(|message| Error {
                code: "closed",
                message,
            })?;
        }
        Ok(())
    }
    pub fn fetchmany(&self, size: usize) -> Result<Vec<Vec<Value>>> {
        let mut state = self.state.lock().map_err(|_| Error::new("closed"))?;
        let result = (|| {
            self.check(&state)?;
            if size == 0 || size > FETCH_LIMIT {
                return Err(Error::new("programming"));
            }
            let rows = if !state.streaming {
                let count = size.min(state.rows.len());
                state.rows.drain(..count).collect()
            } else {
                match self.handle.request(Command::Fetch { id: self.id, size })? {
                    Response::Batch(batch) => {
                        state.streaming = !batch.done;
                        batch.rows
                    }
                    _ => return Err(Error::new("programming")),
                }
            };
            self.check(&state)?;
            Ok(rows)
        })();
        if result.is_err() {
            if let Some(owner) = &state.owner {
                owner.abort();
            }
            let _ = self.release(&mut state);
        }
        result
    }
    pub fn close(&self) -> Result<()> {
        let mut state = self.state.lock().map_err(|_| Error::new("closed"))?;
        self.release(&mut state)
    }
    fn release(&self, state: &mut CursorState) -> Result<()> {
        if state.closed {
            return Ok(());
        }
        let result = if state.streaming
            && !self.handle.closed.load(Ordering::Acquire)
            && self.generation == self.handle.generation.load(Ordering::Acquire)
        {
            self.handle.request(Command::Release(self.id)).map(|_| ())
        } else {
            Ok(())
        };
        state.rows.clear();
        state.streaming = false;
        state.closed = true;
        result
    }
}
impl Drop for Cursor {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

enum Backend {
    Sqlite(rusqlite::Connection),
    Postgres(postgres::Client),
}
impl Backend {
    fn open(config: Config) -> Result<Self> {
        match config {
            Config::Sqlite { path, timeout_ms } => {
                if path.is_empty() || timeout_ms == 0 || timeout_ms > 300_000 {
                    return Err(Error::new("programming"));
                }
                let conn = rusqlite::Connection::open(path)?;
                conn.busy_timeout(Duration::from_millis(timeout_ms))?;
                conn.execute_batch("PRAGMA foreign_keys=ON")?;
                Ok(Self::Sqlite(conn))
            }
            Config::Postgresql {
                host,
                port,
                database,
                user,
                password,
                options,
                connect_timeout_ms,
                statement_timeout_ms,
            } => {
                if !matches!(host.as_str(), "localhost" | "127.0.0.1" | "::1")
                    || port == 0
                    || user.is_empty()
                    || database.is_empty()
                    || connect_timeout_ms == 0
                    || connect_timeout_ms > 300_000
                    || statement_timeout_ms == 0
                    || statement_timeout_ms > 3_600_000
                {
                    return Err(Error::new("programming"));
                }
                let mut config = postgres::Config::new();
                config
                    .host(&host)
                    .port(port)
                    .dbname(&database)
                    .user(&user)
                    .password(&password)
                    .connect_timeout(Duration::from_millis(connect_timeout_ms));
                if !options.is_empty() {
                    config.options(&options);
                }
                let mut conn = config.connect(postgres::NoTls)?;
                conn.batch_execute(&format!("SET statement_timeout={statement_timeout_ms}"))?;
                Ok(Self::Postgres(conn))
            }
        }
    }
    fn control(&mut self, sql: &str) -> Result<()> {
        match self {
            Self::Sqlite(conn) => conn.execute_batch(sql)?,
            Self::Postgres(conn) => conn.batch_execute(sql)?,
        }
        Ok(())
    }
    fn execute(
        &mut self,
        id: u64,
        sql: &str,
        params: Vec<Value>,
        stream: bool,
    ) -> Result<Execution> {
        match self {
            Self::Sqlite(conn) => sqlite_execute(conn, id, sql, params),
            Self::Postgres(conn) => postgres_execute(conn, id, sql, params, stream),
        }
    }
    fn table_names(&mut self) -> Result<Vec<String>> {
        match self {
            Self::Sqlite(conn) => {
                let mut statement = conn.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")?;
                let rows = statement.query_map([], |row| row.get(0))?;
                rows.collect::<std::result::Result<_, _>>().map_err(Into::into)
            }
            Self::Postgres(conn) => Ok(conn.query("SELECT tablename::text FROM pg_catalog.pg_tables WHERE schemaname=current_schema() ORDER BY tablename", &[])?
                .iter().map(|row| row.get(0)).collect()),
        }
    }
    fn observe(&mut self) -> Result<Vec<Schema>> {
        let names = self.table_names()?;
        names
            .into_iter()
            .map(|name| match self {
                Self::Sqlite(conn) => sqlite_schema(conn, name),
                Self::Postgres(conn) => postgres_schema(conn, name),
            })
            .collect()
    }
}
fn actor(mut backend: Backend, receiver: mpsc::Receiver<Request>, generation: Arc<AtomicU64>) {
    let mut transaction = 0_u64;
    let mut active = false;
    let mut rollback_only = false;
    let mut autocommit = false;
    let mut counter = 0_u64;
    let mut pending = None;
    let mut streams = BTreeMap::<u64, Vec<Description>>::new();
    loop {
        let request = match pending.take().map(Ok).unwrap_or_else(|| receiver.recv()) {
            Ok(request) => request,
            Err(_) => break,
        };
        let Request { command, reply } = request;
        let executes = matches!(
            &command,
            Command::Execute { .. }
                | Command::Batch { .. }
                | Command::Fetch { .. }
                | Command::Names
                | Command::Observe
                | Command::Operation { .. }
        );
        if rollback_only && executes {
            let _ = reply.send(Err(Error::rollback_required()));
            continue;
        }
        let result = match command {
            Command::Ticket {
                generation: expected,
            } => {
                if active && !rollback_only && expected == generation.load(Ordering::Acquire) {
                    Ok(Response::Ticket(transaction))
                } else {
                    Err(Error::new("closed"))
                }
            }
            Command::Operation {
                generation: expected,
                transaction: epoch,
                operation,
            } => {
                if !active || epoch != transaction || expected != generation.load(Ordering::Acquire)
                {
                    Err(Error::new("closed"))
                } else {
                    let result = operation(&mut Session {
                        backend: &mut backend,
                        counter: &mut counter,
                    });
                    if result.is_err() {
                        rollback_only = true;
                    }
                    Ok(Response::Operation(result))
                }
            }
            Command::Begin | Command::BeginWrite => {
                if active || autocommit {
                    Ok(Response::Unit)
                } else {
                    let sql = if matches!(command, Command::BeginWrite)
                        && matches!(backend, Backend::Sqlite(_))
                    {
                        "BEGIN IMMEDIATE"
                    } else {
                        "BEGIN"
                    };
                    backend.control(sql).map(|()| {
                        active = true;
                        transaction += 1;
                        Response::Unit
                    })
                }
            }
            Command::Autocommit(value) => match value {
                None => Ok(Response::Flag(autocommit)),
                Some(_) if active => Err(Error::new("programming")),
                Some(value) => {
                    autocommit = value;
                    Ok(Response::Unit)
                }
            },
            Command::Commit | Command::Rollback => {
                let commit = matches!(command, Command::Commit);
                let result = if active && commit && rollback_only {
                    Err(Error::rollback_required())
                } else if active {
                    backend.control(if commit { "COMMIT" } else { "ROLLBACK" })
                } else {
                    Ok(())
                };
                if result.is_ok() {
                    active = false;
                    rollback_only = false;
                    streams.clear();
                    generation.fetch_add(1, Ordering::AcqRel);
                }
                result.map(|()| Response::Unit)
            }
            Command::Names => backend.table_names().map(Response::Names),
            Command::Observe => backend.observe().map(Response::Schema),
            Command::Execute {
                sql,
                params,
                stream,
            } => {
                if !active && !autocommit {
                    match backend.control("BEGIN") {
                        Ok(()) => {
                            active = true;
                            transaction += 1;
                        }
                        Err(error) => {
                            let _ = reply.send(Err(error));
                            continue;
                        }
                    }
                }
                counter += 1;
                if stream && autocommit {
                    Err(Error::new("programming"))
                } else if stream && let Backend::Sqlite(conn) = &backend {
                    pending = sqlite_stream(
                        conn,
                        counter,
                        &sql,
                        params,
                        StreamControl {
                            receiver: &receiver,
                            reply,
                            rollback_only: &mut rollback_only,
                            generation: &generation,
                            transaction,
                        },
                    );
                    continue;
                } else {
                    backend
                        .execute(counter, &sql, params, stream)
                        .map(|execution| {
                            if execution.streaming {
                                streams.insert(counter, execution.description.clone());
                            }
                            Response::Execute(execution)
                        })
                }
            }
            Command::Batch { sql, params } => {
                if autocommit {
                    Err(Error::new("programming"))
                } else {
                    if !active {
                        match backend.control("BEGIN") {
                            Ok(()) => {
                                active = true;
                                transaction += 1;
                            }
                            Err(error) => {
                                let _ = reply.send(Err(error));
                                continue;
                            }
                        }
                    }
                    counter += 1;
                    execute_many(&mut backend, counter, &sql, params).map(Response::Execute)
                }
            }
            Command::Fetch { id, size } => {
                if let Some(description) = streams.get(&id) {
                    if let Backend::Postgres(conn) = &mut backend {
                        postgres_fetch(conn, id, description, size).map(|batch| {
                            if batch.done {
                                streams.remove(&id);
                            }
                            Response::Batch(batch)
                        })
                    } else {
                        Err(Error::new("closed"))
                    }
                } else {
                    Err(Error::new("closed"))
                }
            }
            Command::Release(id) => {
                if streams.remove(&id).is_some() && !rollback_only {
                    backend
                        .control(&format!("CLOSE asterion_cursor_{id}"))
                        .map(|()| Response::Unit)
                } else {
                    Ok(Response::Unit)
                }
            }
            Command::Close => {
                if active {
                    let _ = backend.control("ROLLBACK");
                }
                let _ = reply.send(Ok(Response::Unit));
                return;
            }
        };
        if active
            && executes
            && result
                .as_ref()
                .is_err_and(|error| !matches!(error.code, "busy" | "closed"))
        {
            rollback_only = true;
        }
        let _ = reply.send(result);
    }
    if active {
        let _ = backend.control("ROLLBACK");
    }
}
fn sqlite_params(params: Vec<Value>) -> Result<Vec<rusqlite::types::Value>> {
    use rusqlite::types::Value as S;
    params
        .into_iter()
        .map(|value| {
            Ok(match value {
                Value::Null => S::Null,
                Value::Bool(v) => S::Integer(i64::from(v)),
                Value::Number(v) if v.is_i64() => S::Integer(v.as_i64().expect("i64")),
                Value::Number(v) if v.is_f64() => {
                    S::Real(v.as_f64().ok_or_else(|| Error::new("data"))?)
                }
                Value::String(v) => S::Text(v),
                _ => return Err(Error::new("data")),
            })
        })
        .collect()
}
fn sqlite_description(statement: &rusqlite::Statement<'_>) -> Vec<Description> {
    statement
        .column_names()
        .into_iter()
        .map(|name| Description {
            name: name.into(),
            kind: String::new(),
        })
        .collect()
}
fn sqlite_row(row: &rusqlite::Row<'_>, columns: usize) -> Result<Vec<Value>> {
    use rusqlite::types::ValueRef as S;
    (0..columns)
        .map(|i| {
            Ok(match row.get_ref(i)? {
                S::Null => Value::Null,
                S::Integer(v) => json!(v),
                S::Real(v) if v.is_finite() => json!(v),
                S::Text(v) => Value::String(
                    std::str::from_utf8(v)
                        .map_err(|_| Error::new("data"))?
                        .into(),
                ),
                _ => return Err(Error::new("data")),
            })
        })
        .collect()
}
fn row_bytes(row: &[Value], bytes: &mut usize) -> Result<()> {
    *bytes += serde_json::to_vec(row)
        .map_err(|_| Error::new("data"))?
        .len();
    if *bytes > BYTE_LIMIT {
        return Err(Error::new("data"));
    }
    Ok(())
}
fn sqlite_execute(
    conn: &rusqlite::Connection,
    id: u64,
    sql: &str,
    params: Vec<Value>,
) -> Result<Execution> {
    let params = sqlite_params(params)?;
    let mut statement = conn.prepare(sql)?;
    let description = sqlite_description(&statement);
    let readonly = statement.readonly();
    let mut found = VecDeque::new();
    let mut bytes = 0;
    let mut rows = statement.query(rusqlite::params_from_iter(params.iter()))?;
    while let Some(row) = rows.next()? {
        let value = sqlite_row(row, description.len())?;
        row_bytes(&value, &mut bytes)?;
        if found.len() >= ROW_LIMIT {
            return Err(Error::new("data"));
        }
        found.push_back(value);
    }
    Ok(Execution {
        id,
        description,
        rows: found,
        rowcount: if readonly { -1 } else { conn.changes() as i64 },
        lastrowid: (!readonly).then(|| conn.last_insert_rowid()),
        streaming: false,
    })
}
struct StreamControl<'a> {
    receiver: &'a mpsc::Receiver<Request>,
    reply: mpsc::SyncSender<Result<Response>>,
    rollback_only: &'a mut bool,
    generation: &'a AtomicU64,
    transaction: u64,
}
fn sqlite_stream(
    conn: &rusqlite::Connection,
    id: u64,
    sql: &str,
    params: Vec<Value>,
    control: StreamControl<'_>,
) -> Option<Request> {
    let StreamControl {
        receiver,
        reply,
        rollback_only,
        generation,
        transaction,
    } = control;
    let params = match sqlite_params(params) {
        Ok(params) => params,
        Err(error) => {
            *rollback_only = true;
            let _ = reply.send(Err(error));
            return None;
        }
    };
    let mut statement = match conn.prepare(sql) {
        Ok(statement) => statement,
        Err(error) => {
            *rollback_only = true;
            let _ = reply.send(Err(error.into()));
            return None;
        }
    };
    if !statement.readonly() {
        *rollback_only = true;
        let _ = reply.send(Err(Error::new("programming")));
        return None;
    }
    let description = sqlite_description(&statement);
    let columns = description.len();
    let mut rows = match statement.query(rusqlite::params_from_iter(params.iter())) {
        Ok(rows) => rows,
        Err(error) => {
            *rollback_only = true;
            let _ = reply.send(Err(error.into()));
            return None;
        }
    };
    if reply
        .send(Ok(Response::Execute(Execution {
            id,
            description,
            rows: VecDeque::new(),
            rowcount: -1,
            lastrowid: None,
            streaming: true,
        })))
        .is_err()
    {
        return None;
    }
    while let Ok(request) = receiver.recv() {
        match request.command {
            Command::Ticket {
                generation: expected,
            } => {
                let result = if !*rollback_only && expected == generation.load(Ordering::Acquire) {
                    Ok(Response::Ticket(transaction))
                } else {
                    Err(Error::new("closed"))
                };
                let _ = request.reply.send(result);
            }
            Command::Fetch {
                id: requested,
                size,
            } if requested == id => {
                let batch: Result<Batch> = (|| {
                    let mut found = Vec::new();
                    let mut bytes = 0;
                    for _ in 0..size {
                        let Some(row) = rows.next()? else {
                            return Ok(Batch {
                                rows: found,
                                done: true,
                            });
                        };
                        let value = sqlite_row(row, columns)?;
                        row_bytes(&value, &mut bytes)?;
                        found.push(value);
                    }
                    Ok(Batch {
                        rows: found,
                        done: false,
                    })
                })();
                if batch.is_err() {
                    *rollback_only = true;
                }
                let done = batch.as_ref().map_or(true, |batch| batch.done);
                let _ = request.reply.send(batch.map(Response::Batch));
                if done {
                    return None;
                }
            }
            Command::Release(requested) if requested == id => {
                let _ = request.reply.send(Ok(Response::Unit));
                return None;
            }
            Command::Commit | Command::Rollback | Command::Close => return Some(request),
            Command::Autocommit(None) => {
                let _ = request.reply.send(Ok(Response::Flag(false)));
            }
            _ => {
                let _ = request.reply.send(Err(Error::new("busy")));
            }
        }
    }
    None
}
#[derive(Debug)]
struct Parameter(Value);
impl ToSql for Parameter {
    fn to_sql(
        &self,
        ty: &Type,
        out: &mut BytesMut,
    ) -> std::result::Result<IsNull, Box<dyn std::error::Error + Sync + Send>> {
        let invalid = || Box::new(Error::new("data")) as Box<dyn std::error::Error + Sync + Send>;
        match (&self.0, ty) {
            (Value::Null, _) => Ok(IsNull::Yes),
            (Value::Bool(v), &Type::BOOL) => v.to_sql(ty, out),
            (Value::Number(v), &Type::INT2) => i16::try_from(v.as_i64().ok_or_else(invalid)?)
                .map_err(|_| invalid())?
                .to_sql(ty, out),
            (Value::Number(v), &Type::INT4) => i32::try_from(v.as_i64().ok_or_else(invalid)?)
                .map_err(|_| invalid())?
                .to_sql(ty, out),
            (Value::Number(v), &Type::INT8) => v.as_i64().ok_or_else(invalid)?.to_sql(ty, out),
            (Value::Number(v), &Type::FLOAT4) => {
                let value = v.as_f64().ok_or_else(invalid)? as f32;
                if !value.is_finite() {
                    return Err(invalid());
                }
                value.to_sql(ty, out)
            }
            (Value::Number(v), &Type::FLOAT8) => v.as_f64().ok_or_else(invalid)?.to_sql(ty, out),
            (Value::String(v), &Type::JSON | &Type::JSONB) => serde_json::from_str::<Value>(v)
                .map_err(|_| invalid())?
                .to_sql(ty, out),
            (
                Value::String(v),
                &Type::TEXT | &Type::VARCHAR | &Type::BPCHAR | &Type::NAME | &Type::UNKNOWN,
            ) => v.to_sql(ty, out),
            _ => Err(invalid()),
        }
    }
    fn accepts(_: &Type) -> bool {
        true
    }
    postgres::types::to_sql_checked!();
}
fn postgres_row(row: &postgres::Row) -> Result<Vec<Value>> {
    row.columns()
        .iter()
        .enumerate()
        .map(|(i, column)| {
            macro_rules! get {
                ($type:ty) => {
                    row.try_get::<_, Option<$type>>(i)?
                        .map(|value| json!(value))
                        .unwrap_or(Value::Null)
                };
            }
            Ok(match *column.type_() {
                Type::BOOL => get!(bool),
                Type::INT2 => get!(i16),
                Type::INT4 => get!(i32),
                Type::INT8 => get!(i64),
                Type::FLOAT4 => {
                    let value: Option<f32> = row.try_get(i)?;
                    if value.is_some_and(|v| !v.is_finite()) {
                        return Err(Error::new("data"));
                    }
                    json!(value)
                }
                Type::FLOAT8 => {
                    let value: Option<f64> = row.try_get(i)?;
                    if value.is_some_and(|v| !v.is_finite()) {
                        return Err(Error::new("data"));
                    }
                    json!(value)
                }
                Type::TEXT | Type::VARCHAR | Type::BPCHAR | Type::NAME => get!(String),
                Type::JSON | Type::JSONB => row
                    .try_get::<_, Option<Value>>(i)?
                    .map(|value| Value::String(value.to_string()))
                    .unwrap_or(Value::Null),
                _ => return Err(Error::new("data")),
            })
        })
        .collect()
}
fn postgres_execute(
    conn: &mut postgres::Client,
    id: u64,
    sql: &str,
    params: Vec<Value>,
    stream: bool,
) -> Result<Execution> {
    let parameters: Vec<_> = params.into_iter().map(Parameter).collect();
    let parameters: Vec<&(dyn ToSql + Sync)> = parameters.iter().map(|value| value as _).collect();
    let statement = conn.prepare(sql)?;
    if statement.params().len() != parameters.len() {
        return Err(Error::new("programming"));
    }
    let description: Vec<_> = statement
        .columns()
        .iter()
        .map(|column| Description {
            name: column.name().into(),
            kind: column.type_().name().into(),
        })
        .collect();
    if stream {
        if description.is_empty() {
            return Err(Error::new("programming"));
        }
        conn.execute(
            &format!("DECLARE asterion_cursor_{id} NO SCROLL CURSOR FOR {sql}"),
            &parameters,
        )?;
        return Ok(Execution {
            id,
            description,
            rows: VecDeque::new(),
            rowcount: -1,
            lastrowid: None,
            streaming: true,
        });
    }
    if description.is_empty() {
        let count = conn.execute(&statement, &parameters)?;
        return Ok(Execution {
            id,
            description,
            rows: VecDeque::new(),
            rowcount: i64::try_from(count).map_err(|_| Error::new("data"))?,
            lastrowid: None,
            streaming: false,
        });
    }
    // query_raw avoids an unbounded intermediate Vec even for regular cursors.
    use postgres::fallible_iterator::FallibleIterator;
    let mut rows = conn.query_raw(&statement, parameters)?;
    let mut found = VecDeque::new();
    let mut bytes = 0;
    while let Some(row) = rows.next()? {
        let value = postgres_row(&row)?;
        row_bytes(&value, &mut bytes)?;
        if found.len() >= ROW_LIMIT {
            return Err(Error::new("data"));
        }
        found.push_back(value);
    }
    let rowcount = rows
        .rows_affected()
        .and_then(|value| i64::try_from(value).ok())
        .unwrap_or(-1);
    Ok(Execution {
        id,
        description,
        rows: found,
        rowcount,
        lastrowid: None,
        streaming: false,
    })
}
fn postgres_fetch(
    conn: &mut postgres::Client,
    id: u64,
    _description: &[Description],
    size: usize,
) -> Result<Batch> {
    use postgres::fallible_iterator::FallibleIterator;
    let mut rows = conn.query_raw(
        &format!("FETCH FORWARD {size} FROM asterion_cursor_{id}"),
        std::iter::empty::<&dyn ToSql>(),
    )?;
    let mut found = Vec::new();
    let mut bytes = 0;
    while let Some(row) = rows.next()? {
        let value = postgres_row(&row)?;
        row_bytes(&value, &mut bytes)?;
        found.push(value);
    }
    let done = found.len() < size;
    drop(rows);
    if done {
        conn.batch_execute(&format!("CLOSE asterion_cursor_{id}"))?;
    }
    Ok(Batch { rows: found, done })
}
fn execute_many(
    backend: &mut Backend,
    id: u64,
    sql: &str,
    params: Vec<Vec<Value>>,
) -> Result<Execution> {
    let mut total = 0_u64;
    match backend {
        Backend::Sqlite(conn) => {
            let mut statement = conn.prepare(sql)?;
            if statement.readonly() || statement.column_count() != 0 {
                return Err(Error::new("programming"));
            }
            for row in params {
                let row = sqlite_params(row)?;
                let count = statement.execute(rusqlite::params_from_iter(row.iter()))? as u64;
                total = total.checked_add(count).ok_or_else(|| Error::new("data"))?;
            }
        }
        Backend::Postgres(conn) => {
            let statement = conn.prepare(sql)?;
            if !statement.columns().is_empty() {
                return Err(Error::new("programming"));
            }
            for row in params {
                if row.len() != statement.params().len() {
                    return Err(Error::new("programming"));
                }
                let row: Vec<_> = row.into_iter().map(Parameter).collect();
                let values: Vec<&(dyn ToSql + Sync)> = row.iter().map(|value| value as _).collect();
                let count = conn.execute(&statement, &values)?;
                total = total.checked_add(count).ok_or_else(|| Error::new("data"))?;
            }
        }
    }
    Ok(Execution {
        id,
        description: vec![],
        rows: VecDeque::new(),
        rowcount: i64::try_from(total).map_err(|_| Error::new("data"))?,
        lastrowid: None,
        streaming: false,
    })
}
fn quoted(name: &str) -> String {
    format!("\"{}\"", name.replace('"', "\"\""))
}
fn sqlite_schema(conn: &rusqlite::Connection, name: String) -> Result<Schema> {
    let mut schema = Schema {
        name: name.clone(),
        columns: vec![],
        primary: vec![],
        unique: vec![],
        foreign: vec![],
        indexes: vec![],
    };
    let mut primary = BTreeMap::new();
    let mut statement = conn.prepare(&format!("PRAGMA table_info({})", quoted(&name)))?;
    let mut rows = statement.query([])?;
    while let Some(row) = rows.next()? {
        let column: String = row.get(1)?;
        let kind: String = row.get(2)?;
        let required: bool = row.get(3)?;
        let pk: i64 = row.get(5)?;
        if pk > 0 {
            primary.insert(pk, column.clone());
        }
        schema.columns.push(Column {
            name: column,
            kind: kind.to_uppercase(),
            nullable: !required,
        });
    }
    schema.primary = primary.into_values().collect();
    let mut statement = conn.prepare(&format!("PRAGMA index_list({})", quoted(&name)))?;
    let mut rows = statement.query([])?;
    while let Some(row) = rows.next()? {
        let index: String = row.get(1)?;
        let unique: bool = row.get(2)?;
        let origin: String = row.get(3)?;
        let partial: bool = row.get(4)?;
        if origin == "pk" {
            continue;
        }
        let mut details = conn.prepare(&format!("PRAGMA index_info({})", quoted(&index)))?;
        let columns: Vec<Option<String>> = details
            .query_map([], |row| row.get(2))?
            .collect::<std::result::Result<_, _>>()?;
        if columns.iter().any(Option::is_none) {
            continue;
        }
        let columns: Vec<String> = columns.into_iter().flatten().collect();
        if unique && !partial {
            schema.unique.push(columns.clone());
        }
        schema.indexes.push(Index {
            name: index,
            columns,
            unique: unique && !partial,
        });
    }
    let mut statement = conn.prepare(&format!("PRAGMA foreign_key_list({})", quoted(&name)))?;
    let mut rows = statement.query([])?;
    let mut foreign = BTreeMap::<i64, Foreign>::new();
    while let Some(row) = rows.next()? {
        let id: i64 = row.get(0)?;
        let target: String = row.get(2)?;
        let from: String = row.get(3)?;
        let to: String = row.get(4)?;
        let key = foreign.entry(id).or_insert(Foreign {
            columns: vec![],
            table: target,
            references: vec![],
        });
        key.columns.push(from);
        key.references.push(to);
    }
    schema.foreign = foreign.into_values().collect();
    Ok(schema)
}
fn postgres_schema(conn: &mut postgres::Client, name: String) -> Result<Schema> {
    let mut schema = Schema {
        name: name.clone(),
        columns: vec![],
        primary: vec![],
        unique: vec![],
        foreign: vec![],
        indexes: vec![],
    };
    for row in conn.query("SELECT column_name::text, data_type::text, is_nullable='YES', character_maximum_length::bigint FROM information_schema.columns WHERE table_schema=current_schema() AND table_name=$1 ORDER BY ordinal_position", &[&name])? {
        let kind: String = row.get(1); let length: Option<i64> = row.get(3);
        let kind = match kind.as_str() {
            "character varying" => length.map(|length| format!("VARCHAR({length})")).unwrap_or("VARCHAR".into()),
            "double precision" | "real" => "FLOAT".into(), _ => kind.to_uppercase(),
        };
        schema.columns.push(Column { name: row.get(0), kind, nullable: row.get(2) });
    }
    let mut unique = BTreeMap::<String, Vec<String>>::new();
    for row in conn.query("SELECT tc.constraint_name::text, tc.constraint_type::text, kcu.column_name::text FROM information_schema.table_constraints tc JOIN information_schema.key_column_usage kcu ON tc.constraint_catalog=kcu.constraint_catalog AND tc.constraint_schema=kcu.constraint_schema AND tc.constraint_name=kcu.constraint_name WHERE tc.table_schema=current_schema() AND tc.table_name=$1 AND tc.constraint_type IN ('PRIMARY KEY','UNIQUE') ORDER BY tc.constraint_name,kcu.ordinal_position", &[&name])? {
        let kind: String = row.get(1); let column: String = row.get(2);
        if kind == "PRIMARY KEY" { schema.primary.push(column); } else { unique.entry(row.get(0)).or_default().push(column); }
    }
    schema.unique = unique.into_values().collect();
    let mut indexes = BTreeMap::<String, Index>::new();
    for row in conn.query("SELECT idx.relname::text,a.attname::text,(i.indisunique AND i.indpred IS NULL AND i.indexprs IS NULL) FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class t ON t.oid=i.indrelid JOIN pg_catalog.pg_namespace ns ON ns.oid=t.relnamespace JOIN pg_catalog.pg_class idx ON idx.oid=i.indexrelid JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS k(attnum,pos) ON k.pos<=i.indnkeyatts JOIN pg_catalog.pg_attribute a ON a.attrelid=t.oid AND a.attnum=k.attnum WHERE ns.nspname=current_schema() AND t.relname=$1 AND NOT i.indisprimary ORDER BY idx.relname,k.pos", &[&name])? {
        let index: String = row.get(0); let column: String = row.get(1); let unique: bool = row.get(2);
        indexes.entry(index.clone()).or_insert(Index { name: index, columns: vec![], unique }).columns.push(column);
    }
    for index in indexes.into_values() {
        if index.unique && !schema.unique.contains(&index.columns) {
            schema.unique.push(index.columns.clone());
        }
        schema.indexes.push(index);
    }
    let mut foreign = BTreeMap::<String, Foreign>::new();
    for row in conn.query("SELECT c.conname::text,a.attname::text,tgt.relname::text,b.attname::text FROM pg_catalog.pg_constraint c JOIN pg_catalog.pg_class src ON src.oid=c.conrelid JOIN pg_catalog.pg_namespace ns ON ns.oid=src.relnamespace JOIN pg_catalog.pg_class tgt ON tgt.oid=c.confrelid JOIN LATERAL unnest(c.conkey,c.confkey) WITH ORDINALITY AS k(srcnum,dstnum,pos) ON true JOIN pg_catalog.pg_attribute a ON a.attrelid=src.oid AND a.attnum=k.srcnum JOIN pg_catalog.pg_attribute b ON b.attrelid=tgt.oid AND b.attnum=k.dstnum WHERE c.contype='f' AND ns.nspname=current_schema() AND src.relname=$1 ORDER BY c.conname,k.pos", &[&name])? {
        let key: String = row.get(0); let column: String = row.get(1); let table: String = row.get(2); let target: String = row.get(3);
        let key = foreign.entry(key).or_insert(Foreign { columns: vec![], table, references: vec![] });
        key.columns.push(column); key.references.push(target);
    }
    schema.foreign = foreign.into_values().collect();
    Ok(schema)
}

/// Native bounded pool. A lease returns only after rollback and cursor invalidation.
/// Disposal retires its generation; outstanding leases remain usable until closed.
pub struct Database {
    inner: Arc<Pool>,
}
struct Pool {
    id: u64,
    config: String,
    capacity: usize,
    wait: Duration,
    state: Mutex<PoolState>,
    available: std::sync::Condvar,
}
struct PoolState {
    idle: Vec<Connection>,
    total: usize,
    generation: u64,
}
pub struct Lease {
    connection: Option<Connection>,
    owner: Option<(Arc<Pool>, u64)>,
}
impl Database {
    pub fn new(config: String, capacity: usize, wait_seconds: f64) -> Result<Self> {
        let parsed: Config =
            serde_json::from_str(&config).map_err(|_| Error::new("programming"))?;
        let wait =
            Duration::try_from_secs_f64(wait_seconds).map_err(|_| Error::new("programming"))?;
        if capacity == 0
            || capacity > 64
            || wait.is_zero()
            || wait > Duration::from_secs(300)
            || matches!(parsed, Config::Sqlite { path, .. } if path == ":memory:") && capacity != 1
        {
            return Err(Error::new("programming"));
        }
        Ok(Self {
            inner: Arc::new(Pool {
                id: DATABASE_IDS.fetch_add(1, Ordering::Relaxed),
                config,
                capacity,
                wait,
                state: Mutex::new(PoolState {
                    idle: vec![],
                    total: 0,
                    generation: 0,
                }),
                available: std::sync::Condvar::new(),
            }),
        })
    }
    pub fn id(&self) -> u64 {
        self.inner.id
    }
    pub fn connect(&self) -> Result<Lease> {
        let deadline = std::time::Instant::now() + self.inner.wait;
        let mut state = self.inner.state.lock().map_err(|_| Error::new("closed"))?;
        loop {
            let generation = state.generation;
            if let Some(connection) = state.idle.pop() {
                return Ok(Lease {
                    connection: Some(connection),
                    owner: Some((self.inner.clone(), generation)),
                });
            }
            if state.total < self.inner.capacity {
                state.total += 1;
                drop(state);
                return match Connection::open_for(&self.inner.config, self.inner.id) {
                    Ok(connection) => Ok(Lease {
                        connection: Some(connection),
                        owner: Some((self.inner.clone(), generation)),
                    }),
                    Err(error) => {
                        let mut state =
                            self.inner.state.lock().map_err(|_| Error::new("closed"))?;
                        if state.generation == generation {
                            state.total -= 1;
                        }
                        self.inner.available.notify_one();
                        Err(error)
                    }
                };
            }
            let remaining = deadline.saturating_duration_since(std::time::Instant::now());
            if remaining.is_zero() {
                return Err(Error::new("operational"));
            }
            let (next, timeout) = self
                .inner
                .available
                .wait_timeout(state, remaining)
                .map_err(|_| Error::new("closed"))?;
            state = next;
            if timeout.timed_out() {
                return Err(Error::new("operational"));
            }
        }
    }
    pub fn dispose(&self) -> Result<()> {
        let mut state = self.inner.state.lock().map_err(|_| Error::new("closed"))?;
        let idle = std::mem::take(&mut state.idle);
        state.total = 0;
        state.generation += 1;
        self.inner.available.notify_all();
        drop(state);
        for connection in idle {
            let _ = connection.close();
        }
        Ok(())
    }
}
impl Lease {
    pub fn open(config: &str) -> Result<Self> {
        Ok(Self {
            connection: Some(Connection::open(config)?),
            owner: None,
        })
    }
    pub fn connection(&self) -> Result<&Connection> {
        self.connection.as_ref().ok_or_else(|| Error::new("closed"))
    }
    pub fn close(&mut self) -> Result<()> {
        let Some(connection) = self.connection.take() else {
            return Ok(());
        };
        connection.handle.generation.fetch_add(1, Ordering::AcqRel);
        let healthy = connection
            .rollback()
            .and_then(|()| connection.set_autocommit(false));
        if let Some((pool, generation)) = self.owner.take() {
            let mut state = pool.state.lock().map_err(|_| Error::new("closed"))?;
            if generation == state.generation && healthy.is_ok() {
                state.idle.push(connection);
                pool.available.notify_one();
                return Ok(());
            }
            if generation == state.generation {
                state.total -= 1;
            }
            pool.available.notify_one();
        }
        let _ = connection.close();
        healthy
    }
}
impl Drop for Lease {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
impl Drop for Pool {
    fn drop(&mut self) {
        if let Ok(state) = self.state.get_mut() {
            for connection in state.idle.drain(..) {
                let _ = connection.close();
            }
        }
    }
}
pub fn sqlite_version() -> (u32, u32, u32) {
    let version = rusqlite::version_number() as u32;
    (version / 1_000_000, (version / 1000) % 1000, version % 1000)
}

/// A non-owning ticket to one active physical transaction. Pool reuse and every
/// commit/rollback expire it; it cannot create or extend a transaction.
#[derive(Clone)]
pub struct TransactionRef {
    handle: Weak<Handle>,
    database: u64,
    generation: u64,
    transaction: u64,
}
#[derive(Debug)]
pub enum OperationError {
    Database(Error),
    Contract(String),
}
impl From<Error> for OperationError {
    fn from(value: Error) -> Self {
        Self::Database(value)
    }
}
impl From<String> for OperationError {
    fn from(value: String) -> Self {
        Self::Contract(value)
    }
}
impl From<&str> for OperationError {
    fn from(value: &str) -> Self {
        Self::Contract(value.into())
    }
}
impl std::fmt::Display for OperationError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Database(error) => error.fmt(f),
            Self::Contract(message) => f.write_str(message),
        }
    }
}
impl std::error::Error for OperationError {}
pub(crate) type OperationResult = std::result::Result<Value, OperationError>;
type Operation = Box<dyn FnOnce(&mut Session<'_>) -> OperationResult + Send>;
impl TransactionRef {
    pub fn database_id(&self) -> u64 {
        self.database
    }
    pub fn check(&self) -> Result<()> {
        let handle = self.handle.upgrade().ok_or_else(|| Error::new("closed"))?;
        match handle.request(Command::Ticket {
            generation: self.generation,
        })? {
            Response::Ticket(epoch) if epoch == self.transaction => Ok(()),
            _ => Err(Error::new("closed")),
        }
    }
    /// Fast consumption check. Every physical commit/rollback and pool return
    /// advances this monotonic generation; this never calls the database actor.
    pub(crate) fn check_current(&self) -> Result<()> {
        let handle = self.handle.upgrade().ok_or_else(|| Error::new("closed"))?;
        if handle.closed.load(Ordering::Acquire)
            || self.generation != handle.generation.load(Ordering::Acquire)
        {
            Err(Error::new("closed"))
        } else {
            Ok(())
        }
    }
    // Only fixed kernel modules can run code inside the physical transaction.
    // This is deliberately not an extensibility/plugin or raw Python API.
    pub(crate) fn operation(
        &self,
        operation: impl FnOnce(&mut Session<'_>) -> OperationResult + Send + 'static,
    ) -> OperationResult {
        let handle = self.handle.upgrade().ok_or_else(|| Error::new("closed"))?;
        match handle.request(Command::Operation {
            generation: self.generation,
            transaction: self.transaction,
            operation: Box::new(operation),
        })? {
            Response::Operation(result) => result,
            _ => Err(Error::new("programming").into()),
        }
    }
}
pub(crate) struct Session<'a> {
    backend: &'a mut Backend,
    counter: &'a mut u64,
}
impl Session<'_> {
    pub(crate) fn is_postgres(&self) -> bool {
        matches!(self.backend, Backend::Postgres(_))
    }
    pub(crate) fn execute(&mut self, sql: &str, params: Vec<Value>) -> Result<Vec<Vec<Value>>> {
        statement(sql)?;
        parameters(&params)?;
        *self.counter += 1;
        Ok(self
            .backend
            .execute(*self.counter, sql, params, false)?
            .rows
            .into_iter()
            .collect())
    }
    pub(crate) fn scan(
        &mut self,
        sql: &str,
        mut visit: impl FnMut(Vec<Value>) -> std::result::Result<(), OperationError>,
    ) -> std::result::Result<(), OperationError> {
        *self.counter += 1;
        match self.backend {
            Backend::Sqlite(conn) => {
                let mut statement = conn.prepare(sql).map_err(Error::from)?;
                if !statement.readonly() {
                    return Err(Error::new("programming").into());
                }
                let count = statement.column_count();
                let mut rows = statement.query([]).map_err(Error::from)?;
                while let Some(row) = rows.next().map_err(Error::from)? {
                    let row = sqlite_row(row, count)?;
                    row_bytes(&row, &mut 0)?;
                    visit(row)?;
                }
                Ok(())
            }
            Backend::Postgres(conn) => {
                let id = *self.counter;
                let execution = postgres_execute(conn, id, sql, vec![], true)?;
                let result = (|| {
                    loop {
                        let batch = postgres_fetch(conn, id, &execution.description, FETCH_LIMIT)?;
                        for row in batch.rows {
                            visit(row)?;
                        }
                        if batch.done {
                            break;
                        }
                    }
                    Ok(())
                })();
                // EOF already closes the portal. On validation error release it;
                // the caller poisons the physical transaction until rollback.
                if result.is_err() {
                    let _ = conn.batch_execute(&format!("CLOSE asterion_cursor_{id}"));
                }
                result
            }
        }
    }
}

// Transaction control has dedicated actor commands. Letting statement SQL end
// or replace a transaction would invalidate the actor's physical epoch contract.
fn statement(sql: &str) -> Result<()> {
    if sql.len() > SQL_LIMIT {
        return Err(Error::new("programming"));
    }
    let mut rest = sql;
    loop {
        rest = rest
            .trim_start_matches(|c: char| c.is_ascii_whitespace() || c == ';' || c == '\u{feff}');
        if rest.starts_with("--") {
            let end = rest.find('\n').ok_or_else(|| Error::new("programming"))?;
            rest = &rest[end + 1..];
        } else if rest.starts_with("/*") {
            let mut depth = 1_usize;
            let mut end = 2;
            let bytes = rest.as_bytes();
            while depth > 0 && end + 1 < bytes.len() {
                match &bytes[end..end + 2] {
                    b"/*" => {
                        depth += 1;
                        end += 2;
                    }
                    b"*/" => {
                        depth -= 1;
                        end += 2;
                    }
                    _ => end += 1,
                }
            }
            if depth != 0 {
                return Err(Error::new("programming"));
            }
            rest = &rest[end..];
        } else {
            break;
        }
    }
    if rest.is_empty() {
        return Err(Error::new("programming"));
    }
    let end = rest.bytes().take_while(u8::is_ascii_alphabetic).count();
    let keyword = &rest[..end];
    if [
        "BEGIN",
        "COMMIT",
        "END",
        "ROLLBACK",
        "ABORT",
        "SAVEPOINT",
        "RELEASE",
        "START",
        "PREPARE",
    ]
    .iter()
    .any(|word| keyword.eq_ignore_ascii_case(word))
    {
        return Err(Error::new("programming"));
    }
    Ok(())
}

fn parameters(params: &[Value]) -> Result<()> {
    if params.len() > 32_767 {
        return Err(Error::new("programming"));
    }
    parameter_budget(params)
}
fn parameter_budget(value: &(impl Serialize + ?Sized)) -> Result<()> {
    struct Budget(usize);
    impl std::io::Write for Budget {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.0 = self
                .0
                .checked_add(bytes.len())
                .ok_or_else(|| std::io::Error::other("parameter budget"))?;
            if self.0 > BYTE_LIMIT {
                return Err(std::io::Error::other("parameter budget"));
            }
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    serde_json::to_writer(Budget(0), value).map_err(|_| Error::new("data"))
}
#[cfg(test)]
mod parameter_tests {
    use super::*;
    #[test]
    fn fixed_operations_cannot_bypass_parameter_limits() {
        let connection = Connection::open(r#"{"backend":"sqlite","path":":memory:"}"#).unwrap();
        connection.begin().unwrap();
        let ticket = connection.transaction().unwrap();
        assert!(
            ticket
                .operation(|session| Ok(json!(
                    session.execute("SELECT 1", vec![Value::Null; 32_768])?
                )))
                .is_err()
        );
        assert!(connection.commit().is_err());
        connection.rollback().unwrap();
        connection.begin().unwrap();
        let ticket = connection.transaction().unwrap();
        assert!(
            ticket
                .operation(|session| Ok(json!(
                    session.execute("SELECT $1", vec![json!("x".repeat(BYTE_LIMIT))])?
                )))
                .is_err()
        );
        assert!(connection.commit().is_err());
        connection.rollback().unwrap();
        assert_eq!(
            connection
                .execute("SELECT 1", vec![], false)
                .unwrap()
                .fetchmany(1)
                .unwrap(),
            [vec![json!(1)]]
        );
    }
}
