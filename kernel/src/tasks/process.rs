//! One-shot computation processes with separately bounded binary artifacts.
//! The opaque request and metadata belong to the caller, never to task policy.

use crate::{communication, processes};
use asterion_foundation::{communication::parse_json, wire_generated::Context};
use nix::{
    errno::Errno,
    fcntl::{FcntlArg, OFlag, fcntl},
    poll::{PollFd, PollFlags, PollTimeout, poll},
    sys::signal::Signal,
};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::{
    env, fmt,
    fs::File,
    io::{self, Read, Seek, SeekFrom, Write},
    os::fd::AsFd,
    process::{Child, ChildStderr, ChildStdin, ChildStdout, Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

const CHUNK: usize = 65_536;
const ENVELOPE: u64 = 4096;

#[derive(Debug)]
pub enum Failure {
    Mechanism(&'static str),
    Task(String),
}
impl Failure {
    pub fn code(&self) -> &'static str {
        match self {
            Self::Mechanism(code) => code,
            Self::Task(_) => "task_failed",
        }
    }
}
impl fmt::Display for Failure {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Task(message) => out.write_str(message),
            Self::Mechanism(code) => out.write_str(match *code {
                "timeout" => "Task computation deadline exceeded",
                "input_limit" => "Task input exceeds its byte budget",
                "output_limit" => "Task output exceeds its byte budget",
                "closed" => "Task computation is closed",
                "busy" => "Task computation is already being polled",
                "startup" => "Cannot start task computation",
                "process_exit" => "Task computation process failed",
                _ => "Invalid task computation protocol",
            }),
        }
    }
}
impl std::error::Error for Failure {}
type Result<T> = std::result::Result<T, Failure>;
fn fault(code: &'static str) -> Failure {
    Failure::Mechanism(code)
}
fn io_failure(_: io::Error) -> Failure {
    fault("process_exit")
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Limits {
    pub input_bytes: u64,
    pub artifact_bytes: u64,
    pub metadata_bytes: u64,
    pub stderr_bytes: u64,
}
impl Limits {
    fn validate(&self) -> Result<()> {
        if self.input_bytes == 0
            || self.artifact_bytes == 0
            || self.input_bytes > 8 * 1024 * 1024 * 1024
            || self.artifact_bytes > 8 * 1024 * 1024 * 1024
            || !(2..=16 * 1024 * 1024).contains(&self.metadata_bytes)
            || !(1..=16 * 1024 * 1024).contains(&self.stderr_bytes)
        {
            return Err(fault("protocol"));
        }
        Ok(())
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RequestHeader {
    context: Context,
    input_bytes: u64,
    limits: Limits,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReplyHeader {
    context: Context,
    content_bytes: u64,
    metadata: Map<String, Value>,
    #[serde(deserialize_with = "required_error")]
    error: Option<String>,
}

fn required_error<'de, D: serde::Deserializer<'de>>(
    value: D,
) -> std::result::Result<Option<String>, D::Error> {
    Option::<String>::deserialize(value)
}

fn active(context: &Context) -> Result<()> {
    communication::ensure_active(context).map_err(|_| fault("timeout"))
}
fn encode<T: Serialize>(value: &T, limit: u64) -> Result<Vec<u8>> {
    let bytes = serde_json::to_vec(value).map_err(|_| fault("protocol"))?;
    if bytes.len() as u64 > limit {
        return Err(fault("output_limit"));
    }
    Ok(bytes)
}
fn nonblocking(fd: &impl AsFd) -> Result<()> {
    let flags = fcntl(fd, FcntlArg::F_GETFL).map_err(|_| fault("startup"))?;
    fcntl(
        fd,
        FcntlArg::F_SETFL(OFlag::from_bits_truncate(flags) | OFlag::O_NONBLOCK),
    )
    .map_err(|_| fault("startup"))?;
    Ok(())
}

pub struct Artifact {
    file: Option<File>,
    metadata: Map<String, Value>,
}
impl Artifact {
    pub fn metadata(&self) -> &Map<String, Value> {
        &self.metadata
    }
    pub fn read_chunk(&mut self) -> Result<Vec<u8>> {
        let mut bytes = vec![0; CHUNK];
        let count = self
            .file
            .as_mut()
            .ok_or_else(|| fault("closed"))?
            .read(&mut bytes)
            .map_err(io_failure)?;
        bytes.truncate(count);
        Ok(bytes)
    }
    pub fn close(&mut self) {
        self.file.take();
    }
}

struct Receiver {
    prefix: Vec<u8>,
    header_bytes: Vec<u8>,
    header: Option<ReplyHeader>,
    file: Option<File>,
    received: u64,
}
impl Receiver {
    fn new() -> Result<Self> {
        Ok(Self {
            prefix: Vec::with_capacity(8),
            header_bytes: Vec::new(),
            header: None,
            file: Some(tempfile::tempfile().map_err(|_| fault("startup"))?),
            received: 0,
        })
    }
    fn receive(&mut self, mut bytes: &[u8], context: &Context, limits: &Limits) -> Result<()> {
        while !bytes.is_empty() {
            if self.prefix.len() < 8 {
                let count = bytes.len().min(8 - self.prefix.len());
                self.prefix.extend_from_slice(&bytes[..count]);
                bytes = &bytes[count..];
                continue;
            }
            let size =
                u64::from_be_bytes(self.prefix.as_slice().try_into().expect("8-byte prefix"));
            if size == 0 || size > limits.metadata_bytes + ENVELOPE {
                return Err(fault("output_limit"));
            }
            if self.header.is_none() {
                let count = bytes.len().min(size as usize - self.header_bytes.len());
                self.header_bytes.extend_from_slice(&bytes[..count]);
                bytes = &bytes[count..];
                if self.header_bytes.len() < size as usize {
                    continue;
                }
                let header: ReplyHeader = serde_json::from_value(
                    parse_json(&self.header_bytes).map_err(|_| fault("protocol"))?,
                )
                .map_err(|_| fault("protocol"))?;
                if header.content_bytes > limits.artifact_bytes {
                    return Err(fault("output_limit"));
                }
                encode(&header.metadata, limits.metadata_bytes)?;
                if serde_json::to_value(&header.context).ok() != serde_json::to_value(context).ok()
                    || header
                        .error
                        .as_ref()
                        .is_some_and(|error| error.len() > 2000 || header.content_bytes != 0)
                {
                    return Err(fault("protocol"));
                }
                self.header = Some(header);
            }
            if !bytes.is_empty() {
                self.received = self
                    .received
                    .checked_add(bytes.len() as u64)
                    .ok_or_else(|| fault("output_limit"))?;
                if self.received > self.header.as_ref().expect("parsed header").content_bytes {
                    return Err(fault("protocol"));
                }
                self.file
                    .as_mut()
                    .expect("owned spool")
                    .write_all(bytes)
                    .map_err(io_failure)?;
                bytes = &[];
            }
        }
        Ok(())
    }
    fn finish(&mut self) -> Result<Artifact> {
        let header = self.header.take().ok_or_else(|| fault("protocol"))?;
        if self.received != header.content_bytes {
            return Err(fault("protocol"));
        }
        if let Some(error) = header.error {
            return Err(Failure::Task(error));
        }
        let mut file = self.file.take().ok_or_else(|| fault("closed"))?;
        file.seek(SeekFrom::Start(0)).map_err(io_failure)?;
        Ok(Artifact {
            file: Some(file),
            metadata: header.metadata,
        })
    }
}

pub struct TaskProcess {
    child: Child,
    input: Option<ChildStdin>,
    output: ChildStdout,
    errors: ChildStderr,
    request: Vec<u8>,
    sent: usize,
    receiver: Receiver,
    context: Context,
    limits: Limits,
    deadline: Instant,
    stderr_bytes: u64,
    output_closed: bool,
    errors_closed: bool,
    reaped: bool,
    ready: bool,
    closed: bool,
}
impl TaskProcess {
    pub fn spawn(
        mut command: Command,
        context: Context,
        input: &[u8],
        limits: Limits,
    ) -> Result<Self> {
        limits.validate()?;
        active(&context)?;
        if input.len() as u64 > limits.input_bytes {
            return Err(fault("input_limit"));
        }
        let deadline = Instant::now()
            .checked_add(communication::remaining(&context).map_err(|_| fault("timeout"))?)
            .ok_or_else(|| fault("timeout"))?;
        let header = encode(
            &RequestHeader {
                context: context.clone(),
                input_bytes: input.len() as u64,
                limits: limits.clone(),
            },
            ENVELOPE,
        )?;
        let mut request = Vec::with_capacity(8 + header.len() + input.len());
        request.extend_from_slice(&(header.len() as u64).to_be_bytes());
        request.extend_from_slice(&header);
        request.extend_from_slice(input);
        let receiver = Receiver::new()?;
        command.env_clear();
        for key in ["PATH", "LANG", "LC_ALL", "SYSTEMROOT", "TMPDIR"] {
            if let Some(value) = env::var_os(key) {
                command.env(key, value);
            }
        }
        command.env("PYTHONDONTWRITEBYTECODE", "1");
        command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = processes::spawn(command).map_err(|_| fault("startup"))?;
        let process = Self {
            input: child.stdin.take(),
            output: child.stdout.take().expect("piped stdout"),
            errors: child.stderr.take().expect("piped stderr"),
            child,
            request,
            sent: 0,
            receiver,
            context,
            limits,
            deadline,
            stderr_bytes: 0,
            output_closed: false,
            errors_closed: false,
            reaped: false,
            ready: false,
            closed: false,
        };
        nonblocking(process.input.as_ref().expect("piped stdin"))?;
        nonblocking(&process.output)?;
        nonblocking(&process.errors)?;
        Ok(process)
    }

    pub fn poll(
        &mut self,
        wait: Duration,
        stopped: &AtomicBool,
        interrupt: &mut impl FnMut() -> bool,
    ) -> Result<bool> {
        let result = self.poll_inner(wait, stopped, interrupt);
        if result.is_err() {
            self.close();
        }
        result
    }
    fn poll_inner(
        &mut self,
        wait: Duration,
        stopped: &AtomicBool,
        interrupt: &mut impl FnMut() -> bool,
    ) -> Result<bool> {
        if wait > Duration::from_secs(60) {
            return Err(fault("protocol"));
        }
        let end = Instant::now() + wait;
        loop {
            if self.closed || stopped.load(Ordering::Acquire) || interrupt() {
                return Err(fault("closed"));
            }
            active(&self.context)?;
            if Instant::now() >= self.deadline {
                return Err(fault("timeout"));
            }
            if self.ready {
                return Ok(true);
            }
            if let Some(input) = self.input.as_mut() {
                let until = self.request.len().min(self.sent + CHUNK);
                match input.write(&self.request[self.sent..until]) {
                    Ok(0) => return Err(fault("process_exit")),
                    Ok(count) => self.sent += count,
                    Err(error)
                        if matches!(
                            error.kind(),
                            io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                        ) => {}
                    Err(_) => return Err(fault("process_exit")),
                }
                if self.sent == self.request.len() {
                    self.input.take();
                    self.request = Vec::new();
                }
            }
            let mut buffer = [0; CHUNK];
            if !self.output_closed {
                match self.output.read(&mut buffer) {
                    Ok(0) => self.output_closed = true,
                    Ok(count) => {
                        self.receiver
                            .receive(&buffer[..count], &self.context, &self.limits)?
                    }
                    Err(error)
                        if matches!(
                            error.kind(),
                            io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                        ) => {}
                    Err(_) => return Err(fault("process_exit")),
                }
            }
            if !self.errors_closed {
                match self.errors.read(&mut buffer) {
                    Ok(0) => self.errors_closed = true,
                    Ok(count) => {
                        self.stderr_bytes += count as u64;
                        if self.stderr_bytes > self.limits.stderr_bytes {
                            return Err(fault("output_limit"));
                        }
                    }
                    Err(error)
                        if matches!(
                            error.kind(),
                            io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                        ) => {}
                    Err(_) => return Err(fault("process_exit")),
                }
            }
            if !self.reaped
                && let Some(status) = self.child.try_wait().map_err(io_failure)?
            {
                self.reaped = true;
                // The leader is done: its descendants must not retain our pipes.
                processes::signal(&self.child, Signal::SIGKILL).map_err(io_failure)?;
                if !status.success() || self.input.is_some() {
                    return Err(fault("process_exit"));
                }
            }
            if self.reaped && self.output_closed && self.errors_closed {
                self.ready = true;
                return Ok(true);
            }
            if Instant::now() >= end {
                return Ok(false);
            }
            let mut descriptors = Vec::new();
            if !self.output_closed {
                descriptors.push(PollFd::new(self.output.as_fd(), PollFlags::POLLIN));
            }
            if !self.errors_closed {
                descriptors.push(PollFd::new(self.errors.as_fd(), PollFlags::POLLIN));
            }
            if let Some(input) = self.input.as_ref() {
                descriptors.push(PollFd::new(input.as_fd(), PollFlags::POLLOUT));
            }
            let duration =
                Duration::from_millis(10).min(end.saturating_duration_since(Instant::now()));
            match poll(
                &mut descriptors,
                PollTimeout::try_from(duration).map_err(|_| fault("protocol"))?,
            ) {
                Ok(_) | Err(Errno::EINTR) => (),
                Err(_) => return Err(fault("process_exit")),
            }
        }
    }
    pub fn take_result(&mut self) -> Result<Artifact> {
        let result = if self.closed || !self.ready {
            Err(fault("closed"))
        } else if Instant::now() >= self.deadline {
            Err(fault("timeout"))
        } else {
            active(&self.context).and_then(|_| self.receiver.finish())
        };
        self.close();
        result
    }
    pub fn close(&mut self) {
        if self.closed {
            return;
        }
        self.closed = true;
        self.input.take();
        self.request = Vec::new();
        self.receiver.file.take();
        let _ = processes::signal(&self.child, Signal::SIGKILL);
        let _ = self.child.wait();
    }
}
impl Drop for TaskProcess {
    fn drop(&mut self) {
        self.close();
    }
}

pub struct ComputationResult {
    pub content: Vec<u8>,
    pub metadata: Map<String, Value>,
    pub error: Option<String>,
}

/// Trusted child adapter; computation sees opaque input, framing stays fixed.
pub fn serve(
    reader: &mut impl Read,
    writer: &mut impl Write,
    dispatch: impl FnOnce(&[u8]) -> ComputationResult,
) -> Result<()> {
    let mut prefix = [0; 8];
    reader.read_exact(&mut prefix).map_err(io_failure)?;
    let size = u64::from_be_bytes(prefix);
    if size == 0 || size > ENVELOPE {
        return Err(fault("protocol"));
    }
    let mut header = vec![0; size as usize];
    reader.read_exact(&mut header).map_err(io_failure)?;
    let header: RequestHeader =
        serde_json::from_value(parse_json(&header).map_err(|_| fault("protocol"))?)
            .map_err(|_| fault("protocol"))?;
    header.limits.validate()?;
    active(&header.context)?;
    if header.input_bytes > header.limits.input_bytes {
        return Err(fault("input_limit"));
    }
    let mut input = vec![0; header.input_bytes as usize];
    reader.read_exact(&mut input).map_err(io_failure)?;
    if reader.read(&mut [0]).map_err(io_failure)? != 0 {
        return Err(fault("protocol"));
    }
    let result = dispatch(&input);
    active(&header.context)?;
    if result.content.len() as u64 > header.limits.artifact_bytes {
        return Err(fault("output_limit"));
    }
    encode(&result.metadata, header.limits.metadata_bytes)?;
    if result
        .error
        .as_ref()
        .is_some_and(|error| error.len() > 2000 || !result.content.is_empty())
    {
        return Err(fault("protocol"));
    }
    let reply = encode(
        &ReplyHeader {
            context: header.context,
            content_bytes: result.content.len() as u64,
            metadata: result.metadata,
            error: result.error,
        },
        header.limits.metadata_bytes + ENVELOPE,
    )?;
    writer
        .write_all(&(reply.len() as u64).to_be_bytes())
        .map_err(io_failure)?;
    writer.write_all(&reply).map_err(io_failure)?;
    writer.write_all(&result.content).map_err(io_failure)?;
    writer.flush().map_err(io_failure)
}

#[cfg(test)]
#[path = "process_tests.rs"]
mod tests;
