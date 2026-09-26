use super::*;
use nix::{errno::Errno, sys::signal::kill, unistd::Pid};
use serde_json::json;
use sha2::{Digest, Sha256};

const READ: &str = "import sys,json,struct,os,time\nf=sys.stdin.buffer\nh=json.loads(f.read(struct.unpack('>Q',f.read(8))[0]))\ndata=f.read(h['input_bytes'])\nassert f.read()==b''\n";
const REPLY: &str = "r=json.dumps({'context':h['context'],'content_bytes':len(body),'metadata':{'input':len(data)},'error':None}).encode()\nsys.stdout.buffer.write(struct.pack('>Q',len(r))+r+body)\nsys.stdout.buffer.flush()\n";
fn limits() -> Limits {
    Limits {
        input_bytes: 64 * 1024 * 1024,
        artifact_bytes: 64 * 1024 * 1024,
        metadata_bytes: 1024,
        stderr_bytes: 1024 * 1024,
    }
}
fn python(script: &str) -> Command {
    let mut command = Command::new("python3");
    command.args(["-c", script]);
    command
}
fn spawn(script: &str, input: &[u8], timeout: f64, budget: Limits) -> TaskProcess {
    TaskProcess::spawn(
        python(script),
        communication::context(None, timeout).unwrap(),
        input,
        budget,
    )
    .unwrap()
}
fn complete(process: &mut TaskProcess) -> Result<Artifact> {
    while !process.poll(
        Duration::from_millis(100),
        &AtomicBool::new(false),
        &mut || false,
    )? {}
    process.take_result()
}

#[test]
fn binary_artifact_exceeds_rpc_budget_without_base64_or_corruption() {
    let input = vec![b'x'; 32 * 1024 * 1024 + 7];
    let script = format!(
        "import sys\nsys.stderr.buffer.write(b'private'*10000)\nsys.stderr.flush()\n{READ}body=bytes(range(256))*131073\n{REPLY}"
    );
    let mut task = spawn(&script, &input, 10.0, limits());
    let mut artifact = complete(&mut task).unwrap();
    assert_eq!(artifact.metadata()["input"], input.len());
    let mut hash = Sha256::new();
    let mut size = 0;
    loop {
        let chunk = artifact.read_chunk().unwrap();
        if chunk.is_empty() {
            break;
        }
        size += chunk.len();
        hash.update(chunk);
    }
    let expected: Vec<u8> = (0..=255).cycle().take(256 * 131073).collect();
    assert_eq!(size, expected.len());
    assert_eq!(hash.finalize(), Sha256::digest(&expected));
    artifact.close();
    assert_eq!(artifact.read_chunk().unwrap_err().code(), "closed");
    assert!(task.take_result().is_err());
}

#[test]
fn invalid_frames_exits_and_budget_failures_reap_child() {
    let scripts =
        [
            format!("{READ}sys.stdout.buffer.write(b'partial')"),
            format!("{READ}body=b'ok'\n{REPLY}sys.stdout.buffer.write(b'trailing')"),
            format!("{READ}h['context']['request_id']='c'*32\nbody=b'ok'\n{REPLY}"),
            format!("{READ}body=b'ok'\n{REPLY}sys.exit(3)"),
            format!(
                "{READ}body=b'ok'\n{}",
                REPLY
                    .replace(", 'error':None", "")
                    .replace(",'error':None", "")
            ),
            format!("{READ}body=b'ok'\n{}", REPLY.replace(
            "sys.stdout.buffer.write",
            "r=r.replace(b'\"input\": 5', b'\"input\":5,\"input\":6')\nsys.stdout.buffer.write",
        )),
            format!("{READ}sys.stderr.buffer.write(b'private'*200000)"),
            format!("{READ}body=b'x'*(64*1024*1024+1)\n{REPLY}"),
            format!("{READ}sys.stdout.buffer.write(struct.pack('>Q',1000000))"),
        ];
    for script in scripts {
        let mut task = spawn(&script, b"input", 5.0, limits());
        let pid = task.child.id();
        assert!(complete(&mut task).is_err());
        assert_eq!(kill(Pid::from_raw(pid as i32), None), Err(Errno::ESRCH));
    }
}

