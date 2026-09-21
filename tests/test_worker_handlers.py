import json
import multiprocessing
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor

import httpx
import pytest
from credential_helpers import provider_secrets
from rules_support import intraday_options

from asterion.platform.authorization import worker_token
from asterion.platform.config import Settings
from asterion.platform.tasks.handlers import PublicationResult, TaskHandler, TaskHandlerRegistry
from asterion.runtime import worker
from asterion.runtime.handlers import handlers

CSV = """contract,event_time,available_at,trading_day,open,high,low,close,volume
SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100
"""


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
        worker.execute_job(Settings(), {"kind": "research.unknown"})


@pytest.mark.parametrize("suffix", ["https://other.test", "//other.test", "/../fail", "/x?q=1"])
def test_handler_publication_stays_under_claimed_job(suffix):
    with pytest.raises(ValueError, match="job-relative endpoint"):
        TaskHandler("research.report", offline_report, suffix)


def test_new_builtin_handler_runs_without_worker_dispatch_changes(monkeypatch):
    registry = TaskHandlerRegistry()
    registry.register(TaskHandler("research.report", offline_report, "/publish-report"))
    monkeypatch.setattr(worker, "handlers", registry)
    monkeypatch.setattr(
        worker, "ProcessPoolExecutor", lambda **kwargs: ThreadPoolExecutor(max_workers=1)
    )
    job = {
        "id": "job-report",
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
    job = {"id": "unknown", "kind": "unknown", "token": "lease-fence", "payload": {}}

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


@pytest.mark.parametrize(
    ("kind", "endpoint"), [("data.import_csv", "/publish"), ("data.sync", "/publish-data")]
)
def test_worker_preserves_executor_boundary_and_builtin_publication(monkeypatch, kind, endpoint):
    requests = []
    job = {"id": "built-in", "kind": kind, "token": "lease-fence", "payload": {}}

    def execute_job(settings, claimed):
        assert claimed == job
        return b"computed artifact", {"rows": 10}

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=job if request.url.path.endswith("/claim") else {})

    monkeypatch.setattr(worker, "execute_job", execute_job)
    monkeypatch.setattr(
        worker, "ProcessPoolExecutor", lambda **kwargs: ThreadPoolExecutor(max_workers=1)
    )
    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "test-worker")
    assert [request.url.path for request in requests] == [
        "/api/v1/jobs/claim",
        f"/api/v1/jobs/built-in{endpoint}",
    ]
    assert requests[-1].content == b"computed artifact"
    assert requests[-1].headers["X-Lease-Token"] == "lease-fence"


def test_csv_builtin_executes_in_spawned_process():
    assert handlers.get("data.import_csv").publish_suffix == "/publish"
    job = {
        "kind": "data.import_csv",
        "payload": {
            "options": intraday_options(),
            "csv": CSV,
        },
    }
    with ProcessPoolExecutor(
        max_workers=1, mp_context=multiprocessing.get_context("spawn")
    ) as pool:
        content, manifest = pool.submit(worker.execute_job, Settings(), job).result(timeout=20)
    assert content.startswith(b"PAR1")
    assert manifest["rows"] == 1
    assert manifest["contracts"] == ["SHFE.rb2610"]


def test_sync_builtin_preserves_progress_observations_and_resume(monkeypatch, tmp_path):
    job = {"id": "sync", "kind": "data.sync", "token": "lease-fence", "payload": {"plan": []}}
    requests = []
    resume = [{"index": 1, "validated": True}]

    def transport(request):
        requests.append(request)
        return httpx.Response(200, json=resume if request.url.path.endswith("/resume") else {})

    def collect(payload, data_root, secrets, progress, checkpoint, checkpoints):
        assert payload == job["payload"]
        assert data_root == tmp_path
        approved = provider_secrets("test-runtime-key")
        assert secrets.decrypt(approved.encrypt(b"fixture-credential")) == b"fixture-credential"
        assert secrets.fingerprint(b"fixture") == approved.fingerprint(b"fixture")
        assert checkpoints == resume
        progress(1, 2)
        checkpoint(1, {"observed": "fixture"})
        return b"collected artifact"

    monkeypatch.setattr("asterion.data.sync.collect", collect)
    http_client(monkeypatch, transport)
    result = worker.execute_job(
        Settings(token="test-runtime-key", api_url="http://worker.test", data_root=tmp_path), job
    )
    assert result == (b"collected artifact", {})
    assert handlers.get("data.sync").publish_suffix == "/publish-data"
    assert [request.url.path for request in requests] == [
        "/api/v1/jobs/sync/resume",
        "/api/v1/jobs/sync/progress",
        "/api/v1/jobs/sync/observations/1",
    ]
    assert requests[0].headers["X-Lease-Token"] == "lease-fence"
    assert json.loads(requests[1].content) == {"token": "lease-fence", "completed": 1, "total": 2}
    assert requests[2].headers["X-Lease-Token"] == "lease-fence"
    assert json.loads(requests[2].content) == {"observed": "fixture"}
    assert all(
        r.headers["Authorization"] == f"Bearer {worker_token('test-runtime-key')}" for r in requests
    )


