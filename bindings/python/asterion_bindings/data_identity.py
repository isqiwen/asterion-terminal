"""Identity catalogs from fixed standard contracts versions, built by the Rust
data store from a version preview; refusals raise ValueError."""

import json

from . import _native


def _text(value) -> str:
    return json.dumps(value, default=str)


def check_input(item: dict, manifest: dict) -> None:
    _native.data_catalog_input_check(json.dumps(item), _text(manifest))


def source_catalog(preview: dict, version_id: str, symbols: list[str]) -> dict:
    return json.loads(_native.data_source_catalog(_text(preview), version_id, json.dumps(symbols)))


def product_catalog(preview: dict, version_id: str, product_id: str) -> dict:
    return json.loads(_native.data_product_catalog(_text(preview), version_id, product_id))


def source_contract(preview: dict, contract_id: str) -> tuple[dict, dict]:
    value = json.loads(_native.data_source_contract(_text(preview), contract_id))
    return value["contract"], value["row"]
