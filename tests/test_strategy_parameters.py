import json

import pytest
from pydantic import TypeAdapter
from test_external_strategies import installed  # noqa: F401
from test_research import payload, services  # noqa: F401

from asterion.platform.serialization import canonical
from asterion.research.engine import BacktestRequest, calculate
from asterion.research.packages import ResearchPackages
from asterion.research.parameters import Parameter, validate_parameters
from asterion.research.workspace import DraftConfig


def fields():
    return tuple(
        TypeAdapter(Parameter).validate_python(item)
        for item in [
            {
                "key": "count",
                "label": "周期",
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "default": 2,
            },
            {
                "key": "ratio",
                "label": "阈值",
                "type": "decimal",
                "minimum": "0",
                "maximum": "1",
                "scale": 4,
                "default": "0.1000",
            },
            {"key": "active", "label": "开关", "type": "boolean", "default": False},
            {
                "key": "mode",
                "label": "模式",
                "type": "enum",
                "choices": [{"value": "a", "label": "模式甲"}, {"value": "b", "label": "模式乙"}],
                "default": "a",
            },
        ]
    )


def values():
    return {"count": 2, "ratio": "0.1000", "active": False, "mode": "a"}


@pytest.mark.parametrize(
    "key,value",
    [
        ("count", True),
        ("count", "2"),
        ("count", 2.5),
        ("count", 21),
        ("ratio", 0.1),
        ("ratio", "NaN"),
        ("ratio", "1e-2"),
        ("ratio", "0.10001"),
        ("ratio", "1.0001"),
        ("active", 0),
        ("active", "false"),
        ("active", None),
        ("mode", "unknown"),
        ("mode", 1),
    ],
)
def test_type_range_and_precision_are_not_coerced(key, value):
    with pytest.raises(ValueError):
        validate_parameters(fields(), values() | {key: value})


def test_values_survive_json_request_and_draft_without_conversion():
    parameters = values()
    assert validate_parameters(fields(), parameters) == parameters
    request = BacktestRequest.model_validate(
        payload()["request"] | {"command_id": "parameters", "parameters": parameters}
    )
    assert json.loads(request.model_dump_json())["parameters"] == parameters
    assert (
        json.loads(DraftConfig(parameters=parameters).model_dump_json())["parameters"] == parameters
    )
    for incomplete in ({}, parameters | {"extra": 1}):
        with pytest.raises(ValueError):
            validate_parameters(fields(), incomplete)


@pytest.mark.parametrize(
    "index,change",
    [
        (0, {"default": True}),
        (0, {"minimum": 3}),
        (0, {"maximum": 2**54}),
        (1, {"default": 0.1}),
        (1, {"scale": 13}),
        (1, {"minimum": "NaN"}),
        (1, {"default": "2"}),
        (2, {"default": "false"}),
        (2, {"minimum": 0}),
        (3, {"default": "missing"}),
        (3, {"choices": [{"value": "a", "label": "甲"}, {"value": "a", "label": "乙"}]}),
    ],
)
def test_invalid_declaration_rejected(index, change):
    with pytest.raises(ValueError):
        TypeAdapter(Parameter).validate_python(fields()[index].model_dump() | change)


def test_four_types_persist_publish_and_replay(request):
    from storage_support import scheduler

    fixture_value = request.getfixturevalue("installed")
    service, request, _ = request.getfixturevalue("services")
    _, record, catalog = fixture_value
    service.strategies = catalog
    identity = next(s.identity for s in catalog.list() if s.identity.id == record["manifest"]["id"])
    parameters = {"lookback": 1, "threshold": "0.1000", "enabled": False, "comparison": "inclusive"}
    request = request.model_copy(update={"strategy": identity, "parameters": parameters})
    run = service.submit(request)
    claimed = scheduler(service.engine).claim("typed-parameters")
    assert claimed["payload"]["request"]["parameters"] == parameters
    output = calculate(claimed["payload"], catalog)
    assert output["fills"] == []
    service.publish(claimed["id"], claimed["token"], canonical(output))
    bundles = ResearchPackages(service)
    exported = bundles.export(run["id"], True)
    assert exported["content"]["request"]["parameters"] == parameters
    report = bundles.receive("tester", exported)
    assert report["can_replay"]
    replay = bundles.replay("tester", report["id"], "typed-replay")
    claimed = scheduler(service.engine).claim("typed-replay")
    assert claimed["id"] == replay["id"]
    service.publish(
        claimed["id"], claimed["token"], canonical(calculate(claimed["payload"], catalog))
    )
    assert service.get(replay["id"])["output"] == output
