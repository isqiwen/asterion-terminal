"""Real filesystem checks through the native mechanism and its Python grants."""

import gc
import hashlib
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Event, Thread

import pytest
from asterion_bindings.files import (
    atomic_write,
    file_digest,
    file_lock,
    read_files,
    trusted_tree_digest,
)


def test_native_read_grant_is_anchored_and_bounded(tmp_path):
    root = tmp_path / "root"
    root.mkdir()
    (root / "document").write_bytes(b"original")
    files = read_files(root, max_read_bytes=8)
    root.rename(tmp_path / "renamed")
    root.mkdir()
    (root / "document").write_bytes(b"replacement")
    assert files.read("document") == b"original"
    assert files.digest("document") == hashlib.sha256(b"original").hexdigest()
    with pytest.raises(ValueError, match="read limit"):
        read_files(root, max_read_bytes=3).read("document")
    with pytest.raises(FileNotFoundError):
        files.read("missing")


def test_scan_budget_counts_all_entries_and_rejects_links(tmp_path):
    directory = tmp_path / "parts"
    directory.mkdir()
    for i in range(4):
        (directory / f"{i}.bin").write_bytes(b"a")
    with pytest.raises(ValueError, match="entry limit"):
        read_files(tmp_path, max_scan_entries=2).scan("parts", ".irrelevant")
    files = read_files(tmp_path)
    with ThreadPoolExecutor(4) as pool:
        assert all(
            len(found) == 4 for found in pool.map(lambda _: files.scan("parts", ".bin"), range(30))
        )
    (directory / "link").symlink_to(tmp_path / "absent")
    with pytest.raises(ValueError, match="links"):
        files.scan("parts", ".bin")


def test_atomic_publication_preserves_existing_files_on_conflict(tmp_path):
    target = tmp_path / "state"
    atomic_write(target, b"one", replace=False)
    assert target.stat().st_mode & 0o777 == 0o600
    with pytest.raises(FileExistsError):
        atomic_write(target, b"two", replace=False)
    assert target.read_bytes() == b"one"
    atomic_write(target, b"two")
    assert file_digest(target) == hashlib.sha256(b"two").hexdigest()
    assert sorted(p.name for p in tmp_path.iterdir()) == ["state"]
    (tmp_path / "link").symlink_to(target)
    with pytest.raises(ValueError):
        atomic_write(tmp_path / "link", b"three")
    assert target.read_bytes() == b"two"


def test_native_lock_interoperates_with_real_python_flock_and_drop(tmp_path):
    lock = tmp_path / "lock"
    script = """
import fcntl, sys
with open(sys.argv[1], "a") as stream:
    try:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(23)
"""
    with file_lock(lock):
        assert (
            subprocess.run([sys.executable, "-c", script, str(lock)], check=False).returncode == 23
        )
        with pytest.raises(BlockingIOError), file_lock(lock, blocking=False):
            pass
    assert subprocess.run([sys.executable, "-c", script, str(lock)], check=False).returncode == 0
    from asterion_bindings import _native

    handle = _native.FileLockHandle(lock, False)
    del handle
    gc.collect()
    with file_lock(lock, blocking=False):
        pass


def test_package_records_use_atomic_publication_and_remain_readable(tmp_path):
    from asterion.platform.extensions.packages import Packages

    packages = Packages(tmp_path, {})
    with packages.locked():
        packages._save({})
    assert json.loads((tmp_path / "installed.json").read_bytes()) == {
        "api_version": 1,
        "packages": {},
    }
    assert packages.list() == []
    assert (tmp_path / "installed.json").stat().st_mode & 0o777 == 0o600


def _identity_contract(roots):
    """Independent reference for the established path/content identity frames."""
    digest = hashlib.sha256()
    for index, root in enumerate(roots):
        paths = [root] if root.is_file() else sorted(root.rglob("*"))
        for path in paths:
            relative = path.relative_to(root) if root.is_dir() else Path(root.name)
            if "__pycache__" in relative.parts or path.suffix == ".pyc" or not path.is_file():
                continue
            name = f"{index}/{relative.as_posix()}".encode()
            digest.update(len(name).to_bytes(8, "big"))
            digest.update(name)
            with path.open("rb") as stream:
                digest.update(hashlib.file_digest(stream, "sha256").digest())
    return digest.hexdigest()


def test_installed_interpreter_links_preserve_the_existing_identity_contract(tmp_path):
    from asterion.runtime.build_identity import tree_digest

    environment = tmp_path / "environment"
    interpreter = tmp_path / "interpreter"
    (environment / "bin").mkdir(parents=True)
    (environment / "lib/__pycache__").mkdir(parents=True)
    (interpreter / "python/bin").mkdir(parents=True)
    (interpreter / "python/bin/python3.13").write_bytes(b"interpreter")
    (interpreter / "python/bin/python3").symlink_to("python3.13")
    (environment / "bin/python").symlink_to("../../interpreter/python/bin/python3")
    (environment / "bin/python3").symlink_to("python")
    (environment / "lib64").symlink_to("lib", target_is_directory=True)
    (environment / "lib/module.py").write_bytes(b"installed dependency")
    (environment / "lib/compiled.pyc").write_bytes(b"excluded")
    (environment / "lib/__pycache__/ignored").write_bytes(b"excluded")
    roots = (environment, interpreter)
    assert tree_digest(roots) == _identity_contract(roots)
    before = tree_digest(roots)
    (environment / "lib/__pycache__/ignored").write_bytes(b"changed cache")
    assert tree_digest(roots) == before
    (interpreter / "python/bin/python3.13").write_bytes(b"new content")
    assert tree_digest(roots) != before


