//! Fixed process-set supervision. Product roles, database setup and health queries
//! are assembled above this module; no feature decides restart or cleanup rules.
use crate::{files::atomic_write, processes};
use nix::sys::signal::Signal;
use serde::Deserialize;
use serde_json::{Map, Value, json};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProcessSpec {
    pub name: String,
    pub program: PathBuf,
    pub arguments: Vec<String>,
    pub environment: BTreeMap<String, String>,
    pub cwd: PathBuf,
}
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Policy {
    pub restart_millis: u64,
    pub snapshot_millis: u64,
    pub shutdown_millis: u64,
}
impl Default for Policy {
    fn default() -> Self {
        Self {
            restart_millis: 3000,
            snapshot_millis: 500,
            shutdown_millis: 10000,
        }
    }
}
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Configuration {
    pub processes: Vec<ProcessSpec>,
    pub status_file: PathBuf,
    pub build_id: String,
    pub policy: Policy,
}
#[derive(Debug)]
pub enum Error<E> {
    Mechanism(&'static str),
    Callback(E),
}

pub struct Supervisor {
    configuration: Configuration,
    stopping: AtomicBool,
    started: AtomicBool,
}
struct Slot {
    child: Option<Child>,
    last_start: Option<Instant>,
}
struct Children {
    slots: Vec<Slot>,
    grace: Duration,
    status_file: PathBuf,
    published: bool,
}
impl Children {
    fn shutdown(&mut self) {
        for slot in &self.slots {
            if let Some(child) = &slot.child {
                let _ = processes::signal(child, Signal::SIGTERM);
            }
        }
        let deadline = Instant::now() + self.grace;
        loop {
            let mut running = false;
            for slot in &mut self.slots {
                if let Some(child) = &mut slot.child {
                    match child.try_wait() {
                        Ok(Some(_)) => {
                            // Descendants can retain the group after its leader exits.
                            let _ = processes::signal(child, Signal::SIGKILL);
                            slot.child = None;
                        }
                        Ok(None) | Err(_) => running = true,
                    }
                }
            }
            if !running || Instant::now() >= deadline {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        for slot in &mut self.slots {
            if let Some(mut child) = slot.child.take() {
                let _ = processes::signal(&child, Signal::SIGKILL);
                let _ = child.wait();
            }
        }
    }
}
impl Drop for Children {
    fn drop(&mut self) {
        self.shutdown();
        if self.published {
            let _ = fs::remove_file(&self.status_file);
        }
    }
}

impl Supervisor {
    pub fn new(configuration: Configuration) -> Result<Self, &'static str> {
        let mut names = BTreeSet::new();
        if configuration.processes.is_empty()
            || configuration.processes.len() > 32
            || !configuration.status_file.is_absolute()
            || configuration.build_id.len() != 64
            || !configuration
                .build_id
                .bytes()
                .all(|b| b.is_ascii_hexdigit())
        {
            return Err("Invalid supervisor configuration");
        }
        for millis in [
            configuration.policy.restart_millis,
            configuration.policy.snapshot_millis,
            configuration.policy.shutdown_millis,
        ] {
            if millis == 0 || millis > 120_000 {
                return Err("Invalid supervisor timing policy");
            }
        }
        for process in &configuration.processes {
            if process.name.is_empty()
                || process.name.len() > 64
                || !process
                    .name
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
                || matches!(process.name.as_str(), "observed_at" | "build_id")
                || !names.insert(process.name.clone())
                || !process.program.is_absolute()
                || !process.cwd.is_absolute()
                || process.arguments.len() > 256
                || process.environment.len() > 1024
            {
                return Err("Invalid supervised process declaration");
            }
        }
        Ok(Self {
            configuration,
            stopping: AtomicBool::new(false),
            started: AtomicBool::new(false),
        })
    }
    pub fn request_stop(&self) {
        self.stopping.store(true, Ordering::Release);
    }
    pub fn run<E>(&self, heartbeat: &mut impl FnMut() -> Result<(), E>) -> Result<(), Error<E>> {
        if self.started.swap(true, Ordering::AcqRel) {
            return Err(Error::Mechanism("Supervisor can only run once"));
        }
        let configuration = &self.configuration;
        let mut children = Children {
            slots: configuration
                .processes
                .iter()
                .map(|_| Slot {
                    child: None,
                    last_start: None,
                })
                .collect(),
            grace: Duration::from_millis(configuration.policy.shutdown_millis),
            status_file: configuration.status_file.clone(),
            published: false,
        };
        let mut snapshot_at = Instant::now();
        while !self.stopping.load(Ordering::Acquire) {
            heartbeat().map_err(Error::Callback)?;
            if self.stopping.load(Ordering::Acquire) {
                break;
            }
            let now = Instant::now();
            for (slot, process) in children.slots.iter_mut().zip(&configuration.processes) {
                if let Some(child) = &mut slot.child
                    && child
                        .try_wait()
                        .map_err(|_| Error::Mechanism("Cannot inspect supervised process"))?
                        .is_some()
                {
                    processes::signal(child, Signal::SIGKILL)
                        .map_err(|_| Error::Mechanism("Cannot reclaim supervised process group"))?;
                    slot.child = None;
                }
                if slot.child.is_none()
                    && slot.last_start.is_none_or(|started| {
                        now.duration_since(started)
                            >= Duration::from_millis(configuration.policy.restart_millis)
                    })
                {
                    let mut command = Command::new(&process.program);
                    command
                        .args(&process.arguments)
                        .env_clear()
                        .envs(&process.environment)
                        .current_dir(&process.cwd)
                        .stdin(Stdio::null())
                        .stdout(Stdio::inherit())
                        .stderr(Stdio::inherit());
                    slot.child = Some(
                        processes::spawn(command)
                            .map_err(|_| Error::Mechanism("Cannot start supervised process"))?,
                    );
                    slot.last_start = Some(Instant::now());
                }
            }
            if now >= snapshot_at {
                let mut snapshot: Map<String, Value> = children
                    .slots
                    .iter()
                    .zip(&configuration.processes)
                    .map(|(slot, process)| {
                        (
                            process.name.clone(),
                            json!(if slot.child.is_some() {
                                "running"
                            } else {
                                "stopped"
                            }),
                        )
                    })
                    .collect();
                let observed_at = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map_err(|_| Error::Mechanism("Invalid supervisor clock"))?
                    .as_secs_f64();
                snapshot.insert("observed_at".into(), json!(observed_at));
                snapshot.insert("build_id".into(), json!(configuration.build_id));
                let bytes = serde_json::to_vec(&snapshot)
                    .map_err(|_| Error::Mechanism("Invalid supervisor snapshot"))?;
                atomic_write(&configuration.status_file, &bytes, true)
                    .map_err(|_| Error::Mechanism("Cannot publish supervisor snapshot"))?;
                children.published = true;
                snapshot_at = now + Duration::from_millis(configuration.policy.snapshot_millis);
            }
            // Bounded signal delivery even when no process transition is due.
            std::thread::sleep(Duration::from_millis(
                configuration.policy.snapshot_millis.min(100),
            ));
        }
        Ok(())
    }
}
