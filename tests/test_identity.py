import time

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select
from storage_support import identity_store, raw_engine

from asterion.api.app import create_app
from asterion.identity.service import (
    Identity,
    IdentityError,
    Mailer,
    accounts,
    challenges,
    sessions,
)
from asterion.platform.config import Settings
from asterion.platform.secrets import digest_port


class CaptureMail:
    def __init__(self):
        self.messages = []
        self.fail = False

    def send(self, email, code, purpose):
        if self.fail:
            raise IdentityError("邮件发送失败", 502, "MAIL_FAILED")
        self.messages.append((email, code, purpose))


@pytest.fixture
def identity(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/identity.db")
    mail = CaptureMail()
    return Identity(identity_store(engine), tmp_path, digest_port("test-secret" * 4), mail)


def register(identity):
    identity.register("person@example.com", "correct-password-123", "Test", "User", pin="246810")
    return identity.mailer.messages[-1][1]


def test_verification_login_logout_and_reset(identity):
    code = register(identity)
    with pytest.raises(IdentityError) as error:
        identity.login("person@example.com", "correct-password-123")
    assert error.value.code == "EMAIL_UNVERIFIED"
    identity.verify("person@example.com", code)
    with pytest.raises(IdentityError):
        identity.verify("person@example.com", code)
    result = identity.login("PERSON@example.com", "correct-password-123")
    assert identity.me(result["session"])["email"] == "person@example.com"
    identity.resend("person@example.com", "reset")
    reset_code = identity.mailer.messages[-1][1]
    identity.verify("person@example.com", reset_code, "replacement-password-456")
    with pytest.raises(IdentityError):
        identity.me(result["session"])
    with pytest.raises(IdentityError):
        identity.login("person@example.com", "correct-password-123")
    result = identity.login("person@example.com", "replacement-password-456")
    identity.logout(result["session"])
    with pytest.raises(IdentityError):
        identity.me(result["session"])
    with identity.engine.connect() as conn:
        assert conn.execute(select(accounts.c.password)).scalar() != "replacement-password-456"
        assert conn.execute(select(sessions)).first() is None


def test_code_expiry_attempt_budget_and_resend(identity):
    code = register(identity)
    with pytest.raises(IdentityError) as error:
        identity.resend("person@example.com", "verify")
    assert error.value.status == 429
    wrong = "000000" if code != "000000" else "999999"
    for _ in range(5):
        with pytest.raises(IdentityError):
            identity.verify("person@example.com", wrong)
    with pytest.raises(IdentityError):
        identity.verify("person@example.com", code)
    with identity.engine.begin() as conn:
        conn.execute(challenges.update().values(sent_at=time.time() - 61))
    identity.resend("person@example.com", "verify")
    new_code = identity.mailer.messages[-1][1]
    with identity.engine.begin() as conn:
        conn.execute(challenges.update().values(expires=time.time() - 1))
    with pytest.raises(IdentityError):
        identity.verify("person@example.com", new_code)


def test_delivery_failure_rolls_back_and_failed_login_is_limited(identity):
    identity.mailer.fail = True
    with pytest.raises(IdentityError):
        register(identity)
    with identity.engine.connect() as conn:
        assert conn.execute(select(accounts)).first() is None
    identity.mailer.fail = False
    code = register(identity)
    identity.verify("person@example.com", code)
    for _ in range(5):
        with pytest.raises(IdentityError):
            identity.login("person@example.com", "wrong-password-000")
    with pytest.raises(IdentityError) as error:
        identity.login("person@example.com", "correct-password-123")
    assert error.value.status == 429


def test_api_contract_and_mail_configuration(tmp_path, identity_instances):
    engine = create_engine(f"sqlite:///{tmp_path}/api.db")
    app = create_app(
        Settings(
            token="secret-test-token" * 3,
            data_root=tmp_path,
            require_account=True,
            account_verification="email",
        ),
        raw_engine(engine),
    )
    client = TestClient(app, headers={"Authorization": "Bearer " + "secret-test-token" * 3})
    assert client.get("/api/v1/snapshots").status_code == 401
    assert client.get("/api/v1/jobs").status_code == 401
    assert client.get("/api/v1/account/capabilities").json()["verification"] == "email"
    assert client.get("/api/v1/account/mail").status_code == 404
    registration = {
        "email": "person@example.com",
        "password": "correct-password-123",
        "pin": "246810",
        "first_name": "Test",
        "last_name": "User",
    }
    assert client.post("/api/v1/account/register", json=registration).status_code == 503
    config = {
        "host": "smtp.example.com",
        "port": 465,
        "security": "tls",
        "sender": "sender@example.com",
        "username": "sender",
        "password": "mail-secret",
    }
    identity_instances[-1].mailer.save(config)
    assert "mail-secret" not in str(identity_instances[-1].mailer.public_config())
    assert (tmp_path / "identity-mail.json").stat().st_mode & 0o777 == 0o600
    config["password"] = ""
    identity_instances[-1].mailer.save(config)
    assert identity_instances[-1].mailer.config()["password"] == "mail-secret"
    mail = CaptureMail()
    identity_instances[-1].mailer = mail
    assert client.post("/api/v1/account/register", json=registration).status_code == 200
    assert (
        "session"
        not in client.post(
            "/api/v1/account/verify",
            json={"email": registration["email"], "code": mail.messages[-1][1]},
        ).json()
    )
    session = client.post(
        "/api/v1/account/login",
        json={"email": registration["email"], "password": registration["password"]},
    ).json()["session"]
    assert (
        client.get("/api/v1/account/me", headers={"X-Account-Session": session}).status_code == 200
    )
    assert client.get("/api/v1/account/me").status_code == 401
    assert (
        client.get(
            "/api/v1/account/capabilities", headers={"Authorization": "Bearer wrong"}
        ).status_code
        == 401
    )


def test_smtp_uses_tls_and_propagates_failure(tmp_path, monkeypatch):
    calls = []

    class SMTP:
        def __init__(self, host, port, **kwargs):
            assert kwargs["context"].check_hostname
            calls.append((host, port))

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False

        def login(self, user, password):
            calls.append((user, password))

        def send_message(self, message):
            assert "123456" in message.get_content()
            assert message["To"] == "person@example.com"
            return {}

    monkeypatch.setattr("smtplib.SMTP_SSL", SMTP)
    mail = Mailer(tmp_path)
    mail.save(
        {
            "host": "smtp.example.com",
            "port": 465,
            "security": "tls",
            "sender": "sender@example.com",
            "username": "sender",
            "password": "secret",
        }
    )
    mail.send("person@example.com", "123456", "verify")
    assert calls == [("smtp.example.com", 465), ("sender", "secret")]


def test_default_local_account_accepts_any_code_without_mail(tmp_path, identity_instances):
    app = create_app(
        Settings(token="local-test-token" * 3, data_root=tmp_path),
        raw_engine(create_engine(f"sqlite:///{tmp_path}/local.db")),
    )
    mail = CaptureMail()
    mail.fail = True
    identity_instances[-1].mailer = mail
    client = TestClient(app, headers={"Authorization": "Bearer " + "local-test-token" * 3})
    assert client.get("/api/v1/account/capabilities").json()["verification"] == "local"
    user = {
        "email": "local@example.com",
        "password": "local-password-123",
        "pin": "246810",
        "first_name": "Local",
        "last_name": "User",
    }
    assert (
        client.post("/api/v1/account/register", json=user).json()["status"]
        == "local_verification_ready"
    )
    assert not mail.messages
    assert (
        client.post(
            "/api/v1/account/verify", json={"email": user["email"], "code": "123"}
        ).status_code
        == 422
    )
    assert (
        client.post(
            "/api/v1/account/verify", json={"email": user["email"], "code": "000000"}
        ).status_code
        == 200
    )
    assert (
        client.post(
            "/api/v1/account/verify", json={"email": user["email"], "code": "999999"}
        ).status_code
        == 400
    )
    assert (
        client.post(
            "/api/v1/account/login", json={"email": user["email"], "password": user["password"]}
        ).status_code
        == 200
    )
    assert client.post("/api/v1/account/forgot", json={"email": user["email"]}).status_code == 200
    assert (
        client.post(
            "/api/v1/account/reset",
            json={"email": user["email"], "code": "654321", "password": "replacement-password-123"},
        ).status_code
        == 200
    )
    assert not mail.messages
