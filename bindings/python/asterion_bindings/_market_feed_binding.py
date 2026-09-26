"""Generated quote DTO validation through the sole Rust domain implementation."""

from typing import ClassVar

from ._models import NativeModel

__all__ = ["MarketFeedModel"]


class MarketFeedModel(NativeModel):
    _domain: ClassVar[str] = "market_feed"
