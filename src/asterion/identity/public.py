"""Account checks shared with declared dependent plugins."""

from collections.abc import Callable
from dataclasses import dataclass

from asterion_bindings.plugin_host import Capability
from asterion_bindings.resource import Resource


@dataclass(frozen=True)
class Access:
    account: Callable
    owner: Callable


ACCOUNT_ACCESS = Capability("identity.access", "asterion.identity", Access)


@dataclass(frozen=True)
class AccountPolicy:
    require_account: bool


ACCOUNT_POLICY = Resource("identity.policy", AccountPolicy)
