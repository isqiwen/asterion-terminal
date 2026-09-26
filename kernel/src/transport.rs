//! Fixed, bounded process transport. Business dispatch and package identity stay above L1.
use crate::communication::{Error as CommunicationError, decode_reply, ensure_active, remaining};
use asterion_foundation::{
    communication::{parse_json, validate},
    wire_generated::Context,
};
use nix::{
    errno::Errno,
    fcntl::{FcntlArg, OFlag, fcntl},
    poll::{PollFd, PollFlags, PollTimeout, poll},
    sys::{
        resource::{Resource, setrlimit},
        signal::Signal,
    },
};
use serde_json::{Value, json};
use std::{
    env, fmt,
    io::{self, BufRead, Read, Write},
    os::fd::AsFd,
    process::{Child, ChildStderr, ChildStdin, ChildStdout, Command, Output, Stdio},
    time::{Duration, Instant},
};

pub const BYTE_LIMIT: usize = crate::communication::MESSAGE_LIMIT;
const TICK: Duration = Duration::from_millis(10);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Failure(&'static str);
impl Failure {
    pub fn code(&self) -> &'static str {
        self.0
    }
    pub fn protocol() -> Self {
        Self("protocol")
    }
    pub fn startup() -> Self {
        Self("startup")
    }
}
impl fmt::Display for Failure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self.0 {
            "revoked" => "调用权限已撤销",
            "timeout" => "进程执行超时",
            "request_limit" => "通信请求超过限制",
            "output_limit" => "进程输出超过限制",
            "process_exit" => "进程失败，请检查通信契约和运行环境",
            "startup" => "无法启动进程，请检查运行环境",
            _ => "进程响应不符合当前通信契约",
        })
    }
}
impl std::error::Error for Failure {}

fn communication_failure(error: CommunicationError) -> Failure {
    if error == CommunicationError::Deadline {
        Failure("timeout")
    } else {
        Failure::protocol()
    }
}

fn response(context: &Context, raw: &[u8]) -> Result<Value, Failure> {
    decode_reply(context, raw).map_err(communication_failure)
}

fn nonblocking(fd: &impl AsFd) -> Result<(), Failure> {
    let flags = fcntl(fd, FcntlArg::F_GETFL).map_err(|_| Failure::startup())?;
    fcntl(
        fd,
        FcntlArg::F_SETFL(OFlag::from_bits_truncate(flags) | OFlag::O_NONBLOCK),
    )
    .map_err(|_| Failure::startup())?;
    Ok(())
}

fn deadline(context: &Context, lifetime: Option<Instant>) -> Result<Instant, Failure> {
    let remaining = remaining(context).map_err(communication_failure)?;
    let end = Instant::now()
        .checked_add(remaining)
        .ok_or(Failure("timeout"))?;
    Ok(lifetime.map_or(end, |limit| limit.min(end)))
}

fn check(
    context: &Context,
    end: Instant,
    authorized: &mut impl FnMut() -> bool,
) -> Result<(), Failure> {
    if !authorized() {
        return Err(Failure("revoked"));
    }
    if Instant::now() >= end {
        return Err(Failure("timeout"));
    }
    ensure_active(context).map_err(communication_failure)?;
    Ok(())
}

/// The trusted child bootstrap invokes this before importing package code.
pub fn child_limits() -> Result<(), Failure> {
    setrlimit(Resource::RLIMIT_FSIZE, BYTE_LIMIT as u64, BYTE_LIMIT as u64)
        .map_err(|_| Failure::startup())?;
    setrlimit(Resource::RLIMIT_CPU, 30, 30).map_err(|_| Failure::startup())?;
    Ok(())
}

fn isolate_environment(command: &mut Command) {
    command.env_clear();
    for key in ["PATH", "LANG", "LC_ALL", "SYSTEMROOT", "TMPDIR"] {
        if let Some(value) = env::var_os(key) {
            command.env(key, value);
        }
    }
    command.env("PYTHONDONTWRITEBYTECODE", "1");
}

