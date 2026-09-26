//! Application tables of the Rust services through the fixed kernel database
//! pool (L3). SQL is written with `?` placeholders; PostgreSQL receives `$n`.
//! Calls block on the kernel's connection actor, so async handlers run them
//! on the blocking thread pool.
use asterion_kernel::database::{self, Connection, Database};
use asterion_kernel::storage::{self, Column, ForeignKey, Schema};
use serde_json::Value;
use std::collections::BTreeSet;

#[derive(Clone, Copy)]
pub enum Kind {
    Text,
    Integer,
    Float,
    Boolean,
    /// JSON documents, bound and read as their text.
    Json,
}
impl Kind {
    fn sql(self) -> &'static str {
        match self {
            Self::Text => "VARCHAR",
            Self::Integer => "INTEGER",
            Self::Float => "FLOAT",
            Self::Boolean => "BOOLEAN",
            Self::Json => "JSON",
        }
    }
}

/// A table owned by a Rust service, declared once for DDL and preflight.
pub struct Table {
    pub name: &'static str,
    pub columns: &'static [(&'static str, Kind)],
    pub primary: &'static [&'static str],
    pub unique: &'static [&'static [&'static str]],
}
impl Table {
    fn create(&self) -> String {
        let mut parts: Vec<String> = self
            .columns
            .iter()
            .map(|(name, kind)| format!("{name} {} NOT NULL", kind.sql()))
            .collect();
        parts.push(format!("PRIMARY KEY ({})", self.primary.join(", ")));
        parts.extend(
            self.unique
                .iter()
                .map(|columns| format!("UNIQUE ({})", columns.join(", "))),
        );
        format!("CREATE TABLE {} ({})", self.name, parts.join(", "))
    }
    fn schema(&self) -> Schema {
        Schema {
            name: self.name.into(),
            columns: self
                .columns
                .iter()
                .map(|(name, kind)| Column {
                    name: (*name).into(),
                    kind: kind.sql().into(),
                    nullable: false,
                })
                .collect(),
            primary: self.primary.iter().map(|c| (*c).to_string()).collect(),
            unique: self
                .unique
                .iter()
                .map(|columns| columns.iter().map(|c| (*c).to_string()).collect())
                .collect(),
            foreign: BTreeSet::new(),
        }
    }
}

/// Database spellings of the same current column types.
fn normalized(kind: &str) -> String {
    let kind = kind.trim().to_ascii_uppercase();
    match kind.as_str() {
        "INT" | "INT4" | "INTEGER" => "INTEGER".into(),
        "FLOAT" | "FLOAT8" | "REAL" | "DOUBLE PRECISION" => "FLOAT".into(),
        "BOOL" | "BOOLEAN" => "BOOLEAN".into(),
        "TEXT" | "VARCHAR" | "CHARACTER VARYING" => "VARCHAR".into(),
        "JSON" => "JSON".into(),
        _ => kind,
    }
}

fn observed(schema: database::Schema) -> Schema {
    let mut unique: BTreeSet<BTreeSet<String>> = schema
        .unique
        .into_iter()
        .map(|columns| columns.into_iter().collect())
        .collect();
    unique.extend(
        schema
            .indexes
            .into_iter()
            .filter(|index| index.unique)
            .map(|index| index.columns.into_iter().collect()),
    );
    Schema {
        name: schema.name,
        columns: schema
            .columns
            .into_iter()
            .map(|column| Column {
                name: column.name,
                kind: normalized(&column.kind),
                nullable: column.nullable,
            })
            .collect(),
        primary: schema.primary.into_iter().collect(),
        unique,
        foreign: schema
            .foreign
            .into_iter()
            .map(|foreign| ForeignKey {
                columns: foreign.columns,
                table: foreign.table,
                references: foreign.references,
            })
            .collect(),
    }
}

pub struct Store {
    database: Database,
    postgres: bool,
}

pub struct Transaction<'a> {
    connection: &'a Connection,
    postgres: bool,
}

/// A failed database operation; `integrity` marks constraint violations.
#[derive(Debug)]
pub struct StoreError {
    pub integrity: bool,
    pub message: String,
}
impl std::fmt::Display for StoreError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}
impl From<database::Error> for StoreError {
    fn from(error: database::Error) -> Self {
        Self {
            integrity: error.code == "integrity",
            message: error.message.into(),
        }
    }
}
impl From<String> for StoreError {
    fn from(message: String) -> Self {
        Self {
            integrity: false,
            message,
        }
    }
}
impl From<&'static str> for StoreError {
    fn from(message: &'static str) -> Self {
        message.to_string().into()
    }
}

