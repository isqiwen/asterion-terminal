"""Generated connection DTO checks through the authoritative Rust L2 models."""

from typing import ClassVar

from ._models import NativeModel

__all__ = ["ConnectionsModel"]


class ConnectionsModel(NativeModel):
    _domain: ClassVar[str] = "connections"
