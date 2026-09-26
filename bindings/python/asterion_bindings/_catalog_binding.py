"""Instrument API value conversion; all identity decisions execute in Rust."""

from datetime import date
from typing import TYPE_CHECKING, ClassVar

from ._call import invoke
from ._models import NativeModel

if TYPE_CHECKING:
    from .catalog import Contract, ContractResolution, ReferenceCatalog, ResolutionRequest

__all__ = [
    "CatalogModel",
    "CatalogOps",
    "ImportOps",
    "SourceOps",
    "catalog_digest",
    "validate_market_code",
]


class CatalogModel(NativeModel):
    _domain: ClassVar[str] = "catalog"


class CatalogOps(NativeModel):
    def resolve(self, request: "ResolutionRequest") -> "ContractResolution":
        from .catalog import ContractResolution

        return ContractResolution.model_validate(
            self._call("resolve", {"request": request.model_dump(mode="json")})
        )


class SourceOps(NativeModel):
    def resolve(self, day: date) -> "Contract":
        from .catalog import Contract

        return Contract.model_validate(self._call("resolve", {"day": day.isoformat()}))

    def validate_rows(self, rows: list[dict], exchange: str) -> list[str]:
        return self._call(
            "validate_rows",
            {
                "rows": _identity_rows(rows, ("contract", "trading_day", "symbol", "exchange")),
                "exchange": exchange,
            },
        )


class ImportOps(NativeModel):
    def resolve(self, contract: str, day: date) -> "Contract":
        from .catalog import Contract

        return Contract.model_validate(
            self._call("resolve", {"contract": contract, "day": day.isoformat()})
        )

    def validate_rows(self, rows: list[dict]):
        return self._call(
            "validate_rows", {"rows": _identity_rows(rows, ("contract", "trading_day"))}
        )


def _identity_rows(rows: list[dict], fields: tuple[str, ...]) -> list[dict]:
    # Arrow rows can also contain timestamps, decimals and payload columns. Only
    # identity fields belong to this API; preserve date values without coercing
    # arbitrary objects or truncating a datetime to a trading day.
    result = []
    for row in rows:
        value = {key: row[key] for key in fields if key in row}
        if isinstance(value.get("trading_day"), date):
            value["trading_day"] = value["trading_day"].isoformat()
        result.append(value)
    return result


def catalog_digest(catalog: "ReferenceCatalog") -> str:
    return catalog._call("id", {})


def validate_market_code(contract: str, result: "Contract"):
    invoke(
        "catalog",
        "validate_market_code",
        {"contract": contract, "actual": result.model_dump(mode="json")},
    )
