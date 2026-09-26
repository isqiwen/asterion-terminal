"""The Rust account owns amounts and obeys both plugin and task lifetimes."""

import json
from decimal import ROUND_UP, localcontext
from pathlib import Path

import pytest
from asterion_bindings import _native
from asterion_bindings.execution import ExecutionConfig, ExecutionFactory
from asterion_bindings.plugin_host import Activation, Plugin, PluginHost
from asterion_bindings.tasks import ExecutionContext
from fastapi import FastAPI

from asterion.distribution import strategy_catalog
from asterion.platform.serialization import canonical
from asterion.research.engine import calculate
from asterion.research.execution import EXECUTION


def fixture():
    path = Path(__file__).parents[1] / "domain/execution/tests/fixtures/daily.json"
    value = json.loads(path.read_text())
    case = value["cases"][0]
    case["config"]["rules"]["spec"]["trading_time"] = value["trading_time"]
    return case


def test_current_account_amounts_are_independent_of_python_decimal_context():
    case = fixture()
    with ExecutionFactory() as owner, localcontext() as context:
        context.prec, context.rounding = 2, ROUND_UP
        with owner.create(case["config"]) as account:
            for bar, intent in zip(case["bars"], case["intents"], strict=True):
                account.begin_day(bar)
                account.close_intent(intent)
            assert canonical(account.finish()) == canonical(case["result"])
            with pytest.raises(ValueError):
                account.finish()


def test_factory_close_cancels_every_outstanding_account():
    case = fixture()
    owner = ExecutionFactory()
    account = owner.create(case["config"])
    account.begin_day(case["bars"][0])
    owner.close()
    with pytest.raises(ValueError):
        account.close_intent(True)
    with pytest.raises(ValueError):
        account.finish()
    with pytest.raises(ValueError):
        owner.create(case["config"])


def test_task_scope_revokes_session_and_captured_factory_method():
    case = fixture()
    with ExecutionFactory() as owner:
        context = ExecutionContext((EXECUTION,), {EXECUTION: owner})
        create = context.resource(EXECUTION).create
        session = create(case["config"])
        session.begin_day(case["bars"][0])
        context.close()
        with pytest.raises(ValueError):
            session.close_intent(True)
        with pytest.raises(ValueError):
            create(case["config"])
        session.close()


def test_plugin_scope_revokes_session_independently_of_factory_lifetime():
    case = fixture()
    captured = []

    def activate(context):
        captured.append(context.resource(EXECUTION).create(case["config"]))
        return Activation()

    plugin = Plugin("fixture.research", (), activate, resources=(EXECUTION,))
    with ExecutionFactory() as owner:
        host = PluginHost((plugin,))
        host.activate(FastAPI(), {plugin.id: {EXECUTION: owner}})
        account = captured[0]
        account.begin_day(case["bars"][0])
        host.close()
        with pytest.raises(ValueError):
            account.close_intent(True)
        account.close()
        # The separately owned factory remains usable outside the revoked grant.
        with owner.create(case["config"]):
            pass


def test_invalid_bar_poisoning_and_native_contract_are_enforced():
    case = fixture()
    with ExecutionFactory() as owner:
        with owner.create(case["config"]) as account:
            with pytest.raises(ValueError, match="结算价"):
                account.begin_day(case["bars"][0] | {"settle": None})
            with pytest.raises(ValueError):
                account.begin_day(case["bars"][0])
        native = owner._native.create(json.dumps(case["config"]), [])
        native.begin_day(json.dumps(case["bars"][0]))
        with pytest.raises(TypeError):
            native.close_intent(1)
        native.close()
    with pytest.raises(ValueError):
        ExecutionConfig.model_validate(case["config"] | {"lots": 0})
    with pytest.raises(ValueError):
        _native.invoke(
            "execution", "validate", '{"model":"ExecutionConfig","model":"DailyBar","value":{}}'
        )


def test_strategy_failure_does_not_publish_a_partial_account_result():
    from test_research import payload

    from asterion.research.strategies import StrategyCatalog

    class Session:
        def on_close(self, bar):
            raise ValueError("strategy failed")

        def close(self):
            pass

    with strategy_catalog() as catalog, ExecutionFactory() as owner:
        original = catalog.resolve(catalog.list()[0].identity)
        from dataclasses import replace

        broken = replace(original, create=lambda parameters: Session())
        selected = StrategyCatalog((broken,))
        with pytest.raises(ValueError, match="strategy failed"):
            calculate(payload(), selected, owner)
