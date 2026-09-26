import json
from contextlib import nullcontext

import httpx
import pytest
from asterion_bindings.authority import Grant, worker_token
from asterion_bindings.communication import context as communication_context
from asterion_bindings.task_handlers import TaskHandler, TaskHandlerRegistry
from worker_support import inline_computation

from asterion.platform.config import Settings
from asterion.runtime import worker

CSV = """contract,event_time,available_at,trading_day,open,high,low,close,volume
SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100
"""


@pytest.fixture(autouse=True)
def isolated_worker_state(monkeypatch, tmp_path):
    monkeypatch.setenv("ASTERION_DATA_ROOT", str(tmp_path / "data"))


def offline_report(context, payload):
    return b"offline report", {"input": payload["input"]}


def http_client(monkeypatch, transport):
    client_type = httpx.Client
    monkeypatch.setattr(
        httpx,
        "Client",
        lambda **kwargs: client_type(transport=httpx.MockTransport(transport), **kwargs),
    )


def test_registry_rejects_duplicate_and_unknown_kinds():
    handler = TaskHandler("research.report", offline_report, "/publish-report")
    registry = TaskHandlerRegistry((handler,))
    assert registry.get("research.report") is handler
    with pytest.raises(ValueError, match="Duplicate task handler"):
        registry.register(handler)
    with pytest.raises(ValueError, match="Unsupported job kind"):
        registry.get("research.unknown")
    with pytest.raises(ValueError, match="Unsupported job kind"):
        worker.execute_job(
            Settings(), {"communication": communication_context(), "kind": "research.unknown"}
        )


def test_empty_claim_does_not_start_a_computation_process(monkeypatch):
    requests = []

    def forbidden(*args, **kwargs):
        pytest.fail("No task was claimed; TaskProcess must not start")

    def transport(request):
        requests.append(request.url.path)
        return httpx.Response(200, content=b"null", headers={"Content-Type": "application/json"})

    monkeypatch.setattr(worker, "TaskProcess", forbidden)
    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "idle-worker") is False
    assert requests == ["/api/v1/jobs/claim"]


@pytest.mark.parametrize("suffix", ["https://other.test", "//other.test", "/../fail", "/x?q=1"])
def test_handler_publication_stays_under_claimed_job(suffix):
    with pytest.raises(ValueError, match="job-relative endpoint"):
        TaskHandler("research.report", offline_report, suffix)


@pytest.mark.parametrize("kind", ["report", ".report", "research.", "Research.report", "a..b"])
def test_handler_requires_rust_validated_namespaced_kind(kind):
    with pytest.raises(ValueError, match="namespaced ID"):
        TaskHandler(kind, offline_report, "/publish")


@pytest.mark.parametrize(
    "grant",
    [
        Grant("/jobs/:id/progress", ("GET",)),
        Grant("/jobs/:id/progress", ("POST", "GET")),
        Grant("/jobs/:id/progress", ("POST",), descendants=True),
        Grant("/other/:id/progress", ("POST",)),
        Grant("/jobs/:id/", ("POST",)),
        Grant("/jobs/:id/../progress", ("POST",)),
        Grant("/jobs/:id/progress?job=other", ("POST",)),
        Grant("/jobs/:id/progress/:other", ("POST",)),
        Grant("/jobs/:id/:id", ("POST",)),
    ],
)
def test_handler_rejects_invalid_worker_request_declarations(grant):
    with pytest.raises(ValueError, match="exact job-relative POST"):
        TaskHandler("fixture.run", offline_report, "/publish", requests=(grant,))


@pytest.mark.parametrize("operation", ["claim", "heartbeat", "fail", "cancel"])
def test_handler_cannot_contribute_task_control_operations(operation):
    with pytest.raises(ValueError, match="control operations"):
        TaskHandler("fixture.run", offline_report, f"/{operation}")
    with pytest.raises(ValueError, match="control operations"):
        TaskHandler(
            "fixture.run",
            offline_report,
            "/publish",
            requests=(Grant(f"/jobs/:id/{operation}", ("POST",)),),
        )


