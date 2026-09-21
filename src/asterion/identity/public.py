"""Account checks shared with declared dependent plugins."""

from collections.abc import Callable
from dataclasses import dataclass

from asterion.platform.plugins import Capability
from asterion.platform.resource import Resource
from asterion.platform.secrets import DigestPort


@dataclass(frozen=True)
class Access:
    account: Callable
    owner: Callable


ACCOUNT_ACCESS = Capability("identity.access", "asterion.identity", Access)


@dataclass(frozen=True)
class AccountPolicy:
    require_account: bool
    verification: str


ACCOUNT_POLICY = Resource("identity.policy", AccountPolicy)


IDENTITY_DIGEST = Resource("identity.digest", DigestPort)
