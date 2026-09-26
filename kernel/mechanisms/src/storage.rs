//! Fixed storage grants and transaction lifetime rules, independent of SQL dialects.
//! SQL adapters describe their statement tree; they cannot widen a grant or revive
//! a closed participant. Database drivers must commit only after `commit_ready`.
use crate::{database::TransactionRef, lifetime::Lifetime, plugins::ScopedHandle};
use serde::Deserialize;
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, ThreadId},
};

type Result<T> = std::result::Result<T, &'static str>;
const CLOSED: &str = "Storage transaction is closed or invalid";
// Serialize revocation against admission to commit. Database I/O never runs
// under this lock; cancellation after commit admission cannot undo a commit.
static COMMIT_GATE: Mutex<()> = Mutex::new(());

pub struct Scope {
    database: u64,
    readable: BTreeSet<u64>,
    writable: BTreeSet<u64>,
    active: AtomicBool,
    parent: Option<Arc<Scope>>,
    lifetime: Option<Lifetime>,
}
impl Scope {
    pub fn new(database: u64, writable: Vec<u64>, readable: Vec<u64>) -> Arc<Self> {
        let writable: BTreeSet<_> = writable.into_iter().collect();
        Arc::new(Self {
            database,
            readable: readable
                .into_iter()
                .chain(writable.iter().copied())
                .collect(),
            writable,
            active: AtomicBool::new(true),
            parent: None,
            lifetime: None,
        })
    }
    /// A host resource retains its original grant and gains a plugin lifetime;
    /// neither wrapping nor closing this child can widen or revive its parent.
    pub fn guarded(self: &Arc<Self>, lifetime: Lifetime) -> Result<Arc<Self>> {
        self.check()?;
        lifetime
            .check()
            .map_err(|_| "Storage resource owner is closed")?;
        Ok(Arc::new(Self {
            database: self.database,
            readable: self.readable.clone(),
            writable: self.writable.clone(),
            active: AtomicBool::new(true),
            parent: Some(self.clone()),
            lifetime: Some(lifetime),
        }))
    }
    pub fn check(&self) -> Result<()> {
        if let Some(parent) = &self.parent {
            parent.check()?;
        }
        if let Some(lifetime) = &self.lifetime {
            lifetime
                .check()
                .map_err(|_| "Storage resource owner is closed")?;
        }
        if self.active.load(Ordering::Acquire) {
            Ok(())
        } else {
            Err("Storage is closed")
        }
    }
    pub fn close(&self) {
        let _gate = COMMIT_GATE
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        self.active.store(false, Ordering::Release);
    }
    pub fn initialize(&self, tables: &[u64]) -> Result<()> {
        self.check()?;
        if tables.iter().all(|table| self.writable.contains(table)) {
            Ok(())
        } else {
            Err("Cannot initialize unowned tables")
        }
    }
    pub fn begin(self: &Arc<Self>, write: bool) -> Result<Arc<Transaction>> {
        self.check()?;
        let root = Arc::new(Root {
            active: AtomicBool::new(true),
            thread: thread::current().id(),
            transaction: Mutex::new(None),
            participants: Mutex::new(vec![]),
            writers: Mutex::new(if write { vec![self.clone()] } else { vec![] }),
        });
        Ok(Arc::new(Transaction {
            scope: self.clone(),
            root,
            parent: None,
            write,
            active: AtomicBool::new(true),
        }))
    }
    pub fn join(
        self: &Arc<Self>,
        parent: &Arc<Transaction>,
        write: bool,
    ) -> Result<Arc<Transaction>> {
        self.check()?;
        parent.check()?;
        if self.database != parent.scope.database {
            return Err("Cannot join another database transaction");
        }
        if write && !parent.write {
            return Err("Cannot join a read-only transaction for writing");
        }
        if write {
            parent
                .root
                .writers
                .lock()
                .map_err(|_| CLOSED)?
                .push(self.clone());
        }
        Ok(Arc::new(Transaction {
            scope: self.clone(),
            root: parent.root.clone(),
            parent: Some(parent.clone()),
            write,
            active: AtomicBool::new(true),
        }))
    }
}
struct Root {
    active: AtomicBool,
    thread: ThreadId,
    transaction: Mutex<Option<TransactionRef>>,
    participants: Mutex<Vec<ScopedHandle>>,
    // Keep every writer's grant live through the final commit check, even after
    // its borrowed handle closes. Revocation must not publish earlier writes.
    writers: Mutex<Vec<Arc<Scope>>>,
}
pub struct Transaction {
    scope: Arc<Scope>,
    root: Arc<Root>,
    parent: Option<Arc<Transaction>>,
    write: bool,
    active: AtomicBool,
}
impl Transaction {
    pub fn check(&self) -> Result<()> {
        if !self.active.load(Ordering::Acquire) || !self.root.active.load(Ordering::Acquire) {
            return Err(CLOSED);
        }
        if self.root.thread != thread::current().id() {
            return Err("Storage transaction belongs to another thread");
        }
        self.scope.check()?;
        if let Some(parent) = &self.parent {
            parent.check()?;
        }
        Ok(())
    }
    pub fn bind(&self, ticket: TransactionRef) -> Result<()> {
        self.check()?;
        if self.parent.is_some() || ticket.database_id() != self.scope.database {
            return Err("Cannot bind another database transaction");
        }
        ticket.check().map_err(|_| CLOSED)?;
        let mut bound = self.root.transaction.lock().map_err(|_| CLOSED)?;
        if bound.is_some() {
            return Err("Storage transaction is already bound");
        }
        *bound = Some(ticket);
        Ok(())
    }
    pub fn require_bound(&self, write: bool) -> Result<TransactionRef> {
        if write {
            self.require_write()?;
        } else {
            self.check()?;
        }
        let ticket = self
            .root
            .transaction
            .lock()
            .map_err(|_| CLOSED)?
            .clone()
            .ok_or("Storage transaction is not bound")?;
        ticket.check().map_err(|_| CLOSED)?;
        Ok(ticket)
    }
    pub fn writable(&self) -> Result<bool> {
        self.check()?;
        Ok(self.write)
    }
    /// Consumption of an already admitted result retains both the logical
    /// owner and the original physical transaction, without actor round trips.
    pub fn check_bound(&self) -> Result<()> {
        self.check()?;
        self.root
            .transaction
            .lock()
            .map_err(|_| CLOSED)?
            .as_ref()
            .ok_or("Storage transaction is not bound")?
            .check_current()
            .map_err(|_| CLOSED)
    }
    pub fn require_write(&self) -> Result<()> {
        self.check()?;
        if self.write {
            Ok(())
        } else {
            Err("Storage transaction is read-only")
        }
    }
    pub(crate) fn participant(&self, handle: &ScopedHandle) -> Result<()> {
        self.require_write()?;
        handle.check().map_err(|_| CLOSED)?;
        let mut participants = self.root.participants.lock().map_err(|_| CLOSED)?;
        if !participants.iter().any(|value| value.same_scope(handle)) {
            participants.push(handle.clone());
        }
        Ok(())
    }
    pub fn commit_ready(&self) -> Result<()> {
        if let Some(ticket) = self.root.transaction.lock().map_err(|_| CLOSED)?.as_ref() {
            ticket.check().map_err(|_| CLOSED)?;
        }
        let _gate = COMMIT_GATE.lock().map_err(|_| CLOSED)?;
        self.check()?;
        if self.parent.is_some() {
            return Err("Only the root transaction can commit");
        }
        for writer in self.root.writers.lock().map_err(|_| CLOSED)?.iter() {
            writer.check()?;
        }
        for handle in self.root.participants.lock().map_err(|_| CLOSED)?.iter() {
            handle.check().map_err(|_| CLOSED)?;
        }
        self.root.active.store(false, Ordering::Release);
        Ok(())
    }
    pub fn close(&self) {
        self.active.store(false, Ordering::Release);
        if self.parent.is_none() {
            self.root.active.store(false, Ordering::Release);
        }
    }
    /// Participants share one physical transaction; a failed participant poisons
    /// the root even if an upper caller catches its exception. No savepoint exists.
    /// A handle that ended itself already passed its final check, and every later
    /// statement is rejected before execution, so its rejection leaves the root
    /// intact. A live handle whose parent ended still poisons the root.
    pub fn abort(&self) {
        let _gate = COMMIT_GATE
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if self.active.load(Ordering::Acquire) {
            self.root.active.store(false, Ordering::Release);
        }
    }
    pub fn authorize(&self, statement: &Statement) -> Result<()> {
        self.check()?;
        if statement.raw {
            return Err("Raw SQL is not part of the storage contract");
        }
        if statement.undeclared {
            return Err("Only declared table objects are supported");
        }
        if !matches!(
            statement.operation.as_str(),
            "select" | "insert" | "update" | "delete"
        ) {
            return Err("Only structured queries and row mutations are supported");
        }
        if statement.operation != "select" && statement.writes.is_empty() {
            return Err("Storage mutation requires a declared target");
        }
        if statement
            .writes
            .iter()
            .any(|id| !self.write || !self.scope.writable.contains(id))
        {
            return Err("Storage write is outside the granted tables");
        }
        if statement
            .reads
            .iter()
            .any(|id| !self.scope.readable.contains(id))
        {
            return Err("Storage read is outside the granted tables");
        }
        Ok(())
    }
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Statement {
    pub operation: String,
    pub reads: Vec<u64>,
    pub writes: Vec<u64>,
    pub raw: bool,
    pub undeclared: bool,
}

#[derive(Clone, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Column {
    pub name: String,
    pub kind: String,
    pub nullable: bool,
}
#[derive(Clone, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct ForeignKey {
    pub columns: Vec<String>,
    pub table: String,
    pub references: Vec<String>,
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Schema {
    pub name: String,
    pub columns: Vec<Column>,
    pub primary: BTreeSet<String>,
    pub unique: BTreeSet<BTreeSet<String>>,
    pub foreign: BTreeSet<ForeignKey>,
}

