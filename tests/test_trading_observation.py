import threading
import time
from decimal import Decimal
from types import SimpleNamespace

import pytest
from connection_fakes import access, manager, save_body

from asterion.connector_ctp.normalize import observation
from asterion.trading.observation import AccountService


def account(**changes):
    return {
        "AccountID": "fixture",
        "CurrencyID": "CNY",
        "TradingDay": "20260922",
        "Balance": 100000.0,
        "Available": 80000.0,
        "CurrMargin": 20000.0,
        "PositionProfit": 123.5,
        "CloseProfit": 30.0,
        "Commission": 5.0,
        "FrozenMargin": 0.0,
        "FrozenCommission": 0.0,
    } | changes


def position(**changes):
    return {
        "ExchangeID": "SHFE",
        "InstrumentID": "au2612",
        "PosiDirection": "2",
        "HedgeFlag": "1",
        "PositionDate": "1",
        "Position": 2,
        "TodayPosition": 2,
        "PositionProfit": 100.0,
        "UseMargin": 10000.0,
        "TradingDay": "20260922",
    } | changes


def raw(rows=None, **changes):
    return {"accounts": [account(**changes)], "positions": [position()] if rows is None else rows}


def test_position_grouping_preserves_direction_hedge_today_yesterday():
    summary, positions = observation(
        raw(
            [
                position(),
                position(PositionDate="2", Position=3, TodayPosition=0),
                position(PosiDirection="3"),
                position(HedgeFlag="3"),
            ]
        ),
    )
    assert summary.balance == Decimal(100000)
    assert len(positions) == 3
    assert (positions[0].quantity, positions[0].today, positions[0].yesterday) == (5, 2, 3)
    assert positions[0].name is None and positions[0].margin == Decimal(20000)
    assert positions[1].direction == "short" and positions[2].hedge == "3"


@pytest.mark.parametrize(
    "rows",
    [
        [position(), position()],
        [position(TradingDay="20260921")],
        [position(TodayPosition=3)],
        [position(PosiDirection="1")],
        [position(PositionDate="9")],
    ],
)
def test_invalid_or_duplicate_response_rejected(rows):
    with pytest.raises(ValueError):
        observation(raw(rows))


def test_complete_empty_positions_and_unknown_financial_values():
    summary, positions = observation(raw([], Balance=float("nan"), Available=1e308))
    assert summary.balance is None and summary.available is None and positions == []
    with pytest.raises(ValueError):
        observation(raw(CurrencyID="USD"))


def test_snapshot_ratio_failure_stale_and_session(tmp_path):
    connection = manager(tmp_path)
    key = connection.save(save_body()).connection_id
    connection.connect(key)
    clock = [time.time()]
    service = AccountService(access(connection), key, lambda _: {}, lambda: clock[0])
    first = service.refresh()
    assert first.margin_ratio == 20 and not first.stale
    clock[0] += 61
    assert service.snapshot().stale
    connection.runtime[key].session.fail = True
    failed = service.refresh(force=True)
    assert failed.stale and failed.account.balance == 100 and "private" not in failed.detail
    connection.disconnect(key)
    assert service.snapshot().account is None
    connection.close()


@pytest.mark.parametrize("partial", [False, True])
def test_native_account_queries_throttled_complete_and_readonly(monkeypatch, partial):
    import openctp_ctp
    from test_market_contracts import native_configuration

    from asterion.connector_ctp.query import query_account

    state, times = {}, []

    def auth(body, rid):
        state["spi"].OnRspAuthenticate(None, None, rid, True)
        return 0

    def login(body, rid, *args):
        state["spi"].OnRspUserLogin(None, None, rid, True)
        return 0

    def balance(body, rid):
        times.append(time.monotonic())
        assert body.CurrencyID == "CNY"
        state["spi"].OnRspQryTradingAccount(SimpleNamespace(**account()), None, rid, True)
        return 0

    def holdings(body, rid):
        times.append(time.monotonic())
        state["spi"].OnRspQryInvestorPosition(SimpleNamespace(**position()), None, rid, False)
        state["spi"].OnRspQryInvestorPosition(
            None, SimpleNamespace(ErrorID=7) if partial else None, rid, True
        )
        return 0

    api = SimpleNamespace(
        RegisterSpi=lambda spi: state.update(spi=spi),
        RegisterFront=lambda _: None,
        Init=lambda: state["spi"].OnFrontConnected(),
        ReqAuthenticate=auth,
        ReqUserLogin=login,
        ReqQryTradingAccount=balance,
        ReqQryInvestorPosition=holdings,
        Release=lambda: state.update(released=True),
    )
    monkeypatch.setattr(
        openctp_ctp,
        "tdapi",
        SimpleNamespace(
            CThostFtdcTraderApi=SimpleNamespace(CreateFtdcTraderApi=lambda _: api),
            CThostFtdcTraderSpi=object,
            CThostFtdcReqAuthenticateField=SimpleNamespace,
            CThostFtdcReqUserLoginField=SimpleNamespace,
            CThostFtdcQryTradingAccountField=SimpleNamespace,
            CThostFtdcQryInvestorPositionField=SimpleNamespace,
        ),
    )
    if partial:
        with pytest.raises(ValueError):
            query_account(native_configuration(), "never-log-this", threading.Event())
    else:
        result = query_account(native_configuration(), "never-log-this", threading.Event())
        assert result == raw()
    assert state["released"]
    assert times[1] - times[0] >= 1.1
