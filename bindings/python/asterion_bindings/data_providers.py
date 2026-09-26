"""The built-in Rust data sources: manifests, admitted plans, transport and mapping.

Refusals raise ValueError with the user-facing reason; requests and partitions
cross as their JSON text.
"""

import json

from . import _native


def manifests() -> list[dict]:
    return json.loads(_native.data_provider_manifests())


def plan(provider: str, request: str) -> list[dict]:
    return json.loads(_native.data_provider_plan(provider, request))


def check_plan(request: str, parts: list[dict]) -> None:
    _native.data_provider_check_plan(request, json.dumps(parts))


def probe(provider: str, configuration: dict) -> str:
    return _native.data_provider_probe(provider, json.dumps(configuration))


def fetch(provider: str, partition: str, configuration: dict) -> list[dict]:
    return json.loads(_native.data_provider_fetch(provider, partition, json.dumps(configuration)))


def normalize(provider: str, request: str, rows: list[dict]) -> list[dict]:
    return json.loads(_native.data_provider_normalize(provider, request, json.dumps(rows)))
