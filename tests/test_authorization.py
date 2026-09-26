import pytest
from asterion_bindings.authority import Authority, Grant, worker_token
from asterion_bindings.database import create_engine
from fastapi.testclient import TestClient

from asterion.api.app import create_app
from asterion.platform.config import Settings


def test_scope_is_bound_to_session_policy_expiry_and_environment():
    now = [100]
    policies = {"reader": (Grant("/records/:id", ("GET",)),)}
    authority = Authority("first-secret", policies, (), clock=lambda: now[0])
    token = authority.issue("reader", "account-one")["token"]
    assert authority.authorize(token, "GET", "/records/one", "account-one") == "reader"
    for method, path, session in [
        ("POST", "/records/one", "account-one"),
        ("GET", "/records/one/remove", "account-one"),
        ("GET", "/account", "account-one"),
        ("GET", "/records/one", "account-two"),
        ("GET", "/records/../account", "account-one"),
    ]:
        with pytest.raises(ValueError):
            authority.authorize(token, method, path, session)
    with pytest.raises(ValueError):
        Authority("other-secret", policies, ()).authorize(
            token, "GET", "/records/one", "account-one"
        )
    now[0] = 400
    with pytest.raises(ValueError):
        authority.authorize(token, "GET", "/records/one", "account-one")


def test_server_scopes_cannot_escalate_and_worker_cannot_read_account_or_install(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/auth.db")
    settings = Settings(token="test-authorization-master-long-enough", data_root=tmp_path / "data")
    try:
        with TestClient(create_app(settings, engine)) as client:
            client.headers["Authorization"] = f"Bearer {client.scope('connections')}"
            assert client.get("/api/v1/connections").status_code == 200
            assert client.get("/api/v1/jobs").status_code == 401
            assert (
                client.post("/api/v1/access/scopes", json={"scope": "extensions"}).status_code
                == 401
            )
            assert (
                client.post("/api/v1/extensions/install", json={"archive": ""}).status_code == 401
            )
            client.headers["Authorization"] = f"Bearer {worker_token(settings.token)}"
            # Worker grants reach worker operations; the unknown task is refused later.
            assert client.post("/api/v1/jobs/missing/publish-research").status_code != 401
            assert client.get("/api/v1/connections").status_code == 401
            assert client.get("/api/v1/extensions").status_code == 401
    finally:
        engine.dispose()


def test_research_can_read_identity_catalogues_but_cannot_publish():
    from asterion.distribution import request_policies

    authority = Authority("catalogue-scope-test", request_policies(), ())
    token = authority.issue("research", "account")["token"]
    for path in ("/reference/releases", "/reference/releases/fixed"):
        assert authority.authorize(token, "GET", path, "account") == "research"
        with pytest.raises(ValueError):
            authority.authorize(token, "POST", path, "account")
    with pytest.raises(ValueError):
        authority.authorize(token, "POST", "/reference/source/publish", "account")


def test_internal_app_accepts_only_requests_forwarded_by_the_entry(tmp_path):
    """Direct loopback callers cannot bypass the Rust entry, even with the token."""
    from asterion_bindings.authority import forwarding_token
    from starlette.testclient import TestClient as DirectClient

    engine = create_engine(f"sqlite:///{tmp_path}/direct.db")
    settings = Settings(token="test-direct-access-master-long-enough", data_root=tmp_path / "data")
    forwarded = forwarding_token(settings.token)
    try:
        with DirectClient(create_app(settings, engine)) as client:
            for headers in [
                {"Authorization": f"Bearer {settings.token}"},
                {"X-Asterion-Principal": "root"},
                {"X-Asterion-Forwarded": "0" * 64, "X-Asterion-Principal": "root"},
                {"X-Asterion-Forwarded": forwarded, "X-Asterion-Principal": "unknown"},
                {"X-Asterion-Forwarded": forwarded},
                {
                    "X-Asterion-Forwarded": worker_token(settings.token),
                    "X-Asterion-Principal": "root",
                },
            ]:
                assert client.get("/api/v1/health", headers=headers).status_code == 401, headers
            accepted = {"X-Asterion-Forwarded": forwarded, "X-Asterion-Principal": "root"}
            assert client.get("/api/v1/health", headers=accepted).status_code == 200
    finally:
        engine.dispose()
