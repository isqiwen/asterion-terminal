"""Value conversion for immutable native role registries and opening indexes."""

from datetime import datetime
from typing import TYPE_CHECKING, ClassVar

from ._call import invoke
from ._models import NativeModel

if TYPE_CHECKING:
    from .calendar import Span
    from .roles import (
        ComputedRoleResolution,
        NextOpening,
        RoleQuery,
        RoleResolution,
        RoleSpec,
        RoleVersion,
    )

__all__ = ["ComputedRoleOps", "OpeningOps", "RolesModel", "next_opening", "resolve", "role_id"]


class RolesModel(NativeModel):
    _domain: ClassVar[str] = "roles"


class OpeningOps(NativeModel):
    def next_opening(self, observation_end: datetime, available_at: datetime) -> "Span":
        from .calendar import Span

        return Span.model_validate(
            self._call(
                "next_opening",
                {
                    "observation_end": observation_end.isoformat(),
                    "available_at": available_at.isoformat(),
                },
            )
        )


class ComputedRoleOps(NativeModel):
    def resolve(self, query: "RoleQuery") -> "ComputedRoleResolution":
        from .roles import ComputedRoleResolution

        return ComputedRoleResolution.model_validate(
            self._call("resolve", {"query": query.model_dump(mode="json")})
        )


def role_id(spec: "RoleSpec") -> str:
    return spec._call("id", {})


def resolve(version: "RoleVersion", query: "RoleQuery") -> "RoleResolution":
    from .roles import RoleResolution

    return RoleResolution.model_validate(
        version.spec._call("resolve", {"query": query.model_dump(mode="json")})
    )


def next_opening(body: "NextOpening") -> "Span":
    from .calendar import Span

    return Span.model_validate(invoke("roles", "next_opening", {"request": body._native_value()}))
