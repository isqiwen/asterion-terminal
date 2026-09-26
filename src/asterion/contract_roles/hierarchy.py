"""Read-only navigation summaries owned by the role plugin."""

from typing import Literal

from pydantic import BaseModel


class RoleDirectory(BaseModel):
    product_id: str
    role: Literal["main", "secondary"]
    count: int
