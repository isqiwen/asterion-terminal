"""Generated scan DTO validation through the sole Rust domain implementation."""

from typing import ClassVar

from ._models import NativeModel

__all__ = ["DataStoreModel"]


class DataStoreModel(NativeModel):
    _domain: ClassVar[str] = "data_store"
