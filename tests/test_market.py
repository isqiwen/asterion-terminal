import time
from datetime import datetime
from zoneinfo import ZoneInfo

import pytest
from connection_fakes import access, instrument, manager, save_body

from asterion.connections.public import QuoteEvent
from asterion.market.contracts import ContractChoicesService
from asterion.market.models import Watchlist
from asterion.market.service import MarketService


def market(root, connection, key, clock=time.time):
    directory = ContractChoicesService(
        root / key, lambda cancel: connection.instruments(key, cancel)
    )
    service = MarketService(root / key, key, access(connection), directory, clock)
    connection.listen(lambda cid, g, k, v: service.event(g, k, v) if cid == key else None)
    connection.connect(key)
    service.refresh_contracts()
    directory.thread.join(5)
    return service


def test_watchlist_quotes_connection_isolation_and_restart(tmp_path):
    connection = manager(tmp_path)
    a = connection.save(save_body()).connection_id
    b = connection.save(
        save_body(name="第二账户", config={"user": "other", "endpoint": "server"})
    ).connection_id
    first = market(tmp_path, connection, a)
    second = market(tmp_path, connection, b)
    first.watchlist(Watchlist(subscriptions=[{"exchange": "SHFE", "symbol": "au2612"}]))
    connection.connect(a)
    now = time.time()
    tick = QuoteEvent(
        exchange="SHFE",
        symbol="au2612",
        last=100,
        previous_settlement=80,
        high=110,
        low=90,
        volume=10,
        open_interest=20,
        trading_day="20260922",
        action_day="20260922",
        source_time="09:30:00",
        event_at=now,
        received_at=now,
    )
    connection.runtime[a].session.emit("tick", tick)
    assert first.snapshot().quotes[0].change_percent == 25
    assert not second.snapshot().quotes and not second.configuration.subscriptions
    first.watchlist(Watchlist(subscriptions=[]))
    assert not first.snapshot().quotes and not connection.runtime[a].session.subscriptions
    restored = market(tmp_path, connection, a)
    assert not restored.configuration.subscriptions
    connection.close()


@pytest.mark.parametrize(
    "delta,event_delta,expected",
    [
        (0, 0, "current"),
        (31, 0, "not_updated"),
        (0, -31, "delayed"),
        (0, 10, "time_ahead"),
        (0, None, "time_unknown"),
    ],
)
def test_quote_status(tmp_path, delta, event_delta, expected):
    connection = manager(tmp_path)
    key = connection.save(save_body()).connection_id
    now = time.time()
    service = market(tmp_path, connection, key, lambda: now)
    service.watchlist(Watchlist(subscriptions=[{"exchange": "SHFE", "symbol": "au2612"}]))
    connection.connect(key)
    tick = QuoteEvent(
        exchange="SHFE",
        symbol="au2612",
        last=100,
        previous_settlement=80,
        high=110,
        low=90,
        volume=1,
        open_interest=2,
        trading_day="20260922",
        action_day="20260922",
        source_time="09:00:00",
        event_at=None if event_delta is None else now + event_delta,
        received_at=now - delta,
    )
    connection.runtime[key].session.emit("tick", tick)
    assert service.snapshot().quotes[0].status == expected
    connection.disconnect(key)
    assert service.snapshot().quotes[0].status == "disconnected"
    connection.close()


def test_expiry_background_removes_only_confirmed_contracts(tmp_path):
    connection = manager(tmp_path)
    key = connection.save(save_body()).connection_id
    service = market(tmp_path, connection, key)
    now = datetime(2026, 9, 22, 12, tzinfo=ZoneInfo("Asia/Shanghai"))
    service.directory.clock = lambda: now
    connection.runtime[key].session.rows = [instrument(last_trade_on="2026-09-22")]
    service.refresh_contracts()
    service.directory.thread.join(5)
    service.watchlist(Watchlist(subscriptions=[{"exchange": "SHFE", "symbol": "au2612"}]))
    service.expire_contracts()
    assert service.configuration.subscriptions
    now = now.replace(day=23)
    service.expire_contracts()
    assert not service.configuration.subscriptions
    connection.close()


