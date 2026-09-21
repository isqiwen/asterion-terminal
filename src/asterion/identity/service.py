import hashlib
import hmac
import json
import os
import re
import secrets
import smtplib
import ssl
import time
import uuid
from email.message import EmailMessage
from pathlib import Path

from sqlalchemy import Boolean, Column, Float, Integer, MetaData, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.platform.secrets import DigestPort

from .verification import verification_policy

metadata = MetaData()
accounts = Table(
    "identity_accounts",
    metadata,
    Column("id", String, primary_key=True),
    Column("email", String, unique=True, nullable=False),
    Column("first_name", String, nullable=False),
    Column("last_name", String, nullable=False),
    Column("country_code", String, nullable=False),
    Column("phone", String, nullable=False),
    Column("password", String, nullable=False),
    Column("verified", Boolean, nullable=False),
    Column("failures", Integer, nullable=False),
    Column("blocked_until", Float, nullable=False),
)
challenges = Table(
    "identity_challenges",
    metadata,
    Column("account_id", String, primary_key=True),
    Column("purpose", String, primary_key=True),
    Column("digest", String, nullable=False),
    Column("expires", Float, nullable=False),
    Column("sent_at", Float, nullable=False),
    Column("attempts", Integer, nullable=False),
)
sessions = Table(
    "identity_sessions",
    metadata,
    Column("digest", String, primary_key=True),
    Column("account_id", String, nullable=False),
    Column("expires", Float, nullable=False),
)


class IdentityError(Exception):
    def __init__(self, message, status=400, code="ACCOUNT_ERROR"):
        super().__init__(message)
        self.status, self.code = status, code


def email_address(value):
    value = value.strip().lower()
    if len(value) > 254 or not re.fullmatch(
        r"[a-z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z]{2,63}", value
    ):
        raise IdentityError("请输入有效的邮箱地址")
    return value


def password_hash(password, salt=None):
    if not 12 <= len(password) <= 128:
        raise IdentityError("密码长度必须为 12–128 个字符")
    salt = salt or secrets.token_hex(16)
    digest = hashlib.scrypt(
        password.encode(), salt=bytes.fromhex(salt), n=131072, r=8, p=1, maxmem=268435456
    ).hex()
    return salt + ":" + digest


def password_matches(password, encoded):
    if not 12 <= len(password) <= 128:
        return False
    return hmac.compare_digest(password_hash(password, encoded.split(":")[0]), encoded)


class Mailer:
    def __init__(self, root: Path):
        self.path = root / "identity-mail.json"

    def config(self):
        return json.loads(self.path.read_text()) if self.path.exists() else {}

    def public_config(self):
        config = self.config()
        return {k: v for k, v in config.items() if k != "password"} | {
            "configured": bool(config),
            "has_password": bool(config.get("password")),
        }

    def save(self, config):
        if not config.get("password"):
            config["password"] = self.config().get("password", "")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(f".identity-mail-{uuid.uuid4().hex}.tmp")
        try:
            with os.fdopen(
                os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w"
            ) as out:
                json.dump(config, out)
                out.flush()
                os.fsync(out.fileno())
            temporary.replace(self.path)
        finally:
            temporary.unlink(missing_ok=True)

    def send(self, email, code, purpose):
        config = self.config()
        if not config:
            raise IdentityError("请先在设置 → 邮件服务中配置发信服务器", 503, "MAIL_NOT_CONFIGURED")
        message = EmailMessage()
        message["From"] = config["sender"]
        message["To"] = email
        message["Subject"] = "Asterion Terminal · " + (
            "邮箱验证码" if purpose == "verify" else "重置密码验证码"
        )
        message.set_content(
            f"你的 Asterion Terminal 验证码是：{code}\n\n10 分钟内有效，只能使用一次。若非本人操作，请忽略此邮件。"
        )
        try:
            context = ssl.create_default_context()
            if config["security"] == "tls":
                client = smtplib.SMTP_SSL(
                    config["host"], config["port"], timeout=10, context=context
                )
            else:
                client = smtplib.SMTP(config["host"], config["port"], timeout=10)
            with client:
                if config["security"] == "starttls":
                    client.starttls(context=context)
                if config.get("username"):
                    client.login(config["username"], config["password"])
                if client.send_message(message):
                    raise IdentityError("邮件服务器拒绝了收件地址，请检查邮箱", 502, "MAIL_FAILED")
        except (OSError, smtplib.SMTPException):
            raise IdentityError(
                "邮件发送失败，请检查服务器配置后重试", 502, "MAIL_FAILED"
            ) from None


