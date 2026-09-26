//! Fixed transactional event journal: admission, commit-ordered SQL publication,
//! bounded replay and streaming restore verification on the caller's transaction.
use asterion_foundation::communication;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    sync::Arc,
};

pub type Result<T> = std::result::Result<T, String>;
pub const MAX_SEQUENCE: i64 = i64::MAX;
pub const PAYLOAD_BYTES: usize = 65_536;

fn require(ok: bool, message: &str) -> Result<()> {
    if ok { Ok(()) } else { Err(message.into()) }
}
fn namespaced(text: &str) -> bool {
    text.contains('.')
        && text.split('.').all(|part| {
            part.as_bytes().first().is_some_and(u8::is_ascii_lowercase)
                && part
                    .bytes()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_')
        })
}

/// `payload` identifies a driver-owned business payload type, not a schema the
/// generic kernel interprets or a callback it executes.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Topic {
    pub id: String,
    pub owner: String,
    pub payload: String,
    pub read_path: String,
}
impl Topic {
    pub fn validate(&self) -> Result<()> {
        require(
            namespaced(&self.id) && namespaced(&self.owner),
            "Event owner and topic must be namespaced",
        )?;
        require(
            self.id.len() <= 200 && self.owner.len() <= 100 && !self.payload.is_empty(),
            "Invalid event topic descriptor",
        )?;
        require(
            self.read_path.starts_with('/')
                && self.read_path.len() > 1
                && !self.read_path.contains("//")
                && self
                    .read_path
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || b"_/-".contains(&c)),
            "Event reads require an explicit authorization path",
        )
    }
}

#[derive(Clone, Debug)]
pub struct Registry {
    topics: BTreeMap<String, Topic>,
}
impl Registry {
    pub fn new(topics: Vec<Topic>) -> Result<Self> {
        let mut registered = BTreeMap::new();
        for topic in topics {
            topic.validate()?;
            require(
                registered.insert(topic.id.clone(), topic).is_none(),
                "Duplicate event topic",
            )?;
        }
        Ok(Self { topics: registered })
    }
    pub fn topic(&self, topic: &Topic) -> Result<()> {
        require(
            self.topics.get(&topic.id) == Some(topic),
            "Undeclared event topic",
        )
    }
    pub fn read_path(&self, topic: &str, method: &str) -> Result<&str> {
        require(method == "GET", "Undeclared event read")?;
        self.topics
            .get(topic)
            .map(|t| t.read_path.as_str())
            .ok_or("Undeclared event read".into())
    }
    pub fn publisher(self: &Arc<Self>, owner: &str, topics: &[Topic]) -> Result<Publisher> {
        let mut allowed = BTreeSet::new();
        for topic in topics {
            self.topic(topic)?;
            require(topic.owner == owner, "Invalid event publication grant")?;
            allowed.insert(topic.id.clone());
        }
        Ok(Publisher {
            registry: self.clone(),
            allowed,
        })
    }
    pub fn prepare(
        &self,
        topic: &Topic,
        stream: &str,
        payload: Value,
        trace: Value,
    ) -> Result<Draft> {
        self.topic(topic)?;
        communication::validate("Context", &trace)?;
        let message = json!({
            "version": 1, "id": uuid::Uuid::new_v4().simple().to_string(),
            "topic": topic.id, "owner": topic.owner, "stream": stream, "sequence": "1",
            "correlation_id": trace["correlation_id"], "causation_id": trace["request_id"], "payload": payload
        });
        check_message(&message)?;
        Ok(Draft { message })
    }
    pub fn read_plan(&self, topic: &str, after: &str, limit: i64) -> Result<ReadPlan> {
        require(
            self.topics.contains_key(topic) && (1..=500).contains(&limit),
            "Invalid event topic, cursor or page size",
        )?;
        let offset = if after == "latest" {
            None
        } else {
            Some(cursor(after)?)
        };
        Ok(ReadPlan {
            topic: topic.into(),
            after: after.into(),
            offset,
            limit,
        })
    }
    pub fn latest(&self, plan: &ReadPlan, head: Option<i64>) -> Result<Value> {
        self.check_plan(plan)?;
        require(plan.offset.is_none(), "Event read requires a latest cursor")?;
        let number = head.unwrap_or(0);
        require(number >= 0, "Invalid event head sequence")?;
        Ok(json!({"items": [], "cursor": number.to_string()}))
    }
    pub fn page(&self, plan: &ReadPlan, rows: Vec<Row>) -> Result<Value> {
        self.check_plan(plan)?;
        let mut sequence = plan
            .offset
            .ok_or("Latest event cursor cannot read a page")?;
        require(
            rows.len() <= plan.limit as usize,
            "Event page exceeds its requested size",
        )?;
        let owner = &self.topics[&plan.topic].owner;
        let mut items = Vec::with_capacity(rows.len());
        for row in rows {
            check_row(&row)?;
            require(
                row.topic == plan.topic
                    && row.message["owner"] == *owner
                    && Some(row.sequence) == sequence.checked_add(1),
                "Event page is inconsistent",
            )?;
            sequence = row.sequence;
            items.push(row.message);
        }
        let next = if items.is_empty() {
            plan.after.clone()
        } else {
            sequence.to_string()
        };
        Ok(json!({"items": items, "cursor": next}))
    }
    fn check_plan(&self, plan: &ReadPlan) -> Result<()> {
        let expected = self.read_plan(&plan.topic, &plan.after, plan.limit)?;
        require(*plan == expected, "Invalid event read plan")
    }
}

