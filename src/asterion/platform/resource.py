"""Typed resource identity shared by activation and task execution."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Resource[T]:
    id: str
    contract: type[T]