impl Store {
    pub fn open(url: &str) -> Result<Self, StoreError> {
        let config = database::config_from_url(url)?;
        let postgres = config["backend"] == "postgresql";
        let capacity = if config["path"] == ":memory:" { 1 } else { 8 };
        Ok(Self {
            database: Database::new(config.to_string(), capacity, 30.0)?,
            postgres,
        })
    }

    /// Run `work` in one transaction. A write transaction takes SQLite's writer
    /// lock up front; PostgreSQL rows are locked with `Transaction::locked`.
    /// The transaction commits when `work` returns Ok and rolls back otherwise.
    pub fn transaction<T, E: From<StoreError>>(
        &self,
        write: bool,
        work: impl FnOnce(&Transaction) -> Result<T, E>,
    ) -> Result<T, E> {
        let lease = self.database.connect().map_err(StoreError::from)?;
        let connection = lease.connection().map_err(StoreError::from)?;
        if write {
            connection.begin_write().map_err(StoreError::from)?;
        } else {
            connection.begin().map_err(StoreError::from)?;
        }
        let transaction = Transaction {
            connection,
            postgres: self.postgres,
        };
        match work(&transaction) {
            Ok(value) => {
                connection.commit().map_err(StoreError::from)?;
                Ok(value)
            }
            Err(error) => {
                let _ = connection.rollback();
                Err(error)
            }
        }
    }

    /// Identity of the pool, required by kernel repositories.
    pub fn database_id(&self) -> u64 {
        self.database.id()
    }

    /// Verify existing tables against the declarations, then create missing
    /// ones. A mismatch is refused without altering any table.
    pub fn ensure(&self, tables: &[Table]) -> Result<(), StoreError> {
        self.transaction(true, |transaction| {
            let actual: Vec<Schema> = transaction
                .connection
                .observe_schema()?
                .into_iter()
                .map(observed)
                .collect();
            let expected: Vec<Schema> = tables.iter().map(Table::schema).collect();
            storage::preflight(&expected, &actual)?;
            for table in tables {
                if !actual.iter().any(|schema| schema.name == table.name) {
                    transaction.execute(&table.create(), vec![])?;
                }
            }
            Ok(())
        })
    }
}

impl<'a> Transaction<'a> {
    /// Join the transaction a caller already opened on `connection` (for
    /// example the internal Python process); the caller commits or rolls back.
    pub fn over(connection: &'a Connection) -> Self {
        Self {
            connection,
            postgres: connection.is_postgres(),
        }
    }
    fn sql(&self, sql: &str) -> String {
        if !self.postgres {
            return sql.to_string();
        }
        let mut index = 0;
        let mut output = String::with_capacity(sql.len() + 8);
        for character in sql.chars() {
            if character == '?' {
                index += 1;
                output.push_str(&format!("${index}"));
            } else {
                output.push(character);
            }
        }
        output
    }
    /// The open kernel connection, for kernel repositories that take part in
    /// this transaction (tasks, events).
    pub fn connection(&self) -> &Connection {
        self.connection
    }
    pub fn is_postgres(&self) -> bool {
        self.postgres
    }
    /// Row locking suffix for statements reading rows the transaction updates.
    pub fn locked(&self) -> &'static str {
        if self.postgres { " FOR UPDATE" } else { "" }
    }
    pub fn rows(&self, sql: &str, params: Vec<Value>) -> Result<Vec<Vec<Value>>, StoreError> {
        let cursor = self.connection.execute(&self.sql(sql), params, false)?;
        let mut rows = Vec::new();
        loop {
            let batch = cursor.fetchmany(500)?;
            if batch.is_empty() {
                break;
            }
            rows.extend(batch);
        }
        cursor.close()?;
        Ok(rows)
    }
    pub fn execute(&self, sql: &str, params: Vec<Value>) -> Result<i64, StoreError> {
        let cursor = self.connection.execute(&self.sql(sql), params, false)?;
        let count = cursor.rowcount;
        cursor.close()?;
        Ok(count)
    }
}
