"""Test-only stand-in for the Rust entry in front of the internal Python app.

In production, requests reach the Python process only through `asterion-server`,
which authorizes the client credential with the fixed kernel authority and
forwards its decision. Python API tests make the same kernel decision from the
app's exported authorization declaration, then forward exactly as the entry
does. Accounts are owned by the entry: tests declare the account state the
entry reports for a session in `ACCOUNTS`. The entry's own behaviour, including
how it evaluates sessions, is tested in services/server/tests.
"""

import json
import time
from contextlib import contextmanager

from asterion_bindings._call import invoke
from asterion_bindings.authority import forwarding_token

STRIPPED = {
    b"authorization",
    b"x-asterion-forwarded",
    b"x-asterion-principal",
    b"x-asterion-account",
    b"x-asterion-account-status",
}
# Session -> (status, email) as the entry reports it; unknown sessions expired.
ACCOUNTS: dict[str, tuple[str, str]] = {}


class Entry:
    def __init__(self, app):
        self.app = app

    def __getattr__(self, name):
        return getattr(self.app, name)

    def principal(self, scope, headers):
        declaration = self.app.state.entry_authorization
        credential = headers.get(b"authorization", b"").decode()
        path = scope["path"].removeprefix("/api/v1")
        return invoke(
            "kernel",
            "auth.authorize",
            {
                "secret": self.app.state.settings.token,
                "policies": declaration["policies"],
                "worker_grants": declaration["worker_grants"],
                "credential": credential.removeprefix("Bearer ")
                if credential.startswith("Bearer ")
                else "",
                "method": scope["method"],
                "path": path,
                "session": headers.get(b"x-account-session", b"").decode(),
                "now": time.time(),
            },
        )

    def scope_token(self, scope, session=""):
        """A functional scope credential as the entry issues it to the workbench."""
        return invoke(
            "kernel",
            "auth.issue",
            {
                "secret": self.app.state.settings.token,
                "policies": self.app.state.entry_authorization["policies"],
                "scope": scope,
                "session": session,
                "now": time.time(),
            },
        )["token"]

    async def __call__(self, scope, receive, send):
        state = getattr(self.app, "state", None)
        if scope["type"] != "http" or not hasattr(state, "entry_authorization"):
            return await self.app(scope, receive, send)
        headers = dict(scope["headers"])
        try:
            principal = self.principal(scope, headers)
        except ValueError:
            body = json.dumps(
                {"detail": "请求身份无效或无权访问此接口", "code": "UNAUTHORIZED"},
                ensure_ascii=False,
            ).encode()
            await send(
                {
                    "type": "http.response.start",
                    "status": 401,
                    "headers": [(b"content-type", b"application/json")],
                }
            )
            await send({"type": "http.response.body", "body": body})
            return
        forwarded = [(k, v) for k, v in scope["headers"] if k.lower() not in STRIPPED]
        status, email = ACCOUNTS.get(
            headers.get(b"x-account-session", b"").decode(), ("expired", "")
        )
        forwarded += [
            (b"x-asterion-forwarded", forwarding_token(state.settings.token).encode()),
            (b"x-asterion-principal", principal.encode()),
            (b"x-asterion-account-status", status.encode()),
        ]
        if email:
            forwarded.append((b"x-asterion-account", email.encode()))
        await self.app(dict(scope, headers=forwarded), receive, send)


def _listening(process, port):
    import socket

    deadline = time.monotonic() + 30
    while True:
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode() if process.stderr else "")
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)


@contextmanager
def _entry(settings, upstream):
    import subprocess

    from asterion.runtime.desktop import entry_environment, free_port, server_executable

    port = free_port()
    process = subprocess.Popen(
        [str(server_executable()), "--listen", f"127.0.0.1:{port}", "--upstream", upstream],
        # Sync tasks the entry collects in tests never reach a real source.
        env=entry_environment(settings)
        | {"HTTPS_PROXY": "http://127.0.0.1:9", "HTTP_PROXY": "http://127.0.0.1:9"},
        stderr=subprocess.PIPE,
    )
    try:
        _listening(process, port)
        yield f"http://127.0.0.1:{port}"
    finally:
        process.terminate()
        process.wait(10)


def start_entry_once(settings):
    """Run the real entry until it listens, as every runtime start does.

    The entry verifies or creates the account tables it owns before listening.
    """
    with _entry(settings, "http://127.0.0.1:9"):
        pass


@contextmanager
def running_entry(app, settings):
    """Serve the internal app behind the real entry, as the supervisor does;
    yields the entry's URL. The entry uses `settings.database_url`."""
    import threading

    import uvicorn

    from asterion.runtime.desktop import free_port

    internal = free_port()
    server = uvicorn.Server(
        uvicorn.Config(app, host="127.0.0.1", port=internal, log_level="warning")
    )
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    try:
        deadline = time.monotonic() + 30
        while not server.started:
            if not thread.is_alive() or time.monotonic() > deadline:
                raise RuntimeError("The internal app did not start")
            time.sleep(0.05)
        with _entry(settings, f"http://127.0.0.1:{internal}") as url:
            yield url
    finally:
        server.should_exit = True
        thread.join(10)


class EntryLifecycle:
    """Version archiving through the real entry, which owns it."""

    def __init__(self, client):
        self.client = client

    @staticmethod
    def _result(response):
        from asterion_bindings.task_repository import Conflict

        if response.status_code == 404:
            raise KeyError(response.json()["detail"])
        if response.status_code == 409:
            raise Conflict(response.json()["detail"])
        response.raise_for_status()
        return response.json()

    def inspect(self, version_id):
        return self._result(self.client.get(f"/data/versions/{version_id}/lifecycle"))

    def archive(self, version_id, *, archived, expected_revision):
        body = {"archived": archived, "expected_revision": expected_revision}
        return self._result(self.client.post(f"/data/versions/{version_id}/archive", json=body))


@contextmanager
def entry_lifecycle(database_url, data_root, token="entry-lifecycle-test-token-at-least-24"):
    """The real entry on an existing database, without the internal process."""
    import httpx

    from asterion.platform.config import Settings

    settings = Settings(database_url=database_url, data_root=data_root, token=token)
    with (
        _entry(settings, "http://127.0.0.1:9") as url,
        httpx.Client(
            base_url=url + "/api/v1",
            headers={"Authorization": f"Bearer {token}"},
            trust_env=False,
            timeout=30,
        ) as client,
    ):
        yield EntryLifecycle(client)