@pytest.mark.parametrize(
    ("kind", "detail", "error", "match"),
    [
        ("data.sync", "价格边界错误", ValueError, "价格边界错误"),
        ("data.sync", {"field": "price"}, ValueError, "数据发布校验失败"),
        ("data.import_csv", "invalid CSV", ValueError, "422"),
    ],
)
def test_publication_error_semantics_belong_to_handler(kind, detail, error, match):
    response = PublicationResult(422, detail)
    with pytest.raises(error, match=match):
        handlers.get(kind).check_publication(response)


def test_executor_receives_input_and_only_explicit_resources(monkeypatch):
    from asterion.platform.resource import Resource

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
        worker, "execution_resources", lambda settings, kind, post: {resource: "approved"}
    )
    assert worker.execute_job(
        Settings(token="hidden"),
        {
            "kind": "fixture.run",
            "token": "lease",
            "payload": {"input": "value"},
        },
    ) == (b"result", {})
    with pytest.raises(ValueError, match="closed"):
        retained[0].resource(resource)


@pytest.mark.parametrize("invalid", ["missing", "extra", "type", "duplicate"])
def test_execution_grants_are_checked_before_invoking_handler(monkeypatch, invalid):
    from asterion.platform.resource import Resource

    resource = Resource("fixture.input", str)
    declarations = (resource, resource) if invalid == "duplicate" else (resource,)
    bindings = {resource: 1 if invalid == "type" else "approved"}
    if invalid == "missing":
        bindings.clear()
    if invalid == "extra":
        bindings[Resource("fixture.other", str)] = "not approved"
    calls = []
    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry(
            (
                TaskHandler(
                    "fixture.run",
                    lambda context, payload: calls.append(payload),
                    "/publish",
                    resources=declarations,
                ),
            )
        ),
    )
    monkeypatch.setattr(worker, "execution_resources", lambda settings, kind, post: bindings)
    with pytest.raises((ValueError, TypeError)):
        worker.execute_job(Settings(), {"kind": "fixture.run", "payload": {}})
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
    monkeypatch.setattr(
        worker, "ProcessPoolExecutor", lambda **kwargs: ThreadPoolExecutor(max_workers=1)
    )

    def transport(request):
        requests.append(request.url.path)
        if request.url.path.endswith("/claim"):
            return httpx.Response(
                200, json={"id": "fixture", "kind": "fixture.run", "token": "lease", "payload": {}}
            )
        if request.url.path.endswith("/publish"):
            return httpx.Response(500, json={"detail": "failed"})
        return httpx.Response(200, json={})

    http_client(monkeypatch, transport)
    assert worker.run_once(Settings(api_url="http://worker.test"), "worker")
    assert requests[-1] == "/api/v1/jobs/fixture/fail"


def test_sync_reporting_is_job_bound_expires_and_does_not_expose_http_errors(monkeypatch):
    from asterion.data.public import SYNC_REPORTER

    retained = []
    requests = []

    def execute(context, payload):
        reporter = context.resource(SYNC_REPORTER)
        retained.append(reporter)
        with pytest.raises(ValueError, match="Invalid observation index"):
            reporter.checkpoint("../other", {})
        with pytest.raises(ValueError, match="任务执行通道") as failure:
            reporter.resume()
        assert not hasattr(failure.value, "request")
        assert "runtime-secret" not in str(failure.value)
        return b"result", {}

    from dataclasses import replace

    monkeypatch.setattr(
        worker,
        "handlers",
        TaskHandlerRegistry((replace(handlers.get("data.sync"), execute=execute),)),
    )

    def transport(request):
        requests.append(request)
        return httpx.Response(409, json={"detail": "expired lease"})

    http_client(monkeypatch, transport)
    worker.execute_job(
        Settings(token="runtime-secret", api_url="http://worker.test"),
        {
            "id": "current",
            "kind": "data.sync",
            "token": "lease",
            "payload": {},
        },
    )
    assert [request.url.path for request in requests] == ["/api/v1/jobs/current/resume"]
    assert requests[0].headers["X-Lease-Token"] == "lease"
    with pytest.raises(ValueError, match="closed"):
        retained[0].resume()
    assert len(requests) == 1