def test_native_handler_registration_rejection_does_not_shift_callback_indices():
    first = TaskHandler("fixture.first", offline_report, "/publish-first")
    second = TaskHandler("fixture.second", offline_report, "/publish-second")
    registry = TaskHandlerRegistry((first,))
    with pytest.raises(ValueError, match="Duplicate task handler"):
        registry.register(first)
    registry.register(second)
    assert registry.get(first.kind) is first
    assert registry.get(second.kind) is second
    assert {grant.path for grant in registry.worker_grants()} == {
        "/jobs/claim",
        "/jobs/:id/heartbeat",
        "/jobs/:id/fail",
        "/jobs/:id/publish-first",
        "/jobs/:id/publish-second",
    }


def test_worker_request_port_checks_current_handler_before_http_and_closes(monkeypatch):
    retained = []
    requests = []

    def resources(settings, kind, post):
        retained.append(post)
        return nullcontext({})

    def execute(context, payload):
        post = retained[0]
        assert post("/progress", {"completed": 1}, lease_in_body=True) == {}
        for suffix in (
            "/other",
            "/heartbeat",
            "/fail",
            "/publish",
            "/progress/../other",
            "/progress?job=other",
            "//other.test/progress",
        ):
            with pytest.raises(ValueError, match="not granted"):
                post(suffix, {}, lease_in_body=True)
        return b"complete", {}

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json={})

    registry = TaskHandlerRegistry(
        (
            TaskHandler(
                "fixture.run",
                execute,
                "/publish",
                requests=(Grant("/jobs/:id/progress", ("POST",)),),
            ),
            TaskHandler(
                "fixture.other",
                offline_report,
                "/publish-other",
                requests=(Grant("/jobs/:id/other", ("POST",)),),
            ),
        )
    )
    monkeypatch.setattr(worker, "handlers", registry)
    monkeypatch.setattr(worker, "execution_resources", resources)
    http_client(monkeypatch, transport)
    assert worker.execute_job(
        Settings(api_url="http://worker.test"),
        {
            "communication": communication_context(),
            "id": "current",
            "kind": "fixture.run",
            "token": "lease",
            "payload": {},
        },
    ) == (b"complete", {})
    assert len(requests) == 1
    assert requests[0].url.path == "/api/v1/jobs/current/progress"
    assert json.loads(requests[0].content) == {"completed": 1, "token": "lease"}
    assert requests[0].headers["X-Lease-Token"] == "lease"
    with pytest.raises(ValueError, match="closed"):
        retained[0]("/progress")
    assert len(requests) == 1


@pytest.mark.parametrize("phase", ["resources", "execute"])
def test_worker_request_scope_closes_when_setup_or_callback_fails(monkeypatch, phase):
    retained = []

    def resources(settings, kind, post):
        retained.append(post)
        if phase == "resources":
            raise RuntimeError("Resource activation failed")
        return nullcontext({})

    def execute(context, payload):
        raise RuntimeError("Task execution failed")

    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry(
            (
                TaskHandler(
                    "fixture.run",
                    execute,
                    "/publish",
                    requests=(Grant("/jobs/:id/progress", ("POST",)),),
                ),
            )
        ),
    )
    monkeypatch.setattr(worker, "execution_resources", resources)
    with pytest.raises(RuntimeError, match="failed"):
        worker.execute_job(
            Settings(api_url="http://worker.test"),
            {
                "communication": communication_context(),
                "id": "current",
                "kind": "fixture.run",
                "token": "lease",
                "payload": {},
            },
        )
    with pytest.raises(ValueError, match="closed"):
        retained[0]("/progress")


