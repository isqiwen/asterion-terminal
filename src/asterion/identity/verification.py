"""Verification policy adapters; only server configuration chooses the policy."""

import hmac
import re
from typing import Protocol


class Delivery(Protocol):
    def send(self, email: str, code: str, purpose: str) -> None: ...


class Verification(Protocol):
    mode: str
    retry_after: int

    def deliver(self, mailer: Delivery, email: str, code: str, purpose: str) -> None: ...
    def accepts(self, code: str, actual: str, expected: str) -> bool: ...


class LocalVerification:
    mode = "local"
    retry_after = 0

    def deliver(self, mailer, email, code, purpose):
        # Local accounts have no external delivery or email-ownership claim.
        pass

    def accepts(self, code, actual, expected):
        return re.fullmatch(r"[0-9]{6}", code) is not None


class EmailVerification:
    mode = "email"
    retry_after = 60

    def deliver(self, mailer, email, code, purpose):
        mailer.send(email, code, purpose)

    def accepts(self, code, actual, expected):
        return hmac.compare_digest(actual, expected)


def verification_policy(mode: str) -> Verification:
    if mode == "local":
        return LocalVerification()
    if mode == "email":
        return EmailVerification()
    raise ValueError("Unknown account verification policy")