def test_actual_development_venv_executable_links_are_hashed_by_content():
    from asterion.runtime.build_identity import tree_digest

    # Exercise the real venv's executable aliases, including a target outside the
    # venv on development installs; hashing all dependency payloads is unnecessary.
    roots = (Path(sys.prefix) / "bin", Path(sys.executable))
    assert tree_digest(roots) == _identity_contract(roots)


def test_trusted_digest_rejects_special_files_and_link_failures_without_path_disclosure(tmp_path):
    import os

    path = tmp_path / "private-name"
    path.write_bytes(b"content")
    with pytest.raises(ValueError, match="byte budget"):
        trusted_tree_digest((path,), max_bytes=3)
    with pytest.raises(ValueError, match="entry budget"):
        trusted_tree_digest((tmp_path,), max_entries=1)
    os.mkfifo(tmp_path / "pipe")
    with pytest.raises(ValueError, match="regular files") as caught:
        trusted_tree_digest((tmp_path,))
    assert str(tmp_path) not in str(caught.value)
    (tmp_path / "pipe").unlink()
    (tmp_path / "broken").symlink_to(tmp_path / "missing")
    with pytest.raises(FileNotFoundError) as caught:
        trusted_tree_digest((tmp_path,))
    assert str(tmp_path) not in str(caught.value)


def test_deadline_lock_releases_the_gil_and_preserves_existing_contents(tmp_path):
    lock = tmp_path / "lock"
    lock.write_bytes(b"preserved")
    acquired = Event()
    release = Event()

    def owner():
        with file_lock(lock):
            acquired.set()
            assert release.wait(3)

    thread = Thread(target=owner)
    thread.start()
    assert acquired.wait(3)
    try:
        before = time.monotonic()
        with pytest.raises(TimeoutError), file_lock(lock, timeout=0.04):
            pass
        assert time.monotonic() - before >= 0.04
        release.set()
        # The owner needs the GIL to leave its context and release the native lock.
        with file_lock(lock, timeout=2):
            assert lock.read_bytes() == b"preserved"
    finally:
        release.set()
        thread.join(timeout=3)
    assert not thread.is_alive()
    for timeout in (-1, float("inf"), float("nan"), 86401):
        with pytest.raises(ValueError), file_lock(lock, timeout=timeout):
            pass
    with pytest.raises(ValueError), file_lock(lock, blocking=False, timeout=1):
        pass


def test_desktop_wait_stopped_uses_native_deadline_and_preserves_timeout_message(
    tmp_path, monkeypatch
):
    from contextlib import contextmanager

    from asterion.runtime import desktop

    calls = []

    @contextmanager
    def timed_out(path, *, timeout):
        calls.append((path, timeout))
        raise TimeoutError("native deadline")
        yield

    monkeypatch.setattr(desktop, "file_lock", timed_out)
    with pytest.raises(RuntimeError, match="后台仍在停止"):
        desktop.wait_stopped(tmp_path)
    assert calls == [(tmp_path / "supervisor.lock", 40)]


@pytest.mark.skipif(sys.platform != "linux", reason="Linux subprocess fsync fault injection")
def test_published_native_failure_reaches_python_without_claiming_rollback(tmp_path):
    import os
    import shutil

    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("C compiler required for subprocess-only fsync failure injection")
    source = tmp_path / "fault.c"
    source.write_text(
        """
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
int fsync(int descriptor) {
    struct stat status;
    if (fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)) {
        errno = EIO;
        return -1;
    }
    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");
    return real_fsync(descriptor);
}
"""
    )
    library = tmp_path / "fault.so"
    subprocess.run(
        [compiler, "-shared", "-fPIC", str(source), "-o", str(library), "-ldl"],
        check=True,
        capture_output=True,
    )
    script = """
import sys
from pathlib import Path
from asterion_bindings.files import atomic_write, FilePublicationError
root = Path(sys.argv[1])
for replace in (False, True):
    target = root / str(replace)
    if replace:
        target.write_bytes(b'before')
    try:
        atomic_write(target, b'after', replace=replace)
    except FilePublicationError as failure:
        assert isinstance(failure, OSError)
        assert failure.published is True
        assert str(root) not in str(failure)
    else:
        raise AssertionError('directory sync failure must be reported')
    assert target.read_bytes() == b'after'
    try:
        atomic_write(target, b'must not replace', replace=False)
    except FileExistsError as failure:
        assert not isinstance(failure, FilePublicationError)
    else:
        raise AssertionError('publication conflict must preserve the target')
    assert target.read_bytes() == b'after'
assert not list(root.glob('.asterion-write-*'))
"""
    result = subprocess.run(
        [sys.executable, "-c", script, str(tmp_path)],
        env=os.environ | {"LD_PRELOAD": str(library)},
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
