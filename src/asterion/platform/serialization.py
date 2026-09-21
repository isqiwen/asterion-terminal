"""Canonical serialization for stable artifact and input fingerprints."""

import json


def canonical(value):
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()
