import time

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select

from asterion.api.app import create_app
from asterion.identity.pin import security
from asterion.identity.service import Identity, IdentityError
from asterion.platform.config import Settings


@pytest.fixture
def identity(tmp_path):
    return Identity(
        create_engine(f"sqlite:///{tmp_path}/pin.db"), tmp_path, "test-secret" * 4, mode="local"
    )


def signed_in(identity, pin="246810"):
    identity.register("pin@example.com", "test-password-123", "Pin", "Test", pin=pin)
    identity.verify("pin@example.com", "000000")
    return identity.login("pin@example.com", "test-password-123")["session"]


def test_pin_is_hashed_and_not_the_local_email_code(identity):
    token = signed_in(identity)
    with identity.engine.connect() as conn:
        encoded = conn.execute(select(security.c.pin_hash)).scalar()
        assert encoded and "246810" not in encoded
    state = identity.pin.state(token, "lock")
    with pytest.raises(IdentityError) as wrong:
        identity.pin.state(token, "unlock", pin="000000", expected=state["revision"])
    assert wrong.value.code == "INVALID_PIN"
    assert identity.pin.state(token)["locked"]
    assert not identity.pin.state(token, "unlock", pin="246810", expected=state["revision"])[
        "locked"
    ]


def test_idle_and_activity_share_one_server_lock(identity, monkeypatch):
    token = signed_in(identity)
    second = identity.login("pin@example.com", "test-password-123")["session"]
    clock = [time.time()]
    monkeypatch.setattr("asterion.identity.pin.time.time", lambda: clock[0])
    clock[0] += 290
    assert not identity.pin.state(second, "activity")["locked"]
    clock[0] += 290
    assert not identity.pin.state(token)["locked"]
    clock[0] += 11
    assert identity.pin.state(token, "activity")["locked"]
    assert identity.pin.state(second)["locked"]
    with pytest.raises(IdentityError) as locked:
        identity.pin.require_unlocked(second)
    assert locked.value.status == 423


def test_legacy_setup_cannot_overwrite_existing_pin(identity):
    token = signed_in(identity, pin=None)
    assert identity.pin.state(token)["pin_required"]
    with pytest.raises(IdentityError):
        identity.pin.require_unlocked(token)
    assert not identity.pin.state(token, "setup", pin="246810")["pin_required"]
    with pytest.raises(IdentityError):
        identity.pin.state(token, "setup", pin="111111")
    with pytest.raises(IdentityError):
        identity.pin.state(token, "change", pin="111111", password="wrong-password")
    identity.pin.state(token, "change", pin="111111", password="test-password-123")
    state = identity.pin.state(token, "lock")
    assert not identity.pin.state(token, "unlock", pin="111111", expected=state["revision"])[
        "locked"
    ]


def test_bruteforce_and_stale_unlock(identity):
    token = signed_in(identity)
    state = identity.pin.state(token, "lock")
    for _ in range(5):
        with pytest.raises(IdentityError):
            identity.pin.state(token, "unlock", pin="000000", expected=state["revision"])
    with pytest.raises(IdentityError) as blocked:
        identity.pin.state(token, "unlock", pin="246810", expected=state["revision"])
    assert blocked.value.status == 429
    identity.login("pin@example.com", "test-password-123")
    new = identity.pin.state(token, "lock")
    assert new["revision"] > state["revision"]


def test_api_registration_requires_pin_and_locked_data_is_denied(tmp_path):
    app = create_app(
        Settings(token="test-secret-1234567890123456", data_root=tmp_path, require_account=True),
        create_engine(f"sqlite:///{tmp_path}/api.db"),
    )
    client = TestClient(app, headers={"Authorization": "Bearer test-secret-1234567890123456"})
    registration = {
        "email": "api@example.com",
        "password": "test-password-123",
        "first_name": "Api",
        "last_name": "Test",
    }
    assert client.post("/api/v1/account/register", json=registration).status_code == 422
    registration["pin"] = "246810"
    assert client.post("/api/v1/account/register", json=registration).status_code == 200
    client.post(
        "/api/v1/account/verify", json={"email": registration["email"], "code": "000000"}
    ).raise_for_status()
    login = client.post(
        "/api/v1/account/login",
        json={"email": registration["email"], "password": registration["password"]},
    ).json()
    client.headers["X-Account-Session"] = login["session"]
    assert client.get("/api/v1/reference/releases").status_code == 200
    state = client.post("/api/v1/account/security/lock").json()
    assert client.get("/api/v1/reference/releases").status_code == 423
    assert (
        client.post(
            "/api/v1/account/security/unlock",
            json={"pin": "246810", "expected": state["revision"] - 1},
        ).status_code
        == 409
    )
    assert (
        client.post(
            "/api/v1/account/security/unlock", json={"pin": "246810", "expected": state["revision"]}
        ).status_code
        == 200
    )
    assert client.get("/api/v1/reference/releases").status_code == 200
    assert (
        client.post("/api/v1/account/security/timeout", json={"seconds": 60}).json()[
            "timeout_seconds"
        ]
        == 60
    )
