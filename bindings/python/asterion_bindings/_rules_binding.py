"""Value conversion for exact Rust rule calculations; no Python arithmetic."""

from datetime import date
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
from typing import TYPE_CHECKING, ClassVar

from ._call import invoke
from ._models import NativeModel

if TYPE_CHECKING:
    from .rules import RulePeriod, RuleSpec

__all__ = ["RulePeriodOps", "RulesModel", "RulesOps", "SettlementOps", "rule_id"]


def precision() -> int:
    context = getcontext()
    if context.rounding != ROUND_HALF_EVEN:
        raise ValueError("Rules require ROUND_HALF_EVEN decimal rounding")
    return context.prec


class RulesModel(NativeModel):
    _domain: ClassVar[str] = "rules"


class RulesOps(NativeModel):
    def at(self, day: date) -> "RulePeriod":
        from .rules import RulePeriod

        return RulePeriod.model_validate(self._call("at", {"day": day.isoformat()}))

    def cover(self, contract_id: str, start: date, end: date):
        self._call(
            "cover",
            {"contract_id": contract_id, "start": start.isoformat(), "end": end.isoformat()},
        )


class RulePeriodOps(NativeModel):
    def fee(self, price: Decimal, multiplier: Decimal, lots: int, opening: bool) -> Decimal:
        return Decimal(
            invoke(
                "rules",
                "fee",
                {
                    "period": self._native_value(),
                    "price": str(price),
                    "multiplier": str(multiplier),
                    "lots": lots,
                    "opening": opening,
                    "precision": precision(),
                },
            )
        )


class SettlementOps(NativeModel):
    def values(self):
        mode, fee, margin = invoke(
            "rules", "settlement_values", {"basis": self._native_value(), "precision": precision()}
        )
        return mode, Decimal(fee), Decimal(margin)

    def period(self, start: date, end: date) -> "RulePeriod":
        from .rules import RulePeriod

        return RulePeriod.model_validate(
            invoke(
                "rules",
                "settlement_period",
                {
                    "basis": self._native_value(),
                    "start": start.isoformat(),
                    "end": end.isoformat(),
                    "precision": 28,
                },
            )
        )


def rule_id(spec: "RuleSpec") -> str:
    return spec._call("id", {})
