"""Pydantic value conversion and immutable native-handle ownership."""

import json
from collections.abc import Mapping
from functools import cached_property
from typing import Any, ClassVar, Self

from pydantic import BaseModel, ConfigDict, model_validator

from . import _native
from ._call import invoke


class NativeModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    _domain: ClassVar[str]
    _native_model: ClassVar[str]
    _native_fields: ClassVar[frozenset[str]]
    _indexed: ClassVar[frozenset[str]] = frozenset(
        {
            "ReferenceCatalog",
            "SourceIdentity",
            "ImportIdentity",
            "TimeSpec",
            "RuleSpec",
            "RoleSpec",
            "RoleCalendar",
            "ComputedRoleIndex",
        }
    )

    @cached_property
    def _native_handle(self):
        return _native.DomainHandle(
            self._domain, self._native_model, json.dumps(self._native_value(), ensure_ascii=False)
        )

    def _native_value(self):
        return self.model_dump(mode="json", include=set(self._native_fields))

    @model_validator(mode="after")
    def _validate_native(self):
        if self._native_model in self._indexed:
            _ = self._native_handle
        else:
            invoke(
                self._domain,
                "validate",
                {"model": self._native_model, "value": self._native_value()},
            )
        return self

    def _call(self, operation: str, value: dict[str, Any]):
        return json.loads(
            self._native_handle.call(operation, json.dumps(value, ensure_ascii=False))
        )

    def model_copy(self, *, update: Mapping[str, Any] | None = None, deep: bool = False) -> Self:
        # A copied DTO must never retain a native index for different values.
        return type(self).model_validate(self.model_dump() | dict(update or {}))
