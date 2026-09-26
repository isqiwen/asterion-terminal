use super::*;
use nix::errno::Errno;
use nix::unistd::Pid;
use std::io::Cursor;
use std::time::{SystemTime, UNIX_EPOCH};

fn request(milliseconds: u64) -> Value {
    json!({"context": {
        "version": 1,
        "request_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "correlation_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "causation_id": null,
        "deadline_ms": SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64 + milliseconds
    }, "kind":"query", "contract":"fixture.query", "payload":{}})
}
fn python(script: &str) -> Command {
    let mut command = Command::new("python3");
    command.args(["-c", script]);
    command
}
const ECHO: &str = "import json,sys; r=json.load(sys.stdin); print(json.dumps({'context':r['context'],'result':42,'error':None}))";

#[test]
fn one_call_uses_eof_and_strips_runtime_environment() {
    let mut command = python(
        "import json,sys,os; r=json.load(sys.stdin); print(json.dumps({'context':r['context'],'result':dict(os.environ),'error':None}))",
    );
    command
        .env("ASTERION_SECRET", "private-sentinel")
        .env("PYTHONPATH", "/private-path");
    let value = invoke(command, &request(5000), &mut || true).unwrap();
    assert!(value.get("ASTERION_SECRET").is_none());
    assert!(value.get("PYTHONPATH").is_none());
    assert_eq!(
        invoke(python(ECHO), &request(5000), &mut || true).unwrap(),
        json!(42)
    );
}

#[test]
fn parent_deadline_and_revoke_terminate_in_flight_processes() {
    let start = Instant::now();
    let error = invoke(
        python("import time; time.sleep(10)"),
        &request(80),
        &mut || true,
    )
    .unwrap_err();
    assert_eq!(error.code(), "timeout");
    assert!(start.elapsed() < Duration::from_secs(2));
    let start = Instant::now();
    let error = invoke(
        python("import time; time.sleep(10)"),
        &request(5000),
        &mut || start.elapsed() < Duration::from_millis(70),
    )
    .unwrap_err();
    assert_eq!(error.code(), "revoked");
    assert!(start.elapsed() < Duration::from_secs(2));
}

#[test]
fn completed_child_stderr_and_combined_output_share_one_budget() {
    for script in [
        "import json,sys; r=json.load(sys.stdin); sys.stderr.write('x'*8000001); print(json.dumps({'context':r['context'],'result':True,'error':None}))",
        "import json,sys; r=json.load(sys.stdin); sys.stderr.write('x'*4000000); print(json.dumps({'context':r['context'],'result':'x'*4000000,'error':None}))",
    ] {
        let error = invoke(python(script), &request(5000), &mut || true).unwrap_err();
        assert_eq!(error.code(), "output_limit");
    }
}

#[test]
fn oversized_request_never_spawns_and_mismatched_reply_closes_session() {
    let mut oversized = request(5000);
    oversized["payload"] = json!({"data": "x".repeat(BYTE_LIMIT)});
    assert_eq!(
        invoke(python(ECHO), &oversized, &mut || true)
            .unwrap_err()
            .code(),
        "request_limit"
    );
    let mut session = ProcessSession::spawn(python("import json,sys; r=json.loads(sys.stdin.readline()); r['context']['request_id']='cccccccccccccccccccccccccccccccc'; print(json.dumps({'context':r['context'],'result':None,'error':None}),flush=True); import time; time.sleep(10)"), Duration::from_secs(5), true).unwrap();
    assert_eq!(
        session
            .call(&request(5000), &mut || true)
            .unwrap_err()
            .code(),
        "protocol"
    );
    assert!(session.process.closed);
}

#[test]
fn session_preserves_requests_and_honors_shorter_call_deadline() {
    let script = "import json,sys\nfor line in sys.stdin:\n r=json.loads(line); print(json.dumps({'context':r['context'],'result':r['payload'],'error':None}),flush=True)";
    let mut session = ProcessSession::spawn(python(script), Duration::from_secs(5), true).unwrap();
    for n in 0..3 {
        let mut message = request(5000);
        message["payload"] = json!({"n": n});
        assert_eq!(
            session.call(&message, &mut || true).unwrap(),
            json!({"n": n})
        );
    }
    let mut waiting = ProcessSession::spawn(
        python("import time; time.sleep(10)"),
        Duration::from_secs(5),
        true,
    )
    .unwrap();
    let start = Instant::now();
    assert_eq!(
        waiting.call(&request(70), &mut || true).unwrap_err().code(),
        "timeout"
    );
    assert!(start.elapsed() < Duration::from_secs(2));
    assert!(waiting.process.closed);
}