struct Process {
    child: Child,
    input: Option<ChildStdin>,
    output: ChildStdout,
    errors: ChildStderr,
    bytes: usize,
    closed: bool,
    output_closed: bool,
    errors_closed: bool,
}
impl Process {
    fn spawn(mut command: Command, piped_input: bool) -> Result<Self, Failure> {
        if piped_input {
            command.stdin(Stdio::piped());
        }
        command.stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut child = crate::processes::spawn(command).map_err(|_| Failure::startup())?;
        // Stdio::piped guarantees these handles. Establish ownership before fallible setup.
        let input = child.stdin.take();
        let output = child.stdout.take().expect("piped stdout");
        let errors = child.stderr.take().expect("piped stderr");
        let process = Self {
            child,
            input,
            output,
            errors,
            bytes: 0,
            closed: false,
            output_closed: false,
            errors_closed: false,
        };
        if let Some(input) = &process.input {
            nonblocking(input)?;
        }
        nonblocking(&process.output)?;
        nonblocking(&process.errors)?;
        Ok(process)
    }
    fn close(&mut self) {
        if self.closed {
            return;
        }
        self.closed = true;
        self.input.take();
        // Kill the owned process group, including descendants retaining our pipes.
        // Independently supervised services have their own group and are untouched.
        let _ = crate::processes::signal(&self.child, Signal::SIGKILL);
        let _ = self.child.wait();
    }
    fn read(
        &mut self,
        stdout: &mut Vec<u8>,
        stderr: &mut Vec<u8>,
    ) -> Result<(bool, bool), Failure> {
        if !self.output_closed {
            self.output_closed = drain(&mut self.output, stdout, &mut self.bytes)?;
        }
        if !self.errors_closed {
            self.errors_closed = drain(&mut self.errors, stderr, &mut self.bytes)?;
        }
        Ok((self.output_closed, self.errors_closed))
    }
    fn wait(&self, pending: bool, end: Instant) -> Result<(), Failure> {
        let mut descriptors = Vec::with_capacity(3);
        if !self.output_closed {
            descriptors.push(PollFd::new(self.output.as_fd(), PollFlags::POLLIN));
        }
        if !self.errors_closed {
            descriptors.push(PollFd::new(self.errors.as_fd(), PollFlags::POLLIN));
        }
        if pending && let Some(input) = &self.input {
            descriptors.push(PollFd::new(input.as_fd(), PollFlags::POLLOUT));
        }
        let timeout =
            PollTimeout::try_from(TICK.min(end.saturating_duration_since(Instant::now())))
                .map_err(|_| Failure("timeout"))?;
        match poll(&mut descriptors, timeout) {
            Ok(_) | Err(Errno::EINTR) => Ok(()),
            Err(_) => Err(Failure("process_exit")),
        }
    }
    fn write(&mut self, pending: &mut &[u8]) -> Result<(), Failure> {
        if pending.is_empty() {
            return Ok(());
        }
        let input = self.input.as_mut().ok_or(Failure("process_exit"))?;
        match input.write(pending) {
            Ok(0) => Err(Failure("process_exit")),
            Ok(sent) => {
                *pending = &pending[sent..];
                Ok(())
            }
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                ) =>
            {
                Ok(())
            }
            Err(_) => Err(Failure("process_exit")),
        }
    }
}
impl Drop for Process {
    fn drop(&mut self) {
        self.close();
    }
}

fn drain(stream: &mut impl Read, output: &mut Vec<u8>, total: &mut usize) -> Result<bool, Failure> {
    let mut buffer = [0; 65_536];
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => return Ok(true),
            Ok(count) => {
                *total = total.checked_add(count).ok_or(Failure("output_limit"))?;
                if *total > BYTE_LIMIT {
                    return Err(Failure("output_limit"));
                }
                output.extend_from_slice(&buffer[..count]);
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(false),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return Err(Failure("process_exit")),
        }
    }
}

