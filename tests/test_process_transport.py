"""Real Python/native process-boundary lifecycle and budget checks."""

import gc
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
from asterion_bindings.communication import activate, call, context
from asterion_bindings.diagnostics import ProcessFailure
from asterion_bindings.transport import ProcessTransport

from asterion.platform.extensions.process import PackageSession, call_package, invoke


def wait_for_file(path, *, nonempty=False):
    until = time.monotonic() + 3
    while not path.exists() or (nonempty and not path.read_text()):
        assert time.monotonic() < until, "child did not reach the test barrier"
        time.sleep(0.005)


def test_parent_deadline_bounds_actual_process_wait():
    started = time.monotonic()
    with activate(context(0.1)), pytest.raises(ProcessFailure) as error:
        invoke(
            [sys.executable, "-c", "import time; time.sleep(10)"], "fixture.wait", {}, timeout=30
        )
    assert error.value.code == "timeout"
    assert time.monotonic() - started < 2


def test_quick_child_stderr_cannot_escape_combined_output_budget():
    script = "import json,sys; r=json.load(sys.stdin); sys.stderr.write('secret'*1400000); print(json.dumps({'context':r['context'],'result':True,'error':None}))"
    with pytest.raises(ProcessFailure) as error:
        invoke([sys.executable, "-c", script], "fixture.output", {})
    assert error.value.code == "output_limit"
    assert "secret" not in str(error.value)


def test_concurrent_call_is_rejected_and_cannot_consume_other_reply(tmp_path):
    barrier = tmp_path / "started"
    script = "import json,sys,time,pathlib; r=json.loads(sys.stdin.readline()); pathlib.Path(sys.argv[1]).touch(); time.sleep(10)"
    transport = ProcessTransport(
        [sys.executable, "-c", script, str(barrier)],
        cwd=tmp_path,
        timeout=30,
        authorized=lambda: True,
    )
    with ThreadPoolExecutor(max_workers=1) as pool:
        pending = pool.submit(transport.call, call("fixture.first", {}))
        wait_for_file(barrier)
        with pytest.raises(ProcessFailure) as rejected:
            transport.call(call("fixture.second", {}))
        assert rejected.value.code == "protocol"
        with pytest.raises(ProcessFailure) as interrupted:
            pending.result(timeout=2)
        assert interrupted.value.code == "revoked"
    transport.close()


def test_native_owner_drop_reaps_child_without_explicit_close(tmp_path):
    marker = tmp_path / "pid"
    script = "import os,sys,time,pathlib; pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(10)"
    transport = ProcessTransport(
        [sys.executable, "-c", script, str(marker)],
        cwd=tmp_path,
        timeout=30,
        authorized=lambda: True,
    )
    wait_for_file(marker, nonempty=True)
    pid = int(marker.read_text())
    del transport
    gc.collect()
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_request_construction_failure_closes_package_session(tmp_path):
    marker = tmp_path / "pid"
    (tmp_path / "plugin.py").write_text(
        "import os,time,pathlib; pathlib.Path('pid').write_text(str(os.getpid())); time.sleep(10)"
    )
    session = PackageSession(tmp_path, authorized=lambda: True)
    wait_for_file(marker, nonempty=True)
    pid = int(marker.read_text())
    with pytest.raises(ValueError):
        session.call("fixture.invalid", {"object": object()})
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_native_child_budget_bootstrap_and_sdk_dispatch(tmp_path):
    (tmp_path / "plugin.py").write_text(
        "from asterion_plugin_sdk import serve\n"
        "import resource\n"
        "serve(lambda method, payload: {'cpu':list(resource.getrlimit(resource.RLIMIT_CPU)), 'file':list(resource.getrlimit(resource.RLIMIT_FSIZE))})\n"
    )
    assert call_package(tmp_path, "fixture.limits", {}) == {
        "cpu": [30, 30],
        "file": [8_000_000, 8_000_000],
    }


def test_sdk_callback_failure_never_returns_exception_details(tmp_path):
    (tmp_path / "plugin.py").write_text(
        "from asterion_plugin_sdk import serve\n"
        "def fail(method, payload):\n"
        " raise RuntimeError('private-plugin-token')\n"
        "serve(fail)\n"
    )
    with pytest.raises(ProcessFailure) as error:
        call_package(tmp_path, "fixture.failure", {"token": "private-request-token"})
    assert error.value.code == "protocol"
    assert "private" not in str(error.value)


def test_authorization_callback_can_close_its_own_session_without_deadlock(tmp_path):
    script = """
from asterion_bindings.communication import call
from asterion_bindings.diagnostics import ProcessFailure
from asterion_bindings.transport import ProcessTransport
import sys
session = None
def authorized():
    if session is not None:
        session.close()
    return True
session = ProcessTransport(['/bin/cat'], cwd=sys.argv[1], timeout=1, authorized=authorized)
try:
    session.call(call('fixture.reentrant_close', {}))
except ProcessFailure as failure:
    assert failure.code == 'revoked', failure.code
else:
    raise AssertionError('closed call was accepted')
session.close()
"""
    result = subprocess.run(
        [sys.executable, "-c", script, str(tmp_path)],
        capture_output=True,
        text=True,
        check=False,
        timeout=3,
    )
    assert result.returncode == 0, result.stderr