class Identity:
    def __init__(self, engine, root, signer: DigestPort, mailer=None, mode="email"):
        self.verification = verification_policy(mode)
        self.engine = engine
        self.mailer = mailer or Mailer(root)
        self._signer = signer
        engine.initialize(*metadata.tables.values())
        self.dummy_password = password_hash(secrets.token_urlsafe(24))
        from .pin import PinSecurity

        self.pin = PinSecurity(self)

    def digest(self, value):
        return self._signer.digest(value)

    def user(self, conn, email):
        return (
            conn.execute(select(accounts).where(accounts.c.email == email).with_for_update())
            .mappings()
            .first()
        )

    def issue(self, conn, user, purpose):
        now = time.time()
        condition = (challenges.c.account_id == user["id"]) & (challenges.c.purpose == purpose)
        old = conn.execute(select(challenges).where(condition)).mappings().first()
        if old and now - old["sent_at"] < self.verification.retry_after:
            raise IdentityError("请等待 60 秒后再申请验证码", 429, "RATE_LIMITED")
        code = f"{secrets.randbelow(1000000):06d}"
        self.verification.deliver(self.mailer, user["email"], code, purpose)
        conn.execute(challenges.delete().where(condition))
        conn.execute(
            challenges.insert().values(
                account_id=user["id"],
                purpose=purpose,
                digest=self.digest(user["id"] + purpose + code),
                expires=now + 600,
                sent_at=now,
                attempts=0,
            )
        )

    def register(self, email, password, first_name, last_name, country_code="", phone="", *, pin):
        email = email_address(email)
        encoded = password_hash(password)
        try:
            with self.engine.begin() as conn:
                if self.user(conn, email):
                    raise IdentityError(
                        "该邮箱已注册，请登录或重新发送验证码", 409, "ACCOUNT_EXISTS"
                    )
                user = {
                    "id": str(uuid.uuid4()),
                    "email": email,
                    "password": encoded,
                    "country_code": country_code,
                    "phone": phone,
                    "first_name": first_name,
                    "last_name": last_name,
                    "verified": False,
                    "failures": 0,
                    "blocked_until": 0,
                }
                conn.execute(accounts.insert().values(**user))
                self.pin.create(conn, user["id"], pin)
                self.issue(conn, user, "verify")
        except IntegrityError:
            raise IdentityError("该邮箱已注册，请登录", 409, "ACCOUNT_EXISTS") from None
        return {
            "status": "local_verification_ready"
            if self.verification.mode == "local"
            else "verification_sent",
            "email": email,
            "retry_after": self.verification.retry_after,
        }

    def resend(self, email, purpose):
        email = email_address(email)
        with self.engine.begin() as conn:
            user = self.user(conn, email)
            if user and (purpose == "reset" or not user["verified"]):
                self.issue(conn, user, purpose)
        return {
            "status": "local_verification_ready"
            if self.verification.mode == "local"
            else "if_eligible_sent",
            "retry_after": self.verification.retry_after,
        }

    def consume(self, conn, user, code, purpose):
        if not user:
            return False
        condition = (challenges.c.account_id == user["id"]) & (challenges.c.purpose == purpose)
        challenge = conn.execute(select(challenges).where(condition)).mappings().first()
        if not challenge or challenge["expires"] < time.time() or challenge["attempts"] >= 5:
            return False
        conn.execute(
            challenges.update().where(condition).values(attempts=challenge["attempts"] + 1)
        )
        if not self.verification.accepts(
            code, self.digest(user["id"] + purpose + code), challenge["digest"]
        ):
            return False
        conn.execute(challenges.delete().where(condition))
        return True

    def verify(self, email, code, password=None):
        email = email_address(email)
        encoded = password_hash(password) if password is not None else None
        with self.engine.begin() as conn:
            user = self.user(conn, email)
            valid = self.consume(conn, user, code, "reset" if encoded else "verify")
            if valid:
                values: dict[str, str | bool | int] = {"verified": True}
                if encoded:
                    values.update(password=encoded, failures=0, blocked_until=0)
                    conn.execute(sessions.delete().where(sessions.c.account_id == user["id"]))
                conn.execute(accounts.update().where(accounts.c.id == user["id"]).values(**values))
        if not valid:
            raise IdentityError("验证码无效、已过期或尝试次数过多", 400, "INVALID_CODE")
        return {"status": "password_reset" if encoded else "verified"}

    def login(self, email, password):
        email = email_address(email)
        error = None
        token = secrets.token_urlsafe(32)
        now = time.time()
        with self.engine.begin() as conn:
            user = self.user(conn, email)
            if user and user["blocked_until"] > now:
                error = IdentityError(
                    "尝试次数过多，请 15 分钟后重试或找回密码", 429, "RATE_LIMITED"
                )
            elif not password_matches(password, user["password"] if user else self.dummy_password):
                if user:
                    failures = user["failures"] + 1
                    conn.execute(
                        accounts.update()
                        .where(accounts.c.id == user["id"])
                        .values(
                            failures=failures if failures < 5 else 0,
                            blocked_until=now + 900 if failures >= 5 else 0,
                        )
                    )
                error = IdentityError("邮箱或密码不正确", 401, "INVALID_CREDENTIALS")
            elif not user["verified"]:
                error = IdentityError("请先验证邮箱", 403, "EMAIL_UNVERIFIED")
            else:
                conn.execute(
                    accounts.update()
                    .where(accounts.c.id == user["id"])
                    .values(failures=0, blocked_until=0)
                )
                self.pin.login(conn, user["id"])
                conn.execute(sessions.delete().where(sessions.c.expires < now))
                conn.execute(
                    sessions.insert().values(
                        digest=self.digest(token), account_id=user["id"], expires=now + 43200
                    )
                )
        if error:
            raise error
        return {
            "session": token,
            "user": {k: user[k] for k in ("email", "first_name", "last_name")},
        }

    def me(self, token):
        with self.engine.connect() as conn:
            row = (
                conn.execute(
                    select(accounts.c.email, accounts.c.first_name, accounts.c.last_name)
                    .join(sessions, sessions.c.account_id == accounts.c.id)
                    .where(
                        sessions.c.digest == self.digest(token), sessions.c.expires > time.time()
                    )
                )
                .mappings()
                .first()
            )
        if not row:
            raise IdentityError("登录已过期，请重新登录", 401, "SESSION_EXPIRED")
        return dict(row)

    def logout(self, token):
        with self.engine.begin() as conn:
            conn.execute(sessions.delete().where(sessions.c.digest == self.digest(token)))
        return {"status": "signed_out"}