/// Read-only preflight of all declarations, before the adapter executes any DDL.
pub fn preflight(expected: &[Schema], actual: &[Schema]) -> Result<()> {
    let mut names = BTreeSet::new();
    for schema in expected {
        if !names.insert(&schema.name) {
            return Err("Duplicate storage ownership");
        }
        let Some(found) = actual.iter().find(|item| item.name == schema.name) else {
            continue;
        };
        let wanted: BTreeMap<_, _> = schema.columns.iter().map(|c| (&c.name, c)).collect();
        let present: BTreeMap<_, _> = found.columns.iter().map(|c| (&c.name, c)).collect();
        if wanted != present {
            return Err("Database columns do not match current contract");
        }
        if schema.primary != found.primary {
            return Err("Database key does not match current contract");
        }
        if !schema.unique.is_subset(&found.unique) {
            return Err("Database uniqueness does not match current contract");
        }
        if schema.foreign != found.foreign {
            return Err("Database references do not match current contract");
        }
    }
    Ok(())
}
pub fn ownership(core: &[String], owners: &[Vec<String>]) -> Result<()> {
    let mut names = BTreeSet::new();
    for name in core.iter().chain(owners.iter().flatten()) {
        if !names.insert(name) {
            return Err("Duplicate storage ownership");
        }
    }
    Ok(())
}
