"""Package command and diagnostic adapters; process ownership belongs to Rust L1."""

import sys
from pathlib import Path

from asterion_bindings.communication import call
from asterion_bindings.diagnostics import Observation, ProcessFailure
from asterion_bindings.transport import ProcessTransport
from asterion_bindings.transport import invoke as invoke_transport


def invoke(command, method, params, *, timeout=30, cwd=None, authorized=None):
    try:
        request = call(method, params, timeout=timeout)
    except TimeoutError:
        raise ProcessFailure("timeout", "插件执行超时") from None
    return invoke_transport(command, request, cwd=cwd, authorized=authorized)


def runtime_command():
    return [sys.executable, "-m", "asterion.runtime.cli"]


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
    except BaseException as error:
        observation.finish(error.code if isinstance(error, ProcessFailure) else "host_error")
        raise
    observation.finish()
    return result


class PackageSession:
    """Business-facing owner of one fixed-kernel transport session."""

    def __init__(self, path: Path, *, authorized, timeout=30):
        self.observation = Observation(path)
        self.timeout = timeout
        try:
            self.transport = ProcessTransport(
                [*runtime_command(), "plugin-run", "--target", str(path)],
                cwd=path,
                timeout=timeout,
                authorized=authorized,
            )
        except BaseException as error:
            self.observation.finish(error.code if isinstance(error, ProcessFailure) else "startup")
            raise

    def call(self, method, params):
        self.observation.call(method)
        try:
            return self.transport.call(call(method, params, timeout=self.timeout))
        except TimeoutError:
            self.observation.finish("timeout")
            self.close()
            raise ProcessFailure("timeout", "插件执行超时") from None
        except BaseException as error:
            self.observation.finish(error.code if isinstance(error, ProcessFailure) else "protocol")
            self.close()
            raise

    def close(self):
        self.transport.close()
        self.observation.finish()