def test_ctp_adapter_login_resubscribes_and_shutdown_releases(monkeypatch):
    from types import SimpleNamespace

    import openctp_ctp

    from asterion.connections.public import Subscription
    from asterion.connector_ctp.feed import CtpFeed

    calls = []
    api = SimpleNamespace(
        RegisterSpi=lambda spi: calls.append(("spi", spi)),
        RegisterFront=lambda front: calls.append(("front", front)),
        Init=lambda: calls.append(("init",)),
        ReqUserLogin=lambda body, request_id: (
            calls.append(("login", body.BrokerID, body.UserID)) or 0
        ),
        SubscribeMarketData=lambda symbols, count: calls.append(("subscribe", symbols, count)) or 0,
        UnSubscribeMarketData=lambda symbols, count: (
            calls.append(("unsubscribe", symbols, count)) or 0
        ),
        Release=lambda: calls.append(("release",)),
    )
    monkeypatch.setattr(
        openctp_ctp,
        "mdapi",
        SimpleNamespace(
            CThostFtdcMdSpi=object,
            CThostFtdcMdApi=SimpleNamespace(CreateFtdcMdApi=lambda path: api),
            CThostFtdcReqUserLoginField=SimpleNamespace,
        ),
    )
    events = []
    config = SimpleNamespace(
        front="tcp://fixture:1234",
        broker_id="9999",
        user_id="123456",
        subscriptions=[Subscription(exchange="SHFE", symbol="au2612")],
    )
    config.subscriptions.append(Subscription(exchange="DCE", symbol="m2701"))
    feed = CtpFeed(config, "secret", lambda *args: events.append(args))
    feed.start()
    feed.spi.OnFrontConnected()
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=0), 1, True)
    assert ("login", "9999", "123456") in calls
    assert ("subscribe", [b"au2612", b"m2701"], 2) in calls
    feed.update_subscriptions([Subscription(exchange="DCE", symbol="m2701")])
    assert ("unsubscribe", [b"au2612"], 1) in calls
    assert calls[-1] == ("subscribe", [b"m2701"], 1)
    feed.spi.OnFrontDisconnected(1)
    feed.update_subscriptions([Subscription(exchange="SHFE", symbol="ag2612")])
    feed.spi.OnFrontConnected()
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=0), 1, True)
    assert sum(c[0] == "subscribe" for c in calls) == 3
    assert calls[-1] == ("subscribe", [b"ag2612"], 1)
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=3), 1, True)
    assert events[-1][0] == "error"
    assert "secret" not in str(events)
    feed.spi.OnRspUnSubMarketData(None, SimpleNamespace(ErrorID=7), 1, True)
    assert events[-1][0] == "subscription_warning"
    api.UnSubscribeMarketData = lambda symbols, count: 9
    api.SubscribeMarketData = lambda symbols, count: 8
    feed.update_subscriptions([Subscription(exchange="DCE", symbol="m2701")])
    assert "退订请求未发送" in events[-2][1]
    assert "订阅请求未发送" in events[-1][1]
    feed.close()
    assert feed.password == "" and calls[-1] == ("release",)


@pytest.mark.parametrize(
    "base,last,amount,percent",
    [
        (100, 101.2, 1.2, 1.2),
        (100, 99.5, -0.5, -0.5),
        (100, 100, 0, 0),
        (None, 101, None, None),
        (0, 101, None, None),
        (-1, 101, None, None),
        (100, None, None, None),
    ],
)
def test_quote_change_uses_only_previous_settlement(tmp_path, base, last, amount, percent):
    connection = manager(tmp_path)
    key = connection.save(save_body()).connection_id
    service = market(tmp_path, connection, key)
    service.watchlist(Watchlist(subscriptions=[{"exchange": "SHFE", "symbol": "au2612"}]))
    tick = QuoteEvent(
        exchange="SHFE",
        symbol="au2612",
        last=last,
        previous_settlement=base,
        high=110,
        low=90,
        volume=10,
        open_interest=20,
        trading_day="20260922",
        action_day="20260922",
        source_time="09:30:00",
        event_at=time.time(),
        received_at=time.time(),
    )
    connection.runtime[key].session.emit("tick", tick)
    result = service.snapshot().quotes[0]
    assert result.change == amount and result.change_percent == percent
    assert result.high == 110 and result.low == 90
    connection.close()


def test_ctp_quote_normalization_retains_source_extremes_and_missing_reference():
    from asterion.connector_ctp.normalize import quote

    raw = {
        "ExchangeID": "SHFE",
        "InstrumentID": "au2612",
        "LastPrice": 101,
        "PreSettlementPrice": 1.7976931348623157e308,
        "PreClosePrice": 99,
        "HighestPrice": 105,
        "LowestPrice": 98,
        "Volume": 1,
        "OpenInterest": 10,
        "TradingDay": "20260922",
        "ActionDay": "20260922",
        "UpdateTime": "09:30:00",
        "UpdateMillisec": 0,
    }
    result = quote(raw)
    assert result.previous_settlement is None
    assert result.high == 105 and result.low == 98
    raw.update(HighestPrice=1.7976931348623157e308, LowestPrice=float("nan"))
    result = quote(raw)
    assert result.high is None and result.low is None
