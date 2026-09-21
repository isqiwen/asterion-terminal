"""Read-only source name projection, independent of account connectivity."""

from collections.abc import Callable
from dataclasses import dataclass

from asterion.platform.plugins import Capability


@dataclass(frozen=True)
class ContractNames:
    read: Callable[[str], dict[str, str]]


CONTRACT_NAMES = Capability("market.contract_names", "asterion.market", ContractNames)