#[derive(Clone, Debug)]
pub struct Publisher {
    registry: Arc<Registry>,
    allowed: BTreeSet<String>,
}
impl Publisher {
    pub fn authorize(&self, topic: &Topic, writable: bool) -> Result<()> {
        self.registry.topic(topic)?;
        require(
            writable && self.allowed.contains(&topic.id),
            "Event publication is outside the granted transaction",
        )
    }
}

#[derive(Debug)]
pub struct Draft {
    message: Value,
}
impl Draft {
    /// Driver translates these bounds to a guarded SQL head increment, retaining
    /// the existing row lock until commit and preventing BIGINT overflow.
    pub fn head_bounds(&self) -> (i64, i64) {
        (0, MAX_SEQUENCE - 1)
    }
    pub fn finish(&self, sequence: Option<i64>) -> Result<Value> {
        let sequence = sequence.ok_or("Event sequence exhausted or head invalid")?;
        require(sequence > 0, "Invalid event sequence")?;
        let mut result = self.message.clone();
        result["sequence"] = json!(sequence.to_string());
        Ok(result)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReadPlan {
    pub topic: String,
    pub after: String,
    pub offset: Option<i64>,
    pub limit: i64,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Row {
    pub id: String,
    pub topic: String,
    pub sequence: i64,
    pub stream: String,
    pub message: Value,
}
fn cursor(value: &str) -> Result<i64> {
    require(
        !value.is_empty() && value.len() <= 19 && value.bytes().all(|c| c.is_ascii_digit()),
        "Invalid event topic, cursor or page size",
    )?;
    value
        .parse::<i64>()
        .map_err(|_| "Invalid event topic, cursor or page size".into())
}
fn check_message(message: &Value) -> Result<i64> {
    communication::validate("Event", message)?;
    let value = message["sequence"]
        .as_str()
        .ok_or("Invalid event sequence")?;
    let number = cursor(value)?;
    require(
        number > 0 && number.to_string() == value,
        "Invalid event sequence",
    )?;
    require(
        namespaced(message["topic"].as_str().unwrap_or_default())
            && namespaced(message["owner"].as_str().unwrap_or_default()),
        "Invalid event identity",
    )?;
    check_payload(&message["payload"])?;
    Ok(number)
}
fn check_row(row: &Row) -> Result<()> {
    let sequence = check_message(&row.message)?;
    require(
        row.sequence > 0
            && sequence == row.sequence
            && row.message["id"] == row.id
            && row.message["topic"] == row.topic
            && row.message["stream"] == row.stream,
        "Restored event journal is inconsistent",
    )
}

// Match the UTF-8 JSON budget including the existing comma/colon spaces. This
// counting writer never allocates a second potentially unbounded payload.
#[derive(Default)]
struct Budget(usize);
impl io::Write for Budget {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0 = self
            .0
            .checked_add(bytes.len())
            .ok_or_else(|| io::Error::other("payload size overflow"))?;
        if self.0 > PAYLOAD_BYTES {
            return Err(io::Error::other("payload budget"));
        }
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
struct Spaced;
impl serde_json::ser::Formatter for Spaced {
    fn begin_array_value<W: ?Sized + io::Write>(
        &mut self,
        w: &mut W,
        first: bool,
    ) -> io::Result<()> {
        if first { Ok(()) } else { w.write_all(b", ") }
    }
    fn begin_object_key<W: ?Sized + io::Write>(
        &mut self,
        w: &mut W,
        first: bool,
    ) -> io::Result<()> {
        if first { Ok(()) } else { w.write_all(b", ") }
    }
    fn begin_object_value<W: ?Sized + io::Write>(&mut self, w: &mut W) -> io::Result<()> {
        w.write_all(b": ")
    }
}
fn check_payload(payload: &Value) -> Result<()> {
    payload
        .serialize(&mut serde_json::Serializer::with_formatter(
            Budget::default(),
            Spaced,
        ))
        .map_err(|_| "Event payload exceeds 64 KiB; publish immutable references".into())
}

/// Streaming restore verifier: O(number of topics), never O(number of events).
#[derive(Default, Debug)]
pub struct JournalValidator {
    actual: BTreeMap<String, i64>,
    last_topic: Option<String>,
    count: u64,
    heads_started: bool,
    failed: bool,
    finished: bool,
}
impl JournalValidator {
    fn active(&self) -> Result<()> {
        require(
            !self.failed && !self.finished,
            "Event journal validation is closed",
        )
    }
    pub fn row(&mut self, row: Row) -> Result<()> {
        self.active()?;
        let result = (|| {
            require(
                !self.heads_started,
                "Event rows cannot follow restored heads",
            )?;
            check_row(&row)?;
            require(
                (self.last_topic.as_ref() == Some(&row.topic)
                    || !self.actual.contains_key(&row.topic))
                    && self
                        .actual
                        .get(&row.topic)
                        .copied()
                        .unwrap_or(0)
                        .checked_add(1)
                        == Some(row.sequence),
                "Restored event journal is inconsistent",
            )?;
            self.count = self.count.checked_add(1).ok_or("Event count overflow")?;
            self.last_topic = Some(row.topic.clone());
            self.actual.insert(row.topic, row.sequence);
            Ok(())
        })();
        self.failed = result.is_err();
        result
    }
    pub fn head(&mut self, topic: &str, sequence: i64) -> Result<()> {
        self.active()?;
        self.heads_started = true;
        let result = require(
            self.actual.get(topic) == Some(&sequence),
            "Restored event cursor does not match journal",
        );
        if result.is_ok() {
            self.actual.remove(topic);
        }
        self.failed = result.is_err();
        result
    }
    pub fn finish(&mut self) -> Result<u64> {
        self.active()?;
        self.finished = true;
        require(
            self.actual.is_empty(),
            "Restored event cursor does not match journal",
        )?;
        Ok(self.count)
    }
}

#[cfg(test)]
mod tests;

/// Fixed journal repository bound to one native database pool. It borrows the
/// caller's physical transaction and never starts, commits or replaces it.
#[derive(Clone)]
pub struct Journal {
    database: u64,
    registry: Arc<Registry>,
}
use crate::database::{Connection, OperationError, Session, TransactionRef};
use crate::plugins::ScopedHandle;
use crate::storage::Transaction;
type DatabaseResult<T> = std::result::Result<T, OperationError>;
impl Journal {
    pub fn new(database: u64, topics: Vec<Topic>) -> Result<Self> {
        Ok(Self {
            database,
            registry: Arc::new(Registry::new(topics)?),
        })
    }
    pub fn registry(&self) -> &Arc<Registry> {
        &self.registry
    }
    pub fn publisher(&self, owner: &str, topics: &[Topic]) -> Result<Writer> {
        Ok(Writer {
            journal: self.clone(),
            grant: self.registry.publisher(owner, topics)?,
            lifecycle: None,
        })
    }
    fn ticket(&self, connection: &Connection) -> DatabaseResult<TransactionRef> {
        if connection.database_id() != self.database {
            return Err("Event requires its database transaction".into());
        }
        connection
            .transaction()
            .map_err(|_| "Event requires its database transaction".into())
    }
    pub fn publish_host(
        &self,
        connection: &Connection,
        topic: Topic,
        stream: String,
        payload: Value,
        trace: Value,
    ) -> DatabaseResult<Value> {
        self.publish(self.ticket(connection)?, topic, stream, payload, trace)
    }
    fn publish(
        &self,
        ticket: TransactionRef,
        topic: Topic,
        stream: String,
        payload: Value,
        trace: Value,
    ) -> DatabaseResult<Value> {
        if ticket.database_id() != self.database {
            return Err("Event requires its database transaction".into());
        }
        let registry = self.registry.clone();
        ticket.operation(move |session| {
            let draft = registry.prepare(&topic, &stream, payload, trace)?;
            publish_in(session, &topic.id, &stream, draft)
        })
    }
    pub fn read(&self, connection: &Connection, plan: ReadPlan) -> DatabaseResult<Value> {
        self.registry.check_plan(&plan)?;
        let registry = self.registry.clone();
        self.ticket(connection)?.operation(move |session| {
            if plan.offset.is_none() {
                let rows = session.execute("SELECT sequence FROM communication_heads WHERE topic=$1", vec![json!(plan.topic)])?;
                let number = rows.first().map(|row| integer(&row[0])).transpose()?;
                return Ok(registry.latest(&plan, number)?);
            }
            let rows = session.execute("SELECT id,topic,sequence,stream,message FROM communication_events WHERE topic=$1 AND sequence>$2 ORDER BY sequence LIMIT $3", vec![json!(plan.topic), json!(plan.offset), json!(plan.limit)])?;
            let rows = rows.into_iter().map(event_row).collect::<DatabaseResult<Vec<_>>>()?;
            Ok(registry.page(&plan, rows)?)
        })
    }
}
#[derive(Clone)]
pub struct Writer {
    journal: Journal,
    grant: Publisher,
    lifecycle: Option<ScopedHandle>,
}
impl Writer {
    pub fn check(&self, transaction: &Transaction, topic: &Topic) -> DatabaseResult<()> {
        self.grant.authorize(topic, transaction.writable()?)?;
        if let Some(handle) = &self.lifecycle {
            handle.check()?;
        }
        Ok(())
    }
    pub fn guarded(&self, lifecycle: ScopedHandle) -> Result<Self> {
        lifecycle.check()?;
        if self.lifecycle.is_some() {
            return Err("Event publication lifecycle is already bound".into());
        }
        Ok(Self {
            journal: self.journal.clone(),
            grant: self.grant.clone(),
            lifecycle: Some(lifecycle),
        })
    }
    pub fn publish(
        &self,
        transaction: &Transaction,
        topic: Topic,
        stream: String,
        payload: Value,
        trace: Value,
    ) -> DatabaseResult<Value> {
        self.check(transaction, &topic)?;
        if let Some(handle) = &self.lifecycle {
            transaction.participant(handle)?;
        }
        let ticket = transaction.require_bound(true)?;
        let result = self.journal.publish(ticket, topic, stream, payload, trace);
        if result.is_err() {
            transaction.abort();
        }
        result
    }
}
/// Internal composition point for native task repositories. Only fixed
/// kernel code has Session access; a plugin never supplies SQL or callbacks here.
pub(crate) fn publish_in(
    session: &mut Session<'_>,
    topic: &str,
    stream: &str,
    draft: Draft,
) -> DatabaseResult<Value> {
    session.execute("INSERT INTO communication_heads(topic,sequence) VALUES($1,0) ON CONFLICT(topic) DO NOTHING", vec![json!(topic)])?;
    let (minimum, maximum) = draft.head_bounds();
    // The update lock stays held until the outer business transaction commits.
    let rows = session.execute("UPDATE communication_heads SET sequence=sequence+1 WHERE topic=$1 AND sequence BETWEEN $2 AND $3 RETURNING sequence", vec![json!(topic), json!(minimum), json!(maximum)])?;
    let number = rows.first().map(|row| integer(&row[0])).transpose()?;
    let message = draft.finish(number)?;
    session.execute(
        "INSERT INTO communication_events(id,topic,sequence,stream,message) VALUES($1,$2,$3,$4,$5)",
        vec![
            message["id"].clone(),
            json!(topic),
            json!(number),
            json!(stream),
            json!(message.to_string()),
        ],
    )?;
    Ok(message)
}
fn integer(value: &Value) -> DatabaseResult<i64> {
    value
        .as_i64()
        .ok_or_else(|| "Invalid event sequence".into())
}
fn event_row(row: Vec<Value>) -> DatabaseResult<Row> {
    let [id, topic, sequence, stream, message]: [Value; 5] = row
        .try_into()
        .map_err(|_| "Restored event journal is inconsistent")?;
    let text = |value: Value| {
        value
            .as_str()
            .map(str::to_owned)
            .ok_or("Restored event journal is inconsistent")
    };
    Ok(Row {
        id: text(id)?,
        topic: text(topic)?,
        sequence: integer(&sequence)?,
        stream: text(stream)?,
        message: serde_json::from_str(&text(message)?)
            .map_err(|_| "Restored event journal is inconsistent")?,
    })
}
/// Streaming, read-only verification of retained history inside the supplied
/// transaction; PostgreSQL uses bounded FETCH batches and SQLite actual Rows.
pub fn validate_journal(connection: &Connection) -> DatabaseResult<u64> {
    let result = connection.transaction()?.operation(|session| {
        let mut validator = JournalValidator::default();
        session.scan("SELECT id,topic,sequence,stream,message FROM communication_events ORDER BY topic,sequence", |row| {
            validator.row(event_row(row)?)?;
            Ok(())
        })?;
        session.scan("SELECT topic,sequence FROM communication_heads ORDER BY topic", |row| {
            let topic = row[0].as_str().ok_or("Invalid event topic")?;
            validator.head(topic, integer(&row[1])?)?;
            Ok(())
        })?;
        Ok(json!(validator.finish()?))
    })?;
    result.as_u64().ok_or_else(|| "Invalid event count".into())
}