#[test]
fn pending_does_not_restart_and_close_timeout_drop_reap_process() {
    let script = format!("{READ}time.sleep(10)");
    let mut task = spawn(&script, b"input", 5.0, limits());
    let pid = task.child.id();
    for _ in 0..3 {
        assert!(
            !task
                .poll(
                    Duration::from_millis(10),
                    &AtomicBool::new(false),
                    &mut || false
                )
                .unwrap()
        );
    }
    assert_eq!(pid, task.child.id());
    let start = Instant::now();
    task.close();
    task.close();
    assert!(start.elapsed() < Duration::from_secs(1));
    assert_eq!(kill(Pid::from_raw(pid as i32), None), Err(Errno::ESRCH));
    assert_eq!(
        task.poll(Duration::ZERO, &AtomicBool::new(false), &mut || false)
            .unwrap_err()
            .code(),
        "closed"
    );
    let mut task = spawn(&script, b"input", 0.08, limits());
    let pid = task.child.id();
    assert_eq!(complete(&mut task).err().unwrap().code(), "timeout");
    assert_eq!(kill(Pid::from_raw(pid as i32), None), Err(Errno::ESRCH));
    let task = spawn(&script, b"input", 5.0, limits());
    let pid = task.child.id();
    drop(task);
    assert_eq!(kill(Pid::from_raw(pid as i32), None), Err(Errno::ESRCH));
}

#[test]
fn closed_owner_terminates_descendants_that_retain_pipes() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("descendant");
    let script = format!(
        "{READ}import subprocess\np=subprocess.Popen(['python3','-c','import time; time.sleep(30)'])\nopen(data.decode(),'w').write(str(p.pid))\ntime.sleep(30)\n"
    );
    let mut task = spawn(&script, path.to_str().unwrap().as_bytes(), 5.0, limits());
    for _ in 0..100 {
        task.poll(
            Duration::from_millis(10),
            &AtomicBool::new(false),
            &mut || false,
        )
        .unwrap();
        if path.exists() {
            break;
        }
    }
    let pid: u32 = std::fs::read_to_string(path).unwrap().parse().unwrap();
    task.close();
    for _ in 0..100 {
        let state = std::fs::read_to_string(format!("/proc/{pid}/stat"));
        if state
            .as_ref()
            .map_or(true, |value| value.split_whitespace().nth(2) == Some("Z"))
        {
            return;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    panic!("descendant survived owned process group shutdown");
}

#[test]
fn malformed_input_never_spawns_and_child_response_contract_is_fixed() {
    let mut budget = limits();
    budget.input_bytes = 2;
    assert!(
        TaskProcess::spawn(
            python("raise Exception('must not run')"),
            communication::context(None, 5.0).unwrap(),
            b"oversized",
            budget
        )
        .is_err()
    );
    let request = RequestHeader {
        context: communication::context(None, 5.0).unwrap(),
        input_bytes: 3,
        limits: limits(),
    };
    let header = serde_json::to_vec(&request).unwrap();
    let mut bytes = (header.len() as u64).to_be_bytes().to_vec();
    bytes.extend(header);
    bytes.extend(b"job");
    let mut output = vec![];
    serve(&mut io::Cursor::new(bytes.clone()), &mut output, |input| {
        assert_eq!(input, b"job");
        ComputationResult {
            content: vec![0, 255],
            metadata: json!({"rows":2}).as_object().unwrap().clone(),
            error: None,
        }
    })
    .unwrap();
    let mut receiver = Receiver::new().unwrap();
    for piece in output.chunks(3) {
        receiver
            .receive(piece, &request.context, &limits())
            .unwrap();
    }
    let mut artifact = receiver.finish().unwrap();
    assert_eq!(artifact.read_chunk().unwrap(), [0, 255]);
    bytes.push(1);
    assert!(
        serve(&mut io::Cursor::new(bytes), &mut Vec::new(), |_| panic!(
            "invalid input must not dispatch"
        ))
        .is_err()
    );
}

#[test]
fn expired_completed_result_releases_spool_and_seals_owner() {
    let script = format!("{READ}body=b'result'\n{REPLY}");
    for monotonic in [false, true] {
        let mut task = spawn(&script, b"input", 5.0, limits());
        while !task
            .poll(
                Duration::from_millis(10),
                &AtomicBool::new(false),
                &mut || false,
            )
            .unwrap()
        {}
        if monotonic {
            task.deadline = Instant::now() - Duration::from_millis(1);
        } else {
            task.context.deadline_ms = communication::now_ms().unwrap() - 1;
        }
        assert_eq!(task.take_result().err().unwrap().code(), "timeout");
        assert!(task.receiver.file.is_none());
        assert!(task.closed);
    }
}
