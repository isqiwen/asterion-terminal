"""Server-authoritative account-wide terminal lock, independent of email verification."""

import re
import time

from sqlalchemy import Boolean, Column, Float, Integer, MetaData, String, Table, select

from .service import IdentityError, accounts, password_hash, password_matches, sessions

security = Table(
    "identity_pin_security",
    MetaData(),
    Column("account_id", String, primary_key=True),
    Column("pin_hash", String, nullable=False),
    Column("timeout", Integer, nullable=False),
    Column("last_activity", Float, nullable=False),
    Column("locked", Boolean, nullable=False),
    Column("failures", Integer, nullable=False),
    Column("blocked_until", Float, nullable=False),
    Column("revision", Integer, nullable=False),
)


def encode_pin(pin):
    if not isinstance(pin, str) or not re.fullmatch(r"[0-9]{6}", pin):
        raise IdentityError("PIN 必须为 6 位数字", 422, "INVALID_PIN")
    return password_hash("asterion-pin:" + pin)


class PinSecurity:
    def __init__(self, identity):
        self.identity = identity
        self.engine = identity.engine
        self.engine.initialize(security)

    def create(self, conn, account_id, pin):
        conn.execute(
            security.insert().values(
                account_id=account_id,
                pin_hash=encode_pin(pin),
                timeout=300,
                last_activity=time.time(),
                locked=False,
                failures=0,
                blocked_until=0,
                revision=0,
            )
        )

    def login(self, conn, account_id):
        row = (
            conn.execute(
                select(security).where(security.c.account_id == account_id).with_for_update()
            )
            .mappings()
            .first()
        )
        if row is None or not row["pin_hash"]:
            raise IdentityError(
                "账号安全状态不受支持，缺少注册时设置的 PIN", 409, "UNSUPPORTED_ACCOUNT_SECURITY"
            )
        else:
            conn.execute(
                security.update()
                .where(security.c.account_id == account_id)
                .values(
                    locked=False,
                    last_activity=time.time(),
                    revision=row["revision"] + 1,
                )
            )

    def row(self, conn, token):
        user = (
            conn.execute(
                select(accounts)
                .join(sessions, sessions.c.account_id == accounts.c.id)
                .where(
                    sessions.c.digest == self.identity.digest(token),
                    sessions.c.expires > time.time(),
                )
                .with_for_update()
            )
            .mappings()
            .first()
        )
        if not user:
            raise IdentityError("登录已过期，请重新登录", 401, "SESSION_EXPIRED")
        row = (
            conn.execute(
                select(security).where(security.c.account_id == user["id"]).with_for_update()
            )
            .mappings()
            .first()
        )
        if row is None or not row["pin_hash"]:
            raise IdentityError(
                "账号安全状态不受支持，缺少注册时设置的 PIN", 409, "UNSUPPORTED_ACCOUNT_SECURITY"
            )
        return user, dict(row)

    def state(self, token, action="status", pin=None, password=None, timeout=None, expected=None):
        error = None
        now = time.time()
        with self.engine.begin() as conn:
            user, row = self.row(conn, token)
            if row["blocked_until"] and row["blocked_until"] <= now:
                row.update(failures=0, blocked_until=0)
            if now - row["last_activity"] >= row["timeout"] and not row["locked"]:
                row.update(locked=True, revision=row["revision"] + 1)
            if action == "lock":
                if not row["locked"]:
                    row.update(locked=True, revision=row["revision"] + 1)
            elif action == "activity" and not row["locked"]:
                row["last_activity"] = now
            elif action in ("unlock", "change"):
                if row["blocked_until"] > now:
                    error = IdentityError("PIN 尝试次数过多，请稍后重试", 429, "PIN_RATE_LIMITED")
                elif action == "unlock" and expected != row["revision"]:
                    error = IdentityError("锁屏状态已变化，请重新输入 PIN", 409, "LOCK_CHANGED")
                else:
                    valid = (
                        action == "change" and password_matches(password or "", user["password"])
                    ) or (
                        action == "unlock"
                        and isinstance(pin, str)
                        and re.fullmatch(r"[0-9]{6}", pin)
                        and row["pin_hash"]
                        and password_matches("asterion-pin:" + pin, row["pin_hash"])
                    )
                    if valid:
                        if action != "unlock":
                            row["pin_hash"] = encode_pin(pin)
                        row.update(
                            locked=False,
                            last_activity=now,
                            failures=0,
                            blocked_until=0,
                            revision=row["revision"] + 1,
                        )
                    else:
                        failures = row["failures"] + 1
                        row.update(
                            failures=failures, blocked_until=now + 300 if failures >= 5 else 0
                        )
                        error = IdentityError("PIN 或账号密码不正确", 401, "INVALID_PIN")
            elif action == "timeout":
                if row["locked"] or not row["pin_hash"]:
                    error = IdentityError("请先解锁终端", 423, "TERMINAL_LOCKED")
                elif timeout not in (60, 300, 600, 900, 1800):
                    error = IdentityError("无效的自动锁定时间", 422, "INVALID_TIMEOUT")
                else:
                    row.update(timeout=timeout, last_activity=now)
            conn.execute(
                security.update()
                .where(security.c.account_id == user["id"])
                .values(**{k: v for k, v in row.items() if k != "account_id"})
            )
        if error:
            raise error
        return {
            "locked": row["locked"],
            "timeout_seconds": row["timeout"],
            "remaining_seconds": max(0, row["timeout"] - (now - row["last_activity"])),
            "revision": row["revision"],
            "retry_after": max(0, int(row["blocked_until"] - now)),
        }

    def require_unlocked(self, token):
        state = self.state(token)
        if state["locked"]:
            raise IdentityError("终端已锁定，请输入 PIN", 423, "TERMINAL_LOCKED")
