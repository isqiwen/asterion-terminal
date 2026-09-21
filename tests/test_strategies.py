"""Strategy extension, identity fencing and chronological execution contracts."""

from dataclasses import replace

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from test_research import payload, services  # noqa: F401

from asterion.api.app import create_app
from asterion.distribution import builtin_plugins, strategy_catalog
from asterion.platform.config import Settings
from asterion.platform.plugins import Activation, Capability, Plugin, PluginHost
from asterion.platform.serialization import canonical
from asterion.research.engine import BacktestRequest, calculate
from asterion.research.packages import ResearchPackages, digest
from asterion.research.parameters import IntegerParameter
from asterion.research.strategies import (
    STRATEGIES,
    ClosedBar,
    Strategy,
    StrategyCatalog,
    StrategyInfo,
    StrategyRef,
    catalog_plugin,
)


def fixture_strategy(output=True, warmup=2):
    observed = []

    class Session:
        def close(self):
            pass

        def on_close(self, bar):
            assert isinstance(bar, ClosedBar)
            assert set(bar.__dataclass_fields__) == {"trading_day", "close"}
            observed.append(bar)
            return output

    strategy = Strategy(
        StrategyInfo(
            identity=StrategyRef(id="fixture.independent", version="1", digest="c" * 64),
            name="Independent plugin",
            description="Only closed bars; no execution authority.",
            parameters=(
                IntegerParameter(key="warmup", label="Warmup", minimum=1, maximum=10, default=warmup),
            ),
        ),
        lambda _: None,
        lambda p: int(p["warmup"]),
        lambda _: Session(),
    )
    return strategy, observed


def test_independent_plugin_requires_only_contribution_and_assembly():
    strategy, observed = fixture_strategy()
    capability = Capability("fixture.signal", "fixture.strategy", Strategy)
    plugin = Plugin(
        "fixture.strategy",
        (),
        lambda _: Activation(exports={capability: strategy}),
        provides=(capability,),
    )
    host = PluginHost((plugin, catalog_plugin((capability,))))
    host.activate(FastAPI(), {})
    try:
        catalog = host.resolve(STRATEGIES)
        value = payload()
        value["request"].update(
            strategy=strategy.info.identity.model_dump(), parameters={"warmup": 2}
        )
        result = calculate(value, catalog)
        assert result["fills"][0]["day"] == "2024-01-03"
        assert len(observed) == len(value["bars"])
        assert result["summary"]["fees"] == "2"
        assert canonical(result) == canonical(calculate(value, catalog))
    finally:
        host.close()


@pytest.mark.parametrize(
    "parameters",
    [
        {},
        {"fast": 1},
        {"fast": 1, "slow": 2, "unknown": 0},
        {"fast": 2, "slow": 2},
        {"fast": 0, "slow": 2},
        {"fast": True, "slow": 2},
        {"fast": 1.5, "slow": 2},
    ],
)
def test_invalid_parameters_are_not_defaulted(parameters):
    catalog = strategy_catalog()
    value = payload()
    value["request"]["parameters"] = parameters
    with pytest.raises(ValueError):
        calculate(value, catalog)


@pytest.mark.parametrize(
    "field,value", [("id", "fixture.missing"), ("version", "unavailable"), ("digest", "f" * 64)]
)
def test_unavailable_identity_never_submits_or_replays(request, field, value):
    service, request, _ = request.getfixturevalue("services")
    job = service.submit(request)
    request.strategy = request.strategy.model_copy(update={field: value})
    with pytest.raises(ValueError, match="策略"):
        service.submit(request)
    service.strategies = StrategyCatalog(())
    with pytest.raises(ValueError, match="策略"):
        service.rerun(job["id"], "cannot-rerun")
    assert len(service.list()) == 1


def test_invalid_intention_cannot_change_execution_or_publish():
    strategy, _ = fixture_strategy(output=1000)
    catalog = StrategyCatalog((strategy,))
    value = payload()
    value["request"].update(strategy=strategy.info.identity.model_dump(), parameters={"warmup": 2})
    with pytest.raises(ValueError, match="意图"):
        calculate(value, catalog)
    with pytest.raises(ValueError, match="重复"):
        StrategyCatalog((strategy, strategy))
    with pytest.raises(ValueError, match="预热"):
        StrategyCatalog((replace(strategy, warmup=lambda _: 0),))


def test_second_strategy_worker_publication_and_offline_replay(request):
    from storage_support import scheduler

    service, request, _ = request.getfixturevalue("services")
    strategy = next(
        s for s in service.strategies.list() if s.identity.id == "builtin.momentum-long"
    )
    request = request.model_copy(
        update={"strategy": strategy.identity, "parameters": {"lookback": 1}}
    )
    run = service.submit(request)
    claimed = scheduler(service.engine).claim("momentum")
    output = canonical(calculate(claimed["payload"], service.strategies))
    service.publish(claimed["id"], claimed["token"], output)
    packages = ResearchPackages(service)
    exported = packages.export(run["id"], True)
    assert exported["content"]["request"]["strategy"] == strategy.identity.model_dump()
    assert packages.receive("tester", exported)["can_replay"]
    exported["content"]["request"]["strategy"]["digest"] = "0" * 64
    exported["checksum"] = digest(exported["content"])
    report = packages.receive("tester", exported)
    assert not report["can_replay"]
    assert report["status"] == "UNSUPPORTED"
    with pytest.raises(ValueError):
        packages.replay("tester", report["id"], "tampered")


def test_discovery_auth_and_missing_plugin_fail_closed(tmp_path):
    settings = Settings(token="strategy-discovery-token-minimum-length", data_root=tmp_path)
    from sqlalchemy import create_engine

    engine = create_engine(f"sqlite:///{tmp_path}/discovery.db")
    with TestClient(create_app(settings, engine)) as client:
        assert client.get("/api/v1/research/strategies").status_code == 401
        client.headers["Authorization"] = "Bearer " + settings.token
        response = client.get("/api/v1/research/strategies")
        assert response.status_code == 200
        assert len(response.json()) == 2
    engine.dispose()
    with pytest.raises(ValueError, match="Missing"):
        PluginHost(tuple(p for p in builtin_plugins() if p.id != "asterion.strategy_sma"))


def test_request_requires_explicit_strategy_and_parameters():
    value = payload()["request"] | {"command_id": "explicit"}
    for field in ("strategy", "parameters"):
        with pytest.raises(ValueError):
            BacktestRequest.model_validate({k: v for k, v in value.items() if k != field})
