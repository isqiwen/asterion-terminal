"""Bounded one-call processes. Same-user local code is trusted, not sandboxed."""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from uuid import uuid4

from asterion.platform.extensions.diagnostics import Observation, ProcessFailure


def invoke(command, method, params, *, timeout=30, cwd=None, authorized=None):
    if authorized and not authorized():
        raise ProcessFailure("revoked", "插件已停用或移除")
    identifier = uuid4().hex
    payload = json.dumps(
        {"protocol": 1, "id": identifier, "method": method, "params": params}, allow_nan=False
    ).encode()
    if len(payload) > 8_000_000:
        raise ProcessFailure("request_limit", "插件请求超过限制")
    # No runtime secrets, DB URL, PYTHONPATH or inherited API identity are forwarded.
    environment = {
        key: value
        for key, value in os.environ.items()
        if key in {"PATH", "LANG", "LC_ALL", "SYSTEMROOT", "TMPDIR"}
    }
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    with (
        tempfile.TemporaryFile() as incoming,
        tempfile.TemporaryFile() as outgoing,
        tempfile.TemporaryFile() as errors,
    ):
        incoming.write(payload)
        incoming.seek(0)
        process = subprocess.Popen(
            command,
            stdin=incoming,
            stdout=outgoing,
            stderr=errors,
            cwd=cwd,
            env=environment,
            start_new_session=True,
        )
        deadline = time.monotonic() + timeout
        try:
            while process.poll() is None:
                if authorized and not authorized():
                    raise ProcessFailure("revoked", "插件已停用或移除")
                if time.monotonic() > deadline:
                    raise ProcessFailure("timeout", "插件执行超时")
                if (
                    os.fstat(outgoing.fileno()).st_size + os.fstat(errors.fileno()).st_size
                    > 8_000_000
                ):
                    raise ProcessFailure("output_limit", "插件输出超过限制")
                time.sleep(0.02)
            if process.returncode:
                raise ProcessFailure("process_exit", "插件进程失败，请检查插件契约和运行环境")
            outgoing.seek(0)
            content = outgoing.read(8_000_001)
            if len(content) > 8_000_000:
                raise ProcessFailure("output_limit", "插件输出超过限制")
            value = json.loads(
                content,
                parse_constant=lambda value: (_ for _ in ()).throw(
                    ValueError("Non-finite plugin number")
                ),
            )
            if (
                not isinstance(value, dict)
                or value.get("protocol") != 1
                or value.get("id") != identifier
                or set(value) != {"protocol", "id", "result"}
            ):
                raise ProcessFailure("protocol", "插件响应不符合当前契约或执行失败")
            return value["result"]
        except (json.JSONDecodeError, UnicodeError):
            raise ProcessFailure("protocol", "插件返回无效 JSON") from None
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()


def runtime_command():
    return (
        [sys.executable]
        if getattr(sys, "frozen", False)
        else [sys.executable, "-m", "asterion.runtime.cli"]
    )


def call_package(path: Path, method, params, *, timeout=30, authorized=None):
    observation = Observation(path)
    observation.call(method)
    try:
        result = invoke(
            [*runtime_command(), "plugin-run", "--target", str(path)],
            method,
            params,
            timeout=timeout,
            cwd=path,
            authorized=authorized,
        )
    except Exception as error:
        observation.finish(error.code if isinstance(error, ProcessFailure) else "host_error")
        raise
    observation.finish()
    return result


class PackageSession:
    """Bounded JSON-line RPC session; each call can be revoked while waiting."""

    def __init__(self, path: Path, *, authorized, timeout=30):
        self.observation = Observation(path)
        self.authorized = authorized
        self.deadline = time.monotonic() + timeout
        self.closed = False
        self.buffer = b""
        if not authorized():
            self.observation.finish("revoked")
            raise ProcessFailure("revoked", "插件已停用或移除")
        environment = {
            k: v
            for k, v in os.environ.items()
            if k in {"PATH", "LANG", "LC_ALL", "SYSTEMROOT", "TMPDIR"}
        }
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        self.errors = tempfile.TemporaryFile()  # noqa: SIM115 - owned until session close
        try:
            self.process = subprocess.Popen(
                [*runtime_command(), "plugin-run", "--target", str(path)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self.errors,
                cwd=path,
                env=environment,
                start_new_session=True,
            )
            assert self.process.stdin is not None and self.process.stdout is not None
            os.set_blocking(self.process.stdin.fileno(), False)
            os.set_blocking(self.process.stdout.fileno(), False)
        except BaseException:
            self.observation.finish("startup")
            if hasattr(self, "process"):
                self.close()
            else:
                self.errors.close()
            raise

    def call(self, method, params):
        import select

        if self.closed:
            raise ValueError("插件会话已关闭")
        self.observation.call(method)
        identifier = uuid4().hex
        pending = (
            json.dumps(
                {"protocol": 1, "id": identifier, "method": method, "params": params},
                allow_nan=False,
            ).encode()
            + b"\n"
        )
        try:
            if len(pending) > 8_000_000:
                raise ProcessFailure("request_limit", "插件请求超过限制")
            while True:
                if not self.authorized():
                    raise ProcessFailure("revoked", "插件已停用或移除")
                if time.monotonic() >= self.deadline:
                    raise ProcessFailure("timeout", "插件会话执行超时")
                if len(self.buffer) + os.fstat(self.errors.fileno()).st_size > 8_000_000:
                    raise ProcessFailure("output_limit", "插件输出超过限制")
                if b"\n" in self.buffer:
                    raw, self.buffer = self.buffer.split(b"\n", 1)
                    value = json.loads(
                        raw,
                        parse_constant=lambda _: (_ for _ in ()).throw(
                            ValueError("插件非有限数值")
                        ),
                    )
                    if (
                        pending
                        or not isinstance(value, dict)
                        or set(value) != {"protocol", "id", "result"}
                        or value["protocol"] != 1
                        or value["id"] != identifier
                    ):
                        raise ProcessFailure("protocol", "插件响应不符合当前会话契约")
                    return value["result"]
                assert self.process.stdin is not None and self.process.stdout is not None
                readable, writable, _ = select.select(
                    [self.process.stdout], [self.process.stdin] if pending else [], [], 0.02
                )
                if writable:
                    sent = os.write(self.process.stdin.fileno(), pending)
                    pending = pending[sent:]
                if readable:
                    chunk = os.read(self.process.stdout.fileno(), 65536)
                    if not chunk:
                        raise ProcessFailure("process_exit", "插件会话提前退出")
                    self.buffer += chunk
        except (OSError, ValueError, UnicodeError) as error:
            self.observation.finish(error.code if isinstance(error, ProcessFailure) else "protocol")
            self.close()
            raise ValueError("插件会话失败、超时或已停用，请检查插件状态与契约") from None

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.observation.finish()
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.process.wait()
        if self.process.stdin:
            self.process.stdin.close()
        if self.process.stdout:
            self.process.stdout.close()
        self.errors.close()
