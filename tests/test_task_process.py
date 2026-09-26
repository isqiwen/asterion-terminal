"""Real process and HTTP-lifetime regressions for the fixed task computation owner."""

import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
import pytest
from asterion_bindings.communication import context
from asterion_bindings.diagnostics import ProcessFailure
from asterion_bindings.task_process import TaskProcess

from asterion.platform.config import Settings
from asterion.runtime import worker

LIMITS = {
    "input_bytes": 64 * 1024 * 1024,
    "artifact_bytes": 64 * 1024 * 1024,
    "metadata_bytes": 1024 * 1024,
    "stderr_bytes": 1024 * 1024,
}
SLEEP_CHILD = """
import json, os, subprocess, sys, time
from pathlib import Path
from asterion_bindings.task_process import serve
def dispatch(request):
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
    pending = Path(request['pid_file'] + '.pending')
    pending.write_text(json.dumps([os.getpid(), child.pid]))
    pending.replace(request['pid_file'])
    time.sleep(60)
    return b'late result', {}
serve(dispatch)
"""


def running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    except FileNotFoundError:
        return False


def wait_stopped(pids):
    for _ in range(100):
        if not any(running(pid) for pid in pids):
            return
        time.sleep(0.01)
    assert not any(running(pid) for pid in pids)


def test_binary_task_attachment_exceeds_rpc_limit_and_preserves_metadata():
    child = """
from asterion_bindings.task_process import serve
def dispatch(request):
    return bytes(range(256)) * 131073, {'input': len(request['input'])}
serve(dispatch)
"""
    with TaskProcess(
        [sys.executable, "-c", child], context(20), {"input": "x" * 9_000_001}, limits=LIMITS
    ) as process:
        while not process.poll(1):
            pass
        with process.take_result() as artifact:
            assert artifact.metadata == {"input": 9_000_001}
            digest = hashlib.sha256()
            count = 0
            for chunk in artifact:
                assert len(chunk) <= 65_536
                digest.update(chunk)
                count += len(chunk)
            assert count == 256 * 131073
            assert digest.digest() == hashlib.sha256(bytes(range(256)) * 131073).digest()
        with pytest.raises(ProcessFailure, match="closed"):
            process.take_result()


@pytest.mark.parametrize("cause", ["budget", "failure"])
def test_child_failures_never_produce_a_publishable_artifact(cause):
    child = """
from asterion_bindings.task_process import serve
def dispatch(request):
    if request['cause'] == 'failure':
        raise ValueError('Task input is invalid')
    return b'x' * 1025, {}
serve(dispatch)
"""
    with (
        TaskProcess(
            [sys.executable, "-c", child],
            context(10),
            {"cause": cause},
            limits=LIMITS | {"artifact_bytes": 1024},
        ) as process,
        pytest.raises(ProcessFailure),
    ):
        while not process.poll(1):
            pass
        process.take_result()


@pytest.mark.parametrize("renewal", ["conflict", "timeout"])
def test_renewal_failure_closes_real_computation_before_failure_reporting(
    monkeypatch, tmp_path, renewal
):
    pid_file = tmp_path / "pids.json"
    job = {
        "id": "task",
        "kind": "research.backtest",
        "payload": {},
        "token": "private-lease",
        "communication": context(30),
    }
    requests = []
    pids = []

    def computation(settings, claimed):
        return TaskProcess(
            [sys.executable, "-c", SLEEP_CHILD],
            claimed["communication"],
            {"pid_file": str(pid_file)},
            limits=LIMITS,
        )

    def transport(request):
        requests.append(request.url.path)
        if request.url.path.endswith("/claim"):
            return httpx.Response(200, json=job)
        if request.url.path.endswith("/heartbeat"):
            pids.extend(json.loads(pid_file.read_text()))
            if renewal == "timeout":
                raise httpx.ReadTimeout("Renewal timed out", request=request)
            return httpx.Response(409)
        assert request.url.path.endswith("/fail")
        wait_stopped(pids)
        return httpx.Response(409)

    client = httpx.Client
    monkeypatch.setattr(
        httpx, "Client", lambda **kwargs: client(transport=httpx.MockTransport(transport), **kwargs)
    )
    monkeypatch.setattr(worker, "computation", computation)
    start = time.monotonic()
    try:
        assert worker.run_once(
            Settings(api_url="http://worker.test", data_root=tmp_path, lease_seconds=1), "fixture"
        )
    finally:
        if pid_file.exists():
            owned = json.loads(pid_file.read_text())
            if running(owned[0]):
                os.killpg(owned[0], signal.SIGKILL)
    assert time.monotonic() - start < 5
    assert requests == [
        "/api/v1/jobs/claim",
        "/api/v1/jobs/task/heartbeat",
        "/api/v1/jobs/task/fail",
    ]
    assert len(pids) == 2
    wait_stopped(pids)


@pytest.mark.parametrize("shutdown", [signal.SIGTERM, signal.SIGINT])
def test_supervised_worker_stop_reclaims_computation_process_group(tmp_path, shutdown):
    pid_file = tmp_path / "pids.json"
    script = f"""
import httpx, sys
from asterion_bindings.communication import context
from asterion_bindings.task_process import TaskProcess
from asterion.platform.config import Settings
from asterion.runtime import worker
job = {{'id':'task', 'kind':'research.backtest', 'token':'lease', 'payload':{{}}, 'communication':context(30)}}
original = httpx.Client
def transport(request):
    return httpx.Response(200,json=job if request.url.path.endswith('/claim') else {{}})
httpx.Client=lambda **kwargs: original(transport=httpx.MockTransport(transport), **kwargs)
worker.computation=lambda settings,claimed: TaskProcess([sys.executable,'-c',{SLEEP_CHILD!r}],claimed['communication'],{{'pid_file':sys.argv[1]}},limits={LIMITS!r})
worker.run(Settings(token='test-runtime-key-for-worker-stop', api_url='http://worker.test', data_root=sys.argv[2], lease_seconds=3),once=True)
"""
    parent = subprocess.Popen(
        [sys.executable, "-c", script, str(pid_file), str(tmp_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    pids = []
    try:
        for _ in range(500):
            if pid_file.exists():
                pids = json.loads(pid_file.read_text())
                break
            assert parent.poll() is None, parent.communicate()
            time.sleep(0.01)
        assert len(pids) == 2
        parent.send_signal(shutdown)
        output, errors = parent.communicate(timeout=5)
        assert parent.returncode == 0, (output, errors)
        wait_stopped(pids)
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait()
        if pids and running(pids[0]):
            os.killpg(pids[0], signal.SIGKILL)
