"""Daily cash settlement reconciles independently of close-price signals."""

from decimal import Decimal

import pytest
from asterion_bindings.execution import ExecutionFactory
from test_research import payload

from asterion.distribution import strategy_catalog
from asterion.research.engine import calculate


def test_cash_settlement_and_close_valuation_reconcile_across_open_hold_close():
    value = payload()
    for bar, settle in zip(value["bars"], [10, 11, 19, 20, 16], strict=True):
        bar["settle"] = str(settle)
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert [Decimal(p["balance"]) for p in result["curve"]] == [1000, 1000, 978, 988, 946]
    assert [Decimal(p["settlement_pnl"]) for p in result["curve"]] == [0, 0, -20, 10, -40]
    assert [Decimal(p["close_equity"]) for p in result["curve"]] == [1000, 1000, 998, 968, 946]
    assert [Decimal(p["close_pnl"]) for p in result["curve"]] == [0, 0, 20, -20, 0]
    for p in result["curve"]:
        assert Decimal(p["balance"]) == (
            Decimal(p["opening_balance"]) + Decimal(p["settlement_pnl"]) - Decimal(p["fees"])
        )
        assert Decimal(p["free_cash"]) + Decimal(p["margin"]) == Decimal(p["balance"])
    assert result["fills"] == calculate(payload(), strategy_catalog(), ExecutionFactory())["fills"]
    assert result["summary"]["final_equity"] == "946"
    assert Decimal(result["summary"]["max_drawdown"]) == Decimal("0.054")


def test_last_open_position_uses_settlement_and_not_close_for_summary():
    value = payload()
    value["bars"] = value["bars"][:3]
    value["bars"][-1]["settle"] = "19"
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert result["summary"]["final_equity"] == "978"
    assert result["curve"][-1]["close_equity"] == "998"
    assert result["curve"][-1]["margin"] == "19.0"
    assert result["summary"]["open_lots"] == 1


def test_settlement_margin_deficit_forces_next_open_even_if_close_is_high():
    value = payload()
    value["request"]["capital"] = "50"
    value["bars"][2]["settle"] = "17"
    value["bars"][2]["close"] = "100"
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert result["curve"][2]["balance"] == "8"
    assert result["curve"][2]["close_equity"] == "838"
    assert result["fills"][1]["day"] == "2024-01-04"
    assert result["fills"][1]["reason"] == "保证金不足平仓"


@pytest.mark.parametrize("bad", [None, "", "NaN", "Infinity", "0", "-1", "1e13", "0.123456789"])
def test_invalid_settlement_is_never_replaced_by_close(bad):
    value = payload()
    value["bars"][0]["settle"] = bad
    with pytest.raises(ValueError, match="有效结算价"):
        calculate(value, strategy_catalog(), ExecutionFactory())


def test_missing_settlement_is_rejected():
    value = payload()
    del value["bars"][0]["settle"]
    with pytest.raises(ValueError, match="有效结算价"):
        calculate(value, strategy_catalog(), ExecutionFactory())
