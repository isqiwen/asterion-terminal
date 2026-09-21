"""Server-enforced request scopes bound to an account session and expiration."""

import base64
import hashlib
import hmac
import json
import re
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class Grant:
    path: str
    methods: tuple[str, ...]
    descendants: bool = False

    def allows(self, method, path):
        if (
            method not in self.methods
            or not re.fullmatch(r"/[a-zA-Z0-9_./-]+", path)
            or any(part in {"", ".", ".."} for part in path.split("/")[1:])
        ):
            return False
        expected, actual = self.path.split("/"), path.split("/")
        if len(actual) < len(expected) or (not self.descendants and len(actual) != len(expected)):
            return False
        return all(part == ":id" or part == actual[index] for index, part in enumerate(expected))


def worker_token(secret):
    return hmac.new(secret.encode(), b"asterion.worker.transport.v1", hashlib.sha256).hexdigest()


class Authority:
    def __init__(self, secret, policies, worker_grants, *, clock=time.time):
        self._key = hmac.digest(secret.encode(), b"asterion.request.scopes.v1", "sha256")
        self._root = secret
        self._worker = worker_token(secret)
        self._policies = {key: tuple(value) for key, value in policies.items()}
        self._worker_grants = tuple(worker_grants)
        self._clock = clock

    @staticmethod
    def subject(session):
        return hashlib.sha256(session.encode()).hexdigest()

    def issue(self, scope, session):
        if scope not in self._policies:
            raise ValueError("未登记该功能的请求授权")
        expires = int(self._clock()) + 300
        payload = json.dumps(
            {"scope": scope, "subject": self.subject(session), "expires": expires},
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        body = base64.urlsafe_b64encode(payload).decode().rstrip("=")
        signature = hmac.new(self._key, body.encode(), hashlib.sha256).hexdigest()
        return {"token": f"scope.{body}.{signature}", "expires": expires}

    def authorize(self, credential, method, path, session):
        if hmac.compare_digest(credential.encode(), self._root.encode()):
            return "root"
        if hmac.compare_digest(credential.encode(), self._worker.encode()):
            if any(grant.allows(method, path) for grant in self._worker_grants):
                return "worker"
            raise ValueError("任务执行器无权访问此接口")
        try:
            prefix, body, signature = credential.split(".")
            expected = hmac.new(self._key, body.encode(), hashlib.sha256).hexdigest()
            if prefix != "scope" or not hmac.compare_digest(signature, expected):
                raise ValueError()
            value = json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
            if (
                set(value) != {"scope", "subject", "expires"}
                or value["expires"] <= self._clock()
                or value["subject"] != self.subject(session)
            ):
                raise ValueError()
            grants = self._policies[value["scope"]]
            if not any(grant.allows(method, path) for grant in grants):
                raise ValueError()
            return value["scope"]
        except (ValueError, KeyError, TypeError):
            raise ValueError("请求授权无效、过期或超出当前功能范围") from None