def test_new_builtin_handler_runs_without_worker_dispatch_changes(monkeypatch):
    registry = TaskHandlerRegistry()
    registry.register(TaskHandler("research.report", offline_report, "/publish-report"))
    monkeypatch.setattr(worker, "handlers", registry)
    inline_computation(monkeypatch)
    job = {
        "id": "job-report",
        "communication": communication_context(),
        "kind": "research.report",
        "token": "lease-fence",
        "payload": {"input": "immutable-version"},
    }
    requests = []

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=job if request.url.path.endswith("/claim") else {})

    http_client(monkeypatch, transport)
    settings = Settings(token="test-runtime-key", api_url="http://worker.test")
    assert worker.execute_job(settings, job) == (b"offline report", {"input": "immutable-version"})
    assert worker.run_once(settings, "test-worker")
    assert [request.url.path for request in requests] == [
        "/api/v1/jobs/claim",
        "/api/v1/jobs/job-report/publish-report",
    ]
    published = requests[-1]
    assert published.content == b"offline report"
    assert published.headers["X-Lease-Token"] == "lease-fence"
    assert published.headers["Authorization"] == f"Bearer {worker_token('test-runtime-key')}"


def test_unknown_claim_fails_without_falling_back_to_csv_publication(monkeypatch):
    requests = []
    job = {
        "id": "unknown",
        "communication": communication_context(),
        "kind": "unknown",
        "token": "lease-fence",
        "payload": {},
    }

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=job if request.url.path.endswith("/claim") else {})

    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "test-worker")
    assert [request.url.path for request in requests] == [
        "/api/v1/jobs/claim",
        "/api/v1/jobs/unknown/fail",
    ]
    assert json.loads(requests[-1].content) == {
        "token": "lease-fence",
        "error": "Unsupported job kind: unknown",
    }


@pytest.mark.parametrize(("kind", "endpoint"), [("research.backtest", "/publish-research")])
def test_worker_preserves_executor_boundary_and_builtin_publication(monkeypatch, kind, endpoint):
    requests = []
    job = {
        "id": "built-in",
        "communication": communication_context(),
        "kind": kind,
        "token": "lease-fence",
        "payload": {},
    }

    def execute_job(settings, claimed):
        assert claimed == job
        return b"computed artifact", {"rows": 10}

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=job if request.url.path.endswith("/claim") else {})

    monkeypatch.setattr(worker, "execute_job", execute_job)
    inline_computation(monkeypatch)
    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "test-worker")
    assert [request.url.path for request in requests] == [
        "/api/v1/jobs/claim",
        f"/api/v1/jobs/built-in{endpoint}",
    ]
    assert requests[-1].content == b"computed artifact"
    assert requests[-1].headers["X-Lease-Token"] == "lease-fence"


def test_executor_receives_input_and_only_explicit_resources(monkeypatch):
    from asterion_bindings.resource import Resource

    resource = Resource("fixture.input", str)
    retained = []

    def execute(context, payload):
        assert payload == {"input": "value"}
        assert context.resource(resource) == "approved"
        with pytest.raises(ValueError, match="not granted"):
            context.resource(Resource("fixture.secret", str))
        retained.append(context)
        return b"result", {}

    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry(
            (TaskHandler("fixture.run", execute, "/publish", resources=(resource,)),)
        ),
    )
    monkeypatch.setattr(
        worker,
        "execution_resources",
        lambda settings, kind, post: nullcontext({resource: "approved"}),
    )
    assert worker.execute_job(
        Settings(token="hidden"),
        {
            "communication": communication_context(),
            "kind": "fixture.run",
            "token": "lease",
            "payload": {"input": "value"},
        },
    ) == (b"result", {})
    with pytest.raises(ValueError, match="closed"):
        retained[0].resource(resource)