struct BoundedWriter {
    bytes: Vec<u8>,
    limit: usize,
}
impl Write for BoundedWriter {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > self.limit.saturating_sub(self.bytes.len()) {
            return Err(io::Error::other("message budget exceeded"));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn encode(value: &Value, newline: bool, code: &'static str) -> Result<Vec<u8>, Failure> {
    let mut writer = BoundedWriter {
        bytes: Vec::new(),
        limit: BYTE_LIMIT - usize::from(newline),
    };
    serde_json::to_writer(&mut writer, value).map_err(|_| Failure(code))?;
    if newline {
        writer.bytes.push(b'\n');
    }
    Ok(writer.bytes)
}
fn request_context(request: &Value) -> Result<Context, Failure> {
    validate("Call", request).map_err(|_| Failure::protocol())?;
    serde_json::from_value(request["context"].clone()).map_err(|_| Failure::protocol())
}

/// Check the wire byte budget before allocating the parsed request tree.
pub fn decode_request(raw: &[u8]) -> Result<Value, Failure> {
    if raw.len() > BYTE_LIMIT {
        return Err(Failure("request_limit"));
    }
    let value = parse_json(raw).map_err(|_| Failure::protocol())?;
    request_context(&value)?;
    Ok(value)
}

/// Python dispatch serializes business values; the fixed boundary bounds decoding.
pub fn decode_result(raw: &[u8]) -> Result<Value, Failure> {
    if raw.len() > BYTE_LIMIT {
        return Err(Failure("output_limit"));
    }
    parse_json(raw).map_err(|_| Failure::protocol())
}

/// One owner and one in-flight request per process. Bindings reject concurrent borrowing.
pub struct ProcessSession {
    process: Process,
    end: Instant,
    buffer: Vec<u8>,
}
impl ProcessSession {
    pub fn spawn(
        mut command: Command,
        lifetime: Duration,
        authorized: bool,
    ) -> Result<Self, Failure> {
        if !authorized {
            return Err(Failure("revoked"));
        }
        let end = Instant::now()
            .checked_add(lifetime)
            .ok_or(Failure("timeout"))?;
        isolate_environment(&mut command);
        Ok(Self {
            process: Process::spawn(command, true)?,
            end,
            buffer: Vec::new(),
        })
    }
    pub fn call(
        &mut self,
        request: &Value,
        authorized: &mut impl FnMut() -> bool,
    ) -> Result<Value, Failure> {
        let result = self.call_inner(request, authorized);
        if result.is_err() {
            self.close();
        }
        result
    }
    fn call_inner(
        &mut self,
        request: &Value,
        authorized: &mut impl FnMut() -> bool,
    ) -> Result<Value, Failure> {
        if self.process.closed || !self.buffer.is_empty() {
            return Err(Failure::protocol());
        }
        let context = request_context(request)?;
        let end = deadline(&context, Some(self.end))?;
        let payload = encode(request, true, "request_limit")?;
        let mut pending = payload.as_slice();
        let mut errors = Vec::new();
        loop {
            check(&context, end, authorized)?;
            self.process.write(&mut pending)?;
            let (out_closed, _) = self.process.read(&mut self.buffer, &mut errors)?;
            if let Some(newline) = self.buffer.iter().position(|byte| *byte == b'\n') {
                if !pending.is_empty() {
                    return Err(Failure::protocol());
                }
                let tail = self.buffer.split_off(newline + 1);
                let value = response(&context, &self.buffer[..newline])?;
                self.buffer = tail;
                if !self.buffer.is_empty() {
                    return Err(Failure::protocol());
                }
                check(&context, end, authorized)?;
                return Ok(value);
            }
            if out_closed
                || self
                    .process
                    .child
                    .try_wait()
                    .map_err(|_| Failure("process_exit"))?
                    .is_some()
            {
                return Err(Failure("process_exit"));
            }
            self.process.wait(!pending.is_empty(), end)?;
        }
    }
    pub fn close(&mut self) {
        self.process.close();
    }
}

fn collect(
    mut process: Process,
    context: &Context,
    input: Option<Vec<u8>>,
    authorized: &mut impl FnMut() -> bool,
) -> Result<Output, Failure> {
    let end = deadline(context, None)?;
    let mut pending = input.as_deref().unwrap_or_default();
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    loop {
        check(context, end, authorized)?;
        process.write(&mut pending)?;
        if input.is_some() && pending.is_empty() {
            process.input.take();
        }
        let (out_closed, err_closed) = process.read(&mut stdout, &mut stderr)?;
        let status = process
            .child
            .try_wait()
            .map_err(|_| Failure("process_exit"))?;
        if let Some(status) = status
            && out_closed
            && err_closed
        {
            check(context, end, authorized)?;
            return Ok(Output {
                status,
                stdout,
                stderr,
            });
        }
        process.wait(!pending.is_empty(), end)?;
    }
}

pub fn invoke(
    mut command: Command,
    request: &Value,
    authorized: &mut impl FnMut() -> bool,
) -> Result<Value, Failure> {
    let context = request_context(request)?;
    let end = deadline(&context, None)?;
    check(&context, end, authorized)?;
    let input = encode(request, true, "request_limit")?;
    isolate_environment(&mut command);
    let output = collect(
        Process::spawn(command, true)?,
        &context,
        Some(input),
        authorized,
    )?;
    if !output.status.success() {
        return Err(Failure("process_exit"));
    }
    response(&context, &output.stdout)
}

/// Bounded capture preserves the caller-supplied stdin, including an inherited lease.
pub fn capture(command: Command, context: &Context) -> Result<Output, Failure> {
    deadline(context, None)?;
    collect(Process::spawn(command, false)?, context, None, &mut || true)
}

fn read_frame(reader: &mut impl BufRead) -> Result<Option<Vec<u8>>, Failure> {
    let mut raw = Vec::new();
    loop {
        let available = reader.fill_buf().map_err(|_| Failure::protocol())?;
        if available.is_empty() {
            return Ok((!raw.is_empty()).then_some(raw));
        }
        let count = available
            .iter()
            .position(|byte| *byte == b'\n')
            .map_or(available.len(), |n| n + 1);
        if count > BYTE_LIMIT.saturating_sub(raw.len()) {
            return Err(Failure("request_limit"));
        }
        raw.extend_from_slice(&available[..count]);
        reader.consume(count);
        if raw.last() == Some(&b'\n') {
            return Ok(Some(raw));
        }
    }
}

/// Only JSON dispatch crosses the callback boundary; framing and failure redaction stay fixed.
pub fn serve(
    reader: &mut impl BufRead,
    writer: &mut impl Write,
    mut dispatch: impl FnMut(&Value) -> Result<Value, ()>,
) -> Result<(), Failure> {
    while let Some(raw) = read_frame(reader)? {
        let request = decode_request(&raw)?;
        let context = request_context(&request)?;
        let value = if ensure_active(&context).is_ok() {
            dispatch(&request)
        } else {
            Err(())
        };
        let failure = || json!({"context": context, "result": null, "error": {"code": "OPERATION_FAILED", "message": "Plugin operation failed"}});
        let response = match value {
            Ok(value) => {
                let response = json!({"context": context, "result": value, "error": null});
                if ensure_active(&context).is_ok() && validate("Reply", &response).is_ok() {
                    response
                } else {
                    failure()
                }
            }
            Err(()) => failure(),
        };
        let bytes = encode(&response, true, "output_limit")
            .or_else(|_| encode(&failure(), true, "output_limit"))?;
        writer
            .write_all(&bytes)
            .and_then(|_| writer.flush())
            .map_err(|_| Failure("process_exit"))?;
    }
    Ok(())
}

#[cfg(test)]
#[path = "transport_tests.rs"]
mod tests;
