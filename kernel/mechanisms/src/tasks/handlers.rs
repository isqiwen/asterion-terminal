//! Fixed task contribution declarations and worker request admission.
//!
//! Callback objects belong to the language adapter. A declaration never grants
//! access to another handler's operations or to the task control plane.

use crate::authority::Grant;
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::atomic::{AtomicBool, Ordering},
};

#[derive(Clone, Debug)]
pub struct Declaration {
    kind: String,
    publish_suffix: String,
    requests: Vec<Grant>,
}

fn identifier(value: &str, separator: u8) -> bool {
    value.as_bytes().first().is_some_and(u8::is_ascii_lowercase)
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == separator)
}

fn control_operation(value: &str) -> bool {
    matches!(value, "claim" | "heartbeat" | "fail" | "cancel")
}

impl Declaration {
    pub fn new(kind: String, publish_suffix: String, requests: Vec<Grant>) -> Result<Self, String> {
        if !kind.contains('.') || !kind.split('.').all(|part| identifier(part, b'_')) {
            return Err("Task handler kind must be a namespaced ID".into());
        }
        if !publish_suffix
            .strip_prefix('/')
            .is_some_and(|part| identifier(part, b'-'))
        {
            return Err("Task handler must publish to a job-relative endpoint".into());
        }
        for grant in &requests {
            let relative = grant.path.strip_prefix("/jobs/:id/");
            let valid_path = relative.is_some_and(|relative| {
                let mut parts = relative.split('/');
                parts.next().is_some_and(|part| identifier(part, b'-'))
                    && parts.all(|part| part == ":id" || identifier(part, b'-'))
            });
            if !valid_path || grant.methods != ["POST"] || grant.descendants {
                return Err("Worker requests must be exact job-relative POST operations".into());
            }
        }
        if control_operation(&publish_suffix[1..])
            || requests.iter().any(|grant| {
                control_operation(
                    grant
                        .path
                        .trim_start_matches("/jobs/:id/")
                        .split('/')
                        .next()
                        .unwrap_or(""),
                )
            })
        {
            return Err("Task control operations cannot be contributed by a handler".into());
        }
        Ok(Self {
            kind,
            publish_suffix,
            requests,
        })
    }

    pub fn kind(&self) -> &str {
        &self.kind
    }

    pub fn requests(&self) -> RequestScope {
        RequestScope {
            grants: self.requests.clone(),
            active: AtomicBool::new(true),
        }
    }
}

/// Per-execution port. Closing it rejects retained callbacks; the host owns the
/// HTTP call and the current job/lease, neither is supplied by a plugin.
pub struct RequestScope {
    grants: Vec<Grant>,
    active: AtomicBool,
}

impl RequestScope {
    pub fn authorize(&self, suffix: &str) -> Result<(), String> {
        if !self.active.load(Ordering::Acquire) {
            return Err("Execution transport is closed".into());
        }
        // Grant::allows rejects escapes, empty/dot segments and URL metacharacters.
        // The fixed prefix binds :id to the host's current job, not a caller ID.
        let path = format!("/jobs/current{suffix}");
        if !suffix.starts_with('/') || !self.grants.iter().any(|grant| grant.allows("POST", &path))
        {
            return Err("Job-relative operation is not granted to this task handler".into());
        }
        Ok(())
    }

    pub fn close(&self) {
        self.active.store(false, Ordering::Release);
    }
}

#[derive(Default)]
pub struct Registry {
    indices: BTreeMap<String, usize>,
    declarations: Vec<Declaration>,
}

impl Registry {
    pub fn register(&mut self, declaration: Declaration) -> Result<usize, String> {
        let kind = declaration.kind();
        if self.indices.contains_key(kind) {
            return Err(format!("Duplicate task handler or invalid ID: {kind}"));
        }
        let index = self.declarations.len();
        self.indices.insert(kind.to_owned(), index);
        self.declarations.push(declaration);
        Ok(index)
    }

    pub fn get(&self, kind: &str) -> Result<usize, String> {
        self.indices
            .get(kind)
            .copied()
            .ok_or_else(|| format!("Unsupported job kind: {kind}"))
    }

    /// Server transport grants come only from validated declarations. The
    /// control operations remain host-owned and are not added to RequestScope.
    pub fn worker_grants(&self) -> Vec<Grant> {
        let mut paths: BTreeSet<String> = ["/jobs/claim", "/jobs/:id/heartbeat", "/jobs/:id/fail"]
            .into_iter()
            .map(str::to_owned)
            .collect();
        for declaration in &self.declarations {
            paths.insert(format!("/jobs/:id{}", declaration.publish_suffix));
            paths.extend(declaration.requests.iter().map(|grant| grant.path.clone()));
        }
        paths
            .into_iter()
            .map(|path| Grant {
                path,
                methods: vec!["POST".into()],
                descendants: false,
            })
            .collect()
    }
}