@pytest.mark.parametrize("invalid", ["missing", "extra", "type", "duplicate"])
def test_execution_grants_are_checked_before_invoking_handler(monkeypatch, invalid):
    from asterion_bindings.resource import Resource

    resource = Resource("fixture.input", str)
    declarations = (resource, resource) if invalid == "duplicate" else (resource,)
    bindings = {resource: 1 if invalid == "type" else "approved"}
    if invalid == "missing":
        bindings.clear()
    if invalid == "extra":
        bindings[Resource("fixture.other", str)] = "not approved"
    calls = []

    def execute(context, payload):
        calls.append(payload)
        return b"unexpected result", {}

    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry(
            (
                TaskHandler(
                    "fixture.run",
                    execute,
                    "/publish",
                    resources=declarations,
                ),
            )
        ),
    )
    monkeypatch.setattr(worker, "execution_resources", lambda settings, kind, post: bindings)
    with pytest.raises((ValueError, TypeError)):
        worker.execute_job(
            Settings(),
            {"communication": communication_context(), "kind": "fixture.run", "payload": {}},
        )
    assert calls == []


def test_publication_callback_cannot_bypass_core_http_failure_check(monkeypatch):
    requests = []

    def check(result):
        assert result.status_code == 500
        assert result.detail == "failed"
        assert set(result.__dataclass_fields__) == {"status_code", "detail"}

    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry(
            (
                TaskHandler(
                    "fixture.run", lambda context, payload: (b"result", {}), "/publish", check
                ),
            )
        ),
    )
    inline_computation(monkeypatch)

    def transport(request):
        requests.append(request.url.path)
        if request.url.path.endswith("/claim"):
            return httpx.Response(
                200,
                json={
                    "id": "fixture",
                    "communication": communication_context(),
                    "kind": "fixture.run",
                    "token": "lease",
                    "payload": {},
                },
            )
        if request.url.path.endswith("/publish"):
            return httpx.Response(500, json={"detail": "failed"})
        return httpx.Response(200, json={})

    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "worker")
    assert requests[-1] == "/api/v1/jobs/fixture/fail"


def test_worker_failure_logs_only_native_categories(monkeypatch, tmp_path, caplog):
    from asterion_bindings.communication import activate
    from asterion_bindings.diagnostics import events

    requests = []
    job = {
        "id": "private-job-reference",
        "communication": communication_context(),
        "kind": "fixture.fail",
        "token": "private-lease-token",
        "payload": {"input": "private-payload"},
    }

    def fail(context, payload):
        raise ValueError("private-exception-detail")

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=job if request.url.path.endswith("/claim") else {})

    monkeypatch.setattr(
        worker, "handlers", TaskHandlerRegistry((TaskHandler("fixture.fail", fail, "/publish"),))
    )
    monkeypatch.setattr(
        worker, "execute_job", lambda settings, claimed: fail(None, claimed["payload"])
    )
    inline_computation(monkeypatch)
    http_client(monkeypatch, transport)
    other = communication_context()
    with activate(other):
        assert worker.run_once(
            Settings(token="private-runtime-key", data_root=tmp_path, api_url="http://worker.test"),
            "worker",
        )
    assert requests[-1].url.path.endswith("/fail")
    rows = events(tmp_path / ".diagnostics")
    assert rows[0]["component"] == "worker" and rows[0]["code"] == "execution_failed"
    assert rows[0]["correlation_id"] == job["communication"]["correlation_id"]
    assert rows[0]["request_id"] == job["communication"]["request_id"]
    assert rows[0]["request_id"] != other["request_id"]
    assert "private" not in json.dumps(rows)
    assert "private" not in caplog.text
    assert b"private" not in (tmp_path / ".diagnostics" / "diagnostics.sqlite").read_bytes()


def test_control_plane_failure_stays_failed_when_native_logging_cannot_write(monkeypatch, tmp_path):
    import httpx

    def unavailable(request):
        raise httpx.ConnectError("private transport detail", request=request)

    http_client(monkeypatch, unavailable)
    (tmp_path / ".diagnostics").write_text("preserve")
    with pytest.raises(httpx.ConnectError, match="private transport detail"):
        worker.run(
            Settings(
                token="diagnostics-test-runtime-key-long",
                data_root=tmp_path,
                api_url="http://worker.test",
            ),
            once=True,
        )
    assert (tmp_path / ".diagnostics").read_text() == "preserve"
