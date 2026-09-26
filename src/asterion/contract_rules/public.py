"""Application-owned access to published immutable Rust rule versions."""

from collections.abc import Callable
from dataclasses import dataclass

from asterion_bindings.plugin_host import Capability
from asterion_bindings.rules import RuleVersion


@dataclass(frozen=True)
class RuleAccess:
    read: Callable[[str], RuleVersion]


RULE_ACCESS = Capability("contract_rules.read", "asterion.contract_rules", RuleAccess)