#[test]
fn invalid_request_and_drop_reap_the_owned_child() {
    let mut session = ProcessSession::spawn(
        python("import time; time.sleep(10)"),
        Duration::from_secs(5),
        true,
    )
    .unwrap();
    let pid = session.process.child.id();
    assert_eq!(
        session.call(&json!({}), &mut || true).unwrap_err().code(),
        "protocol"
    );
    assert_eq!(
        nix::sys::signal::kill(Pid::from_raw(pid as i32), None),
        Err(Errno::ESRCH)
    );
    let session = ProcessSession::spawn(
        python("import time; time.sleep(10)"),
        Duration::from_secs(5),
        true,
    )
    .unwrap();
    let pid = session.process.child.id();
    drop(session);
    assert_eq!(
        nix::sys::signal::kill(Pid::from_raw(pid as i32), None),
        Err(Errno::ESRCH)
    );
}

#[test]
fn capture_preserves_file_stdin_and_limits_waiting() {
    use std::io::{Seek, SeekFrom};
    let mut lease = tempfile::tempfile().unwrap();
    lease.write_all(b"retained input").unwrap();
    lease.seek(SeekFrom::Start(0)).unwrap();
    let mut command = python("import sys; print(sys.stdin.read())");
    command.stdin(lease);
    let context = request_context(&request(5000)).unwrap();
    let output = capture(command, &context).unwrap();
    assert!(output.status.success());
    assert_eq!(output.stdout, b"retained input\n");
}

#[test]
fn server_frames_validate_calls_and_redact_callback_failures() {
    let message = request(5000);
    let mut input = Cursor::new(format!("{message}\n{message}\n"));
    let mut output = Vec::new();
    let mut count = 0;
    serve(&mut input, &mut output, |_| {
        count += 1;
        if count == 1 {
            Ok(json!({"ok":true}))
        } else {
            Err(())
        }
    })
    .unwrap();
    let replies = String::from_utf8(output).unwrap();
    let lines: Vec<_> = replies.lines().collect();
    assert_eq!(lines.len(), 2);
    assert_eq!(
        parse_json(lines[0].as_bytes()).unwrap()["result"],
        json!({"ok":true})
    );
    assert_eq!(
        parse_json(lines[1].as_bytes()).unwrap()["error"]["code"],
        "OPERATION_FAILED"
    );
    let invalid = "{\"context\":{},\"context\":{}}";
    assert_eq!(
        serve(&mut Cursor::new(invalid), &mut Vec::new(), |_| panic!(
            "invalid input must not dispatch"
        ))
        .unwrap_err()
        .code(),
        "protocol"
    );
}

#[test]
fn server_rejects_oversized_frames_and_bounds_callback_results() {
    assert_eq!(
        serve(
            &mut Cursor::new(vec![b'x'; BYTE_LIMIT + 1]),
            &mut Vec::new(),
            |_| panic!("oversized input must not dispatch")
        )
        .unwrap_err()
        .code(),
        "request_limit"
    );
    let mut output = Vec::new();
    serve(
        &mut Cursor::new(request(5000).to_string()),
        &mut output,
        |_| Ok(json!("x".repeat(BYTE_LIMIT))),
    )
    .unwrap();
    assert!(output.len() < 1000);
    assert_eq!(
        parse_json(&output).unwrap()["error"]["code"],
        "OPERATION_FAILED"
    );
}

#[test]
fn five_thousand_frames_fit_the_session_without_polling_delay_per_frame() {
    let script = "import json,sys\nfor line in sys.stdin:\n r=json.loads(line); print(json.dumps({'context':r['context'],'result':True,'error':None}),flush=True)";
    let mut session = ProcessSession::spawn(python(script), Duration::from_secs(10), true).unwrap();
    let message = request(30_000);
    let start = Instant::now();
    for _ in 0..5000 {
        assert_eq!(session.call(&message, &mut || true).unwrap(), json!(true));
    }
    eprintln!("5000 native JSON-line roundtrips: {:?}", start.elapsed());
}

#[test]
fn late_valid_reply_is_a_timeout_instead_of_protocol_failure() {
    let mut context = request_context(&request(5000)).unwrap();
    context.deadline_ms = 1;
    let raw = json!({"context":context,"result":{"ok":true},"error":null}).to_string();
    assert_eq!(
        response(&context, raw.as_bytes()).unwrap_err().code(),
        "timeout"
    );
    let context = request_context(&request(5000)).unwrap();
    assert_eq!(
        response(&context, raw.as_bytes()).unwrap_err().code(),
        "protocol"
    );
}

#[test]
fn raw_frames_are_bounded_before_json_parsing() {
    let oversized_invalid = vec![b'x'; BYTE_LIMIT + 1];
    assert_eq!(
        decode_request(&oversized_invalid).unwrap_err().code(),
        "request_limit"
    );
    assert_eq!(
        decode_result(&oversized_invalid).unwrap_err().code(),
        "output_limit"
    );
    assert_eq!(decode_request(b"not json").unwrap_err().code(), "protocol");
    assert_eq!(decode_result(b"not json").unwrap_err().code(), "protocol");
}
