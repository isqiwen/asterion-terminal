import pytest
from asterion_bindings.database import create_engine
from fastapi.testclient import TestClient
from storage_support import raw_engine

from asterion.api.app import create_app
from asterion.platform.config import Settings

TOKEN = "account-access-test-token-24"


@pytest.fixture
def client(tmp_path, accounts):
    engine = create_engine(f"sqlite:///{tmp_path}/access.db")
    app = create_app(
        Settings(token=TOKEN, data_root=tmp_path, require_account=True), raw_engine(engine)
    )
    with TestClient(app, headers={"Authorization": f"Bearer {TOKEN}"}) as client:
        yield client
    engine.dispose()


@pytest.mark.parametrize(
    ("state", "status", "code"),
    [
        (("unlocked", "owner@example.com"), 200, None),
        (("locked", "owner@example.com"), 423, "TERMINAL_LOCKED"),
        (("expired", ""), 401, "SESSION_EXPIRED"),
        (("unsupported", ""), 409, "UNSUPPORTED_ACCOUNT_SECURITY"),
        (("unavailable", ""), 503, "ACCOUNT_UNAVAILABLE"),
        # An entry decision without its account is refused, never guessed.
        (("unlocked", ""), 503, "ACCOUNT_UNAVAILABLE"),
        (("unknown", "owner@example.com"), 503, "ACCOUNT_UNAVAILABLE"),
    ],
)
def test_protected_operations_follow_the_entry_account_decision(
    client, accounts, state, status, code
):
    accounts["session"] = state
    response = client.get("/api/v1/services", headers={"X-Account-Session": "session"})
    assert response.status_code == status
    if code:
        assert response.json()["code"] == code


def test_owner_is_the_entry_account_and_locked_sessions_keep_it(client, accounts):
    accounts["session"] = ("unlocked", "owner@example.com")
    client.headers["X-Account-Session"] = "session"
    assert client.get("/api/v1/research/workspace").status_code == 200
    mismatch = client.get("/api/v1/research/workspace?expected_account=other@example.com")
    assert mismatch.status_code == 409
    assert client.get("/api/v1/services").status_code == 200
    accounts["session"] = ("locked", "owner@example.com")
    assert client.get("/api/v1/research/workspace").status_code == 423


def test_development_mode_needs_no_account(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/open.db")
    app = create_app(Settings(token=TOKEN, data_root=tmp_path), raw_engine(engine))
    with TestClient(app, headers={"Authorization": f"Bearer {TOKEN}"}) as client:
        assert client.get("/api/v1/services").status_code == 200
    engine.dispose()
