"""Application read capability for published provider role versions."""

from collections.abc import Callable
from dataclasses import dataclass

from asterion_bindings.plugin_host import Capability
from asterion_bindings.roles import RoleVersion


@dataclass(frozen=True)
class RoleAccess:
    read: Callable[[str], RoleVersion]


ROLE_ACCESS = Capability("contract_roles.read", "asterion.contract_roles", RoleAccess)
