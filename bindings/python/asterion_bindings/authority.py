"""Python values for the fixed Rust request-scope authority."""

import time
from dataclasses import asdict, dataclass

from ._call import invoke


@dataclass(frozen=True)
class Grant:
    path: str
    methods: tuple[str, ...]
    descendants: bool = False

    def allows(self, method, path):
        return invoke(
            "kernel",
            "auth.grant_allows",
            {
                "grant": asdict(self),
                "method": method,
                "path": path,
            },
        )


def worker_token(secret):
    return invoke("kernel", "auth.worker_token", {"secret": secret})


def forwarding_token(secret):
    return invoke("kernel", "auth.forwarding_token", {"secret": secret})


def forwarded(credential, secret):
    """Whether a request carries the Rust entry's forwarding credential."""
    return invoke("kernel", "auth.forwarded", {"credential": credential, "secret": secret})


class Authority:
    def __init__(self, secret, policies, worker_grants, *, clock=time.time):
        self._secret = secret
        self._policies = {
            key: [asdict(grant) for grant in grants] for key, grants in policies.items()
        }
        self._worker_grants = [asdict(grant) for grant in worker_grants]
        self._clock = clock

    @staticmethod
    def subject(session):
        return invoke("kernel", "auth.subject", {"session": session})

    def issue(self, scope, session):
        return invoke(
            "kernel",
            "auth.issue",
            {
                "secret": self._secret,
                "policies": self._policies,
                "scope": scope,
                "session": session,
                "now": self._clock(),
            },
        )

    def authorize(self, credential, method, path, session):
        return invoke(
            "kernel",
            "auth.authorize",
            {
                "secret": self._secret,
                "policies": self._policies,
                "worker_grants": self._worker_grants,
                "credential": credential,
                "method": method,
                "path": path,
                "session": session,
                "now": self._clock(),
            },
        )
