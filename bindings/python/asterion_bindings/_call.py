"""JSON value conversion at the native boundary."""

import json
from typing import Any

from . import _native


def invoke(module: str, operation: str, value: dict[str, Any]) -> Any:
    return json.loads(_native.invoke(module, operation, json.dumps(value, ensure_ascii=False)))
