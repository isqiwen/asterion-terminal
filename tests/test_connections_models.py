"""The generated Python values and the Rust normalizer share one current schema."""

import json

import pytest
from asterion_bindings import _native
from asterion_bindings._call import invoke
from asterion_bindings.connections import (
    ChannelState,
    ConfigField,
    ConnectionProfile,
    ConnectorDescriptor,
    ReadBatch,
    ReadRequest,
    Subscription,
)


def test_generated_defaults_and_mapping_values_match_rust():
    field = ConfigField(key="account", label="账户")
    assert field.model_dump() == invoke(
        "connections",
        "validate",
        {
            "model": "ConfigField",
            "value": {"key": "account", "label": "账户"},
        },
    )
    assert ChannelState().state == "disconnected"
    descriptor = ConnectorDescriptor(
        id="test",
        owner="test.source",
        title="Source",
        capabilities=("market_quotes",),
        fields=(field,),
    )
    assert descriptor.version == 1 and descriptor.instructions == ""
    assert isinstance(descriptor.fields, tuple)
    profile = ConnectionProfile(
        connection_id="a" * 32,
        connector_id="test",
        name="账户",
        config_revision=1,
        config={"account": "测试"},
    )
    assert json.loads(profile.model_dump_json())["config"] == {"account": "测试"}
    with pytest.raises(ValueError):
        profile.model_copy(update={"config_revision": 0})
    with pytest.raises(ValueError):
        ConnectionProfile.model_validate(profile.model_dump() | {"config": {"account": None}})
    with pytest.raises(ValueError):
        descriptor.model_copy(update={"version": 2})


def test_generated_values_reject_malformed_current_inputs():
    for values in [
        {"exchange": "SHFE", "symbol": "rb0000"},
        {"exchange": "SHFE", "symbol": "rb2609", "unknown": True},
    ]:
        with pytest.raises(ValueError):
            Subscription.model_validate(values)
    request = ReadRequest(connection_id="a" * 32, generation=1, request_id="b" * 32, started_at=1)
    batch = ReadBatch(**request.model_dump(), observed_at=2, complete=True)
    for update in (
        {"complete": False},
        {"observed_at": 0},
        {"observed_at": float("inf")},
        {"request_id": "unissued"},
    ):
        with pytest.raises(ValueError):
            ReadBatch.model_validate(batch.model_dump() | update)
    for key in batch.model_dump():
        value = batch.model_dump()
        value.pop(key)
        with pytest.raises(ValueError):
            ReadBatch.model_validate(value)


def test_native_value_boundary_rejects_duplicate_fields_and_oversized_input():
    with pytest.raises(ValueError):
        _native.invoke(
            "connections", "validate", '{"model":"ChannelState","model":"ReadBatch","value":{}}'
        )
    with pytest.raises(ValueError):
        _native.invoke("connections", "validate", " " * (16 * 1024 * 1024 + 1))
