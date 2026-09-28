"""Agent program publication, explicit recovery and preservation acceptance."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

fixture, agent, revision = map(lambda value: Path(value).resolve(), sys.argv[1:])
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
assert digest(agent) != digest(revision)
with tempfile.TemporaryDirectory(prefix="ast-upgrade-", ignore_cleanup_errors=True) as folder:
    root = Path(folder).resolve()
    (root / "bin").mkdir()
    installed = root / "bin" / agent.name
    def inspect(source=agent):
        result = subprocess.run(
            [str(fixture), "--operation", "inspect", "--executable", str(installed),
             "--root", str(root), "--endpoint", "unused", "--name", "me.asterion.acceptance.inspect",
             "--source", str(source)], capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    initial = sorted(str(p.relative_to(root)) for p in root.rglob("*"))
    assert inspect()["state"] == "not_installed"
    assert sorted(str(p.relative_to(root)) for p in root.rglob("*")) == initial
    shutil.copy2(agent, installed)
    assert inspect()["state"] == "current"
    assert inspect(revision)["state"] == "update_available"
    before, after = digest(agent), digest(revision)
    data = root / "services" / "paper" / "ledger"
    data.mkdir(parents=True)
    (data / "retained").write_bytes(b"test-owned immutable ledger")
    endpoint = "asterion.acceptance.update." + str(os.getpid()) if os.name == "nt" else str(root / "agent.sock")
    def replace(expected=before, source=revision):
        return subprocess.run(
            [str(fixture), "--operation", "replace", "--executable", str(installed),
             "--root", str(root), "--endpoint", endpoint, "--name", "me.asterion.acceptance.update",
             "--source", str(source), "--expected", expected],
            capture_output=True, text=True, timeout=20,
        )
    # Agent owns the directory: publication must fail without any write.
    # Keep the service fixture outside its managed services directory until it stops.
    preserved = root / "preserved"
    (root / "services").rename(preserved)
    process = subprocess.Popen([str(installed), "--directory", str(root), "--endpoint", endpoint],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        end = time.monotonic() + 10
        while not (root / "agent.pid").exists():
            assert process.poll() is None
            assert time.monotonic() < end
            time.sleep(.05)
        assert replace().returncode != 0
        assert digest(installed) == before and not (root / "agent-upgrade.json").exists()
    finally:
        process.terminate()
        process.wait(timeout=15)
    data = preserved / "paper" / "ledger"
    assert replace("0" * 64).returncode != 0
    assert digest(installed) == before

    record = {"version": 1, "installed": str(installed), "before": before, "after": after}
    journal = root / "agent-upgrade.json"
    pending = root / "agent-upgrade.pending"
    staged = root / "bin" / "agent-upgrade.staged"
    for name in ["agent-service-upgrade.json", "agent-service-upgrade.pending"]:
        retained = root / name
        retained.write_text("{")
        assert inspect()["state"] == "recovery_required"
        assert retained.read_text() == "{"
        retained.unlink()  # Explicit test-owned fixture cleanup.
    pending.write_text("{")
    assert inspect()["state"] == "recovery_required"
    assert replace().returncode != 0 and pending.read_text() == "{"
    pending.unlink()  # Explicit test-owned fault cleanup.
    staged.write_bytes(b"incomplete candidate")
    assert replace().returncode != 0 and staged.read_bytes() == b"incomplete candidate"
    journal.write_text(json.dumps(record))
    assert replace().returncode != 0 and digest(installed) == before
    staged.unlink()  # Explicit fault cleanup, never product automatic deletion.

    # Interrupted after recording but before staging: explicitly resume.
    resumed = replace()
    assert resumed.returncode == 0, resumed.stderr
    assert digest(installed) == after and not journal.exists()
    assert (data / "retained").read_bytes() == b"test-owned immutable ledger"
    assert subprocess.run([str(installed), "--version"], capture_output=True).returncode == 0

    # Interrupted after publication: same transaction completes without rewriting.
    journal.write_text(json.dumps(record))
    stamp = installed.stat().st_mtime_ns
    assert replace().returncode == 0
    assert installed.stat().st_mtime_ns == stamp and not journal.exists()

    # Conflicting recovery intent and modified published program are preserved.
    journal.write_text(json.dumps(record))
    assert replace(source=agent).returncode != 0 and journal.exists()
    journal.unlink()
    shutil.copy2(agent, installed)
    successful = replace()
    assert successful.returncode == 0, successful.stderr
    assert digest(installed) == after
print("Agent replacement: live ownership/stale digest/partial state rejected; explicit resume and data preservation passed")
